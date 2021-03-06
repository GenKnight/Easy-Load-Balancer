#include <set>
#include <stdio.h>
#include <netdb.h>
#include <assert.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "log.h"
#include "Server.h"
#include "RouteLb.h"
#include "easy_reactor.h"

struct LbConfigure
{
    int initSuccCnt;   //初始的（虚拟）成功个数，防止刚启动时少量失败就认为过载
    int ovldErrCnt;    //被判定为overload时（虚拟）失败个数
    int continSuccLim; //当overload节点连续成功次数超过此值，认为成功恢复
    int continErrLim;  //当正常节点连续失败次数超过此值，认为overload
    int probeNum;      //经过几次获取节点请求后，试探选择一次overload节点
    long updateTimo;   //对于每个modid/cmdid，多久更新一下本地路由,s
    long clearTimo;    //对于某个modid/cmdid下的某个host，多久清理一下负载信息
    long reportTimo;   //每个modid/cmdid的上报周期,多久上报给reporter一次
    long ovldWaitLim;  //对于某个modid/cmdid下的某个host被判断过载后，在过载队列等待的最大时间,s
    float succRate;    //当overload节点（虚拟）成功率高于此值，节点变idle
    float errRate;     //当idle节点（虚拟）失败率高于此值，节点变overload
    float windErrRate; //整个窗口的真实失败率阈值
    int windErrLim;    //连续N个窗口真实失败率高于windErrRate, 如果N>=windErrLim，强行认为节点过载
} LbConfig;

static uint32_t MyIp = 0;

static pthread_once_t onceLoad = PTHREAD_ONCE_INIT;

static void initLbEnviro()
{
    //load configures
    LbConfig.initSuccCnt   = config_reader::ins()->GetNumber("lb", "init_succ_cnt", 270);
    LbConfig.ovldErrCnt    = config_reader::ins()->GetNumber("lb", "ovld_err_cnt", 5);
    LbConfig.continSuccLim = config_reader::ins()->GetNumber("lb", "contin_succ_lim", 10);
    LbConfig.continErrLim  = config_reader::ins()->GetNumber("lb", "contin_err_lim", 10);
    LbConfig.probeNum      = config_reader::ins()->GetNumber("lb", "probe_num", 10);
    LbConfig.updateTimo    = config_reader::ins()->GetNumber("lb", "update_timeout", 15);
    LbConfig.clearTimo     = config_reader::ins()->GetNumber("lb", "clear_timeout", 15);
    LbConfig.reportTimo    = config_reader::ins()->GetNumber("lb", "report_timeout", 15);
    LbConfig.ovldWaitLim   = config_reader::ins()->GetNumber("lb", "overload_wait_lim", 300);
    LbConfig.succRate      = config_reader::ins()->GetFloat("lb", "succ_rate", 0.92);
    LbConfig.errRate       = config_reader::ins()->GetFloat("lb", "err_rate", 0.1);

    LbConfig.windErrRate   = config_reader::ins()->GetFloat("lb", "wind_err_rate", 0.7);
    LbConfig.windErrLim    = config_reader::ins()->GetNumber("lb", "wind_err_limit", 2);

    //get local IP
    char myhostname[1024];
    if (::gethostname(myhostname, 1024) == 0)
    {
        struct hostent* hd = ::gethostbyname(myhostname);
        if (hd)
        {
            struct sockaddr_in myaddr;
            myaddr.sin_addr = *(struct in_addr* )hd->h_addr;
            MyIp = ntohl(myaddr.sin_addr.s_addr);
        }
    }
    if (!MyIp)
    {
        struct in_addr inaddr;
        ::inet_aton("127.0.0.1", &inaddr);
        MyIp = ntohl(inaddr.s_addr);
    }
}

/**
 * 计算整个窗口的真实失败率, 如果达到连续失败窗口值则返回true
 */
bool HI::checkWindow() {
    if (rSucc + rErr == 0) {
        windErrCnt = 0;
        return false;
    }
    //如果真实失败率>=windErrRate
    if (rErr * 1.0 / (rSucc + rErr) >= LbConfig.windErrRate) {
        windErrCnt += 1;
        return (int)windErrCnt >= LbConfig.windErrLim;
    }
    windErrCnt = 0;
    return false;
}

void HI::resetIdle(uint32_t initSucc)
{
    succ = initSucc;
    err = 0;
    rSucc = 0;
    rErr = 0;
    continSucc = 0;
    continErr = 0;
    overload = false;
    windowTs = time(NULL);//重置窗口时间
    overloadTs = 0;
    //windErrCnt = windErrCnt;保持不变即可
}

void HI::setOverload(uint32_t overloadErr)
{
    succ = 0;
    err = overloadErr;
    rSucc = 0;
    rErr = 0;
    continSucc = 0;
    continErr = 0;
    overload = true;
    overloadTs = time(NULL);//设置被判定为overload的时刻
    windErrCnt = 0;
}

LB::~LB()
{
    for (HostMapIt it = _hostMap.begin();it != _hostMap.end(); ++it)
    {
        HI* hi = it->second;
        delete hi;
    }
}

int LB::getHost(elb::GetHostRsp& rsp)
{
    if (_runningList.empty())//此[modid, cmdid]已经过载了，即所有节点都已经过载
    {
        //访问次数超过了probeNum，于是决定试探一次overload节点
        if (_accessCnt >= LbConfig.probeNum)
        {
            _accessCnt = 0;
            //选择一个overload节点
            HI* hi = _downList.front();
            elb::HostAddr* hp = rsp.mutable_host();
            hp->set_ip(hi->ip);
            hp->set_port(hi->port);
            _downList.pop_front();
            _downList.push_back(hi);
        }
        else
        {
            //明确告诉API：已经过载了
            ++_accessCnt;
            return OVERLOAD;
        }
    }
    else
    {
        if (_downList.empty())//此[modid, cmdid]完全正常
        {
            _accessCnt = 0;//重置访问次数，仅在有节点过载时才记录
            //选择一个idle节点
            HI* hi = _runningList.front();
            elb::HostAddr* hp = rsp.mutable_host();
            hp->set_ip(hi->ip);
            hp->set_port(hi->port);
            _runningList.pop_front();
            _runningList.push_back(hi);
        }
        else//有部分节点过载了
        {
            //访问次数超过了probeNum，于是决定试探一次overload节点
            if (_accessCnt >= LbConfig.probeNum)
            {
                _accessCnt = 0;
                //选择一个overload节点
                HI* hi = _downList.front();
                elb::HostAddr* hp = rsp.mutable_host();
                hp->set_ip(hi->ip);
                hp->set_port(hi->port);
                _downList.pop_front();
                _downList.push_back(hi);
            }
            else
            {
                ++_accessCnt;
                //选择一个idle节点
                HI* hi = _runningList.front();
                elb::HostAddr* hp = rsp.mutable_host();
                hp->set_ip(hi->ip);
                hp->set_port(hi->port);
                _runningList.pop_front();
                _runningList.push_back(hi);
            }
        }
    }
    return SUCCESS;
}

void LB::getRoute(std::vector<HI*>& vec)
{
    for (HostMapIt it = _hostMap.begin();it != _hostMap.end(); ++it)
    {
        HI* hi = it->second;
        vec.push_back(hi);
    }
}

void LB::persist(FILE* fp)
{
    for (HostMapIt it = _hostMap.begin();it != _hostMap.end(); ++it)
    {
        HI* hi = it->second;
        fprintf(fp, "%d %d %d %d\n", _modid, _cmdid, hi->ip, hi->port);
    }
}

void LB::report(int ip, int port, int retcode)
{
    uint64_t key = ((uint64_t)ip << 32) + port;
    if (_hostMap.find(key) == _hostMap.end())
        return ;
    HI* hi = _hostMap[key];
    if (retcode == 0)
    {
        //更新虚拟成功、真实成功次数
        hi->succ += 1;
        hi->rSucc++;
        //连续成功、失败个数更新
        hi->continSucc++;
        hi->continErr = 0;
    }
    else
    {
        //更新虚拟失败、真实失败次数
        hi->err += 1;
        hi->rErr++;
        //连续成功、失败个数更新
        hi->continSucc = 0;
        hi->continErr++;
    }
    long currenTs = time(NULL);
    //检查idle节点是否满足overload条件;或者overload节点是否满足idle条件
    //如果是idle节点，则只有调用失败才有必要判断是否达到overload条件
    if (!hi->overload && retcode != 0)
    {
        bool ifOverload = false;
        //idle节点,检查是否达到判定为overload状态的条件
        //[1].计算失败率,如果大于预设失败率，则认为overload
        double errRate = hi->err * 1.0 / (hi->succ + hi->err);
        if (errRate > LbConfig.errRate)
            ifOverload = true;
        //[2].连续失败次数达到阈值，认为overload
        if (!ifOverload && hi->continErr >= (uint32_t)LbConfig.continErrLim)
            ifOverload = true;
        //判定为overload了
        if (ifOverload)
        {
            struct in_addr saddr;
            saddr.s_addr = htonl(hi->ip);
            log_error("[%d, %d] host %s:%d suffer overload, succ %u err %u",
                _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->succ, hi->err);
            //重置hi为overload状态
            hi->setOverload(LbConfig.ovldErrCnt);
            //移除出runingList,放入downList
            _runningList.remove(hi);
            _downList.push_back(hi);
            return ;
        }
    }
    else if (hi->overload && retcode == 0)//如果是overload节点，则只有调用成功才有必要判断是否达到idle条件
    {
        bool ifIdle = false;
        //overload节点,检查是否达到回到idle状态的条件
        //[1].计算成功率,如果大于预设成功率，则认为idle
        double succRate = hi->succ * 1.0 / (hi->succ + hi->err);
        if (succRate > LbConfig.succRate)
            ifIdle = true;
        //[2].连续成功次数达到阈值，认为idle
        if (!ifIdle && hi->continSucc >= (uint32_t)LbConfig.continSuccLim)
            ifIdle = true;
        //判定为idle了
        if (ifIdle)
        {
            struct in_addr saddr;
            saddr.s_addr = htonl(hi->ip);
            log_error("[%d, %d] host %s:%d recover to idle, succ %u err %u",
                _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->succ, hi->err);
            //重置hi为idle状态
            hi->resetIdle(LbConfig.initSuccCnt);
            //移除出downList,重新放入runingList            
            _downList.remove(hi);
            _runningList.push_back(hi);
            return ;
        }
    }

    //检查idle时间窗口是否到达，若到达，重置;或者检查overload节点是否处于overload状态的时长已经超时了
    if (!hi->overload)
    {
        //节点是idle状态，检查是否时间窗口到达
        if (currenTs - hi->windowTs >= LbConfig.clearTimo)
        {
            //时间窗口到达
            if (hi->checkWindow()) {
                //说明连续N个窗口失败率都高于M
                //将此节点打为过载
                struct in_addr saddr;
                saddr.s_addr = htonl(hi->ip);
                log_error("[%d, %d] host %s:%d suffer overload because of continuous windows error rate too high, real succ %u real err %u",
                    _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->rSucc, hi->rErr);
                //重置hi为overload状态
                hi->setOverload(LbConfig.ovldErrCnt);
                //移除出runingList,放入downList
                _runningList.remove(hi);
                _downList.push_back(hi);
            }
            else {
                //新窗口
                hi->resetIdle(LbConfig.initSuccCnt);
            }
        }
    }
    else
    {
        //检查overload节点是否处于overload状态的时长已经超时了
        if (currenTs - hi->overloadTs >= LbConfig.ovldWaitLim)
        {
            struct in_addr saddr;
            saddr.s_addr = htonl(hi->ip);
            log_error("[%d, %d] host %s:%d recover to idle, succ %u err %u",
                _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->succ, hi->err);
            //处于overload状态的时长已经超时了
            hi->resetIdle(LbConfig.initSuccCnt);
            //重新把节点放入runningList
            _downList.remove(hi);
            _runningList.push_back(hi);
        }
    }
}

void LB::reportSomeSucc(int ip, int port, unsigned succCnt)
{
    uint64_t key = ((uint64_t)ip << 32) + port;
    if (_hostMap.find(key) == _hostMap.end())
        return ;
    HI* hi = _hostMap[key];
    //更新真实成功次数
    hi->rSucc += succCnt;
    //更新连续成功、连续失败个数
    hi->continSucc += succCnt;
    hi->continErr = 0;
    long currenTs = time(NULL);
    //如果是idle节点，则在调用状态上报后，肯定还是idle，仅需更新虚拟成功个数、连续成功个数
    if (!hi->overload)
    {
        hi->succ += succCnt;
    }
    else
    {
        //否则，检查此overload节点是否满足回到idle的条件
        bool ifIdle = false;
        unsigned leftSuccCnt = succCnt;
        //[1].连续成功次数达到阈值，认为idle
        if (hi->continSucc >= (uint32_t)LbConfig.continSuccLim)
        {
            leftSuccCnt = hi->continSucc - LbConfig.continSuccLim;
            ifIdle = true;
        }
        //[2].计算成功率,如果大于预设成功率，则认为idle
        if (!ifIdle)
        {
            unsigned needSucc = LbConfig.succRate * hi->err / (1 - LbConfig.succRate);
            if (needSucc * 1.0 / (needSucc + hi->err) < LbConfig.succRate)
                ++needSucc;
            //如果成功个数超过所需成功个数，则认为idle
            if (succCnt >= needSucc)
            {
                leftSuccCnt = succCnt - needSucc;
                ifIdle = true;
            }
        }
        hi->succ += succCnt - leftSuccCnt;
        //判定为idle了
        if (ifIdle)
        {
            struct in_addr saddr;
            saddr.s_addr = htonl(hi->ip);
            log_error("after report some, [%d, %d] host %s:%d recover to idle, succ %u err %u",
                _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->succ, hi->err);
            //重置hi为idle状态
            hi->resetIdle(LbConfig.initSuccCnt);
            //移除出downList,重新放入runingList
            _downList.remove(hi);
            _runningList.push_back(hi);
        }
        //继续把剩下的成功个数加上
        hi->succ += leftSuccCnt;
    }

    //检查idle时间窗口是否到达，若到达，重置;或者检查overload节点是否处于overload状态的时长已经超时了
    if (!hi->overload)
    {
        //节点是idle状态，检查是否时间窗口到达
        if (currenTs - hi->windowTs >= LbConfig.clearTimo)
        {
            //时间窗口到达
            if (hi->checkWindow()) {
                //说明连续N个窗口失败率都高于M
                //将此节点打为过载
                struct in_addr saddr;
                saddr.s_addr = htonl(hi->ip);
                log_error("[%d, %d] host %s:%d suffer overload because of continuous windows error rate too high, real succ %u real err %u",
                    _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->rSucc, hi->rErr);
                //重置hi为overload状态
                hi->setOverload(LbConfig.ovldErrCnt);
                //移除出runingList,放入downList
                _runningList.remove(hi);
                _downList.push_back(hi);
            }
            else {
                //新窗口
                hi->resetIdle(LbConfig.initSuccCnt);
            }
        }
    }
    else
    {
        //检查overload节点是否处于overload状态的时长已经超时了
        if (currenTs - hi->overloadTs >= LbConfig.ovldWaitLim)
        {
            struct in_addr saddr;
            saddr.s_addr = htonl(hi->ip);
            log_error("[%d, %d] host %s:%d recover to idle, succ %u err %u",
                _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->succ, hi->err);
            //处于overload状态的时长已经超时了
            hi->resetIdle(LbConfig.initSuccCnt);
            //重新把节点放入runningList
            _downList.remove(hi);
            _runningList.push_back(hi);
        }
    }
}

void LB::reportSomeErr(int ip, int port, unsigned errCnt)
{
    printf("DEBUG: tell error count is %u\n", errCnt);
    uint64_t key = ((uint64_t)ip << 32) + port;
    if (_hostMap.find(key) == _hostMap.end())
        return ;
    HI* hi = _hostMap[key];
    //更新真实失败次数
    hi->rErr += errCnt;
    //更新连续成功、连续失败个数
    hi->continSucc = 0;
    hi->continErr += errCnt;
    long currenTs = time(NULL);
    //如果是overload节点，则在调用状态上报后，肯定还是overload，仅需更新虚拟失败个数
    if (hi->overload)
    {
        hi->err += errCnt;
    }
    else
    {
        //否则，检查此idle节点是否满足回到overload的条件
        bool ifOverload = false;
        unsigned leftErrCnt = errCnt;
        //[1].连续失败次数达到阈值，认为overload
        if (hi->continErr >= (uint32_t)LbConfig.continErrLim)
        {
            leftErrCnt = hi->continErr - LbConfig.continErrLim;
            ifOverload = true;
        }
        //[2].计算失败率,如果大于预设失败率，则认为overload
        if (!ifOverload)
        {
            unsigned needErr = LbConfig.errRate * hi->succ / (1 - LbConfig.errRate);
            if (needErr * 1.0 / (needErr + hi->succ) < LbConfig.errRate)
                ++needErr;
            //如果失败个数超过所需失败个数，则认为idle
            if (errCnt >= needErr)
            {
                leftErrCnt = errCnt - needErr;
                ifOverload = true;
            }
        }
        hi->err += errCnt - leftErrCnt;
        //判定为overload了
        if (ifOverload)
        {
            struct in_addr saddr;
            saddr.s_addr = htonl(hi->ip);
            log_error("[%d, %d] host %s:%d suffer overload, succ %u err %u",
                _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->succ, hi->err);
            //重置hi为overload状态
            hi->setOverload(LbConfig.ovldErrCnt);
            //移除出runingList,放入downList
            _runningList.remove(hi);
            _downList.push_back(hi);
        }
        hi->err += leftErrCnt;
    }

    //检查idle时间窗口是否到达，若到达，重置;或者检查overload节点是否处于overload状态的时长已经超时了
    if (!hi->overload)
    {
        //节点是idle状态，检查是否时间窗口到达
        if (currenTs - hi->windowTs >= LbConfig.clearTimo)
        {
            //时间窗口到达
            if (hi->checkWindow()) {
                //说明连续N个窗口失败率都高于M
                //将此节点打为过载
                struct in_addr saddr;
                saddr.s_addr = htonl(hi->ip);
                log_error("[%d, %d] host %s:%d suffer overload because of continuous windows error rate too high, real succ %u real err %u",
                    _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->rSucc, hi->rErr);
                //重置hi为overload状态
                hi->setOverload(LbConfig.ovldErrCnt);
                //移除出runingList,放入downList
                _runningList.remove(hi);
                _downList.push_back(hi);
            }
            else {
                //新窗口
                hi->resetIdle(LbConfig.initSuccCnt);
            }
        }
    }
    else
    {
        //检查overload节点是否处于overload状态的时长已经超时了
        if (currenTs - hi->overloadTs >= LbConfig.ovldWaitLim)
        {
            struct in_addr saddr;
            saddr.s_addr = htonl(hi->ip);
            log_error("[%d, %d] host %s:%d recover to idle, succ %u err %u",
                _modid, _cmdid, ::inet_ntoa(saddr), hi->port, hi->succ, hi->err);
            //处于overload状态的时长已经超时了
            hi->resetIdle(LbConfig.initSuccCnt);
            //重新把节点放入runningList
            _downList.remove(hi);
            _runningList.push_back(hi);
        }
    }
}

void LB::update(elb::GetRouteRsp& rsp)
{
    assert(rsp.hosts_size() != 0);
    //如果是第一次拉过来，则重置lstRptTime，防止第一次api上报状态就触发lbagent给reporter上报
    bool resetRptTime = _hostMap.empty();
    bool updated = false;
    long currenTs = time(NULL);
    //hosts who need to delete
    std::set<uint64_t> remote;
    std::set<uint64_t> todel;
    for (int i = 0;i < rsp.hosts_size(); ++i)
    {
        const elb::HostAddr& h = rsp.hosts(i);
        uint64_t key = ((uint64_t)h.ip() << 32) + h.port();
        remote.insert(key);

        if (_hostMap.find(key) == _hostMap.end())
        {
            updated = true;
            //it is new
            HI* hi = new HI(h.ip(), h.port(), LbConfig.initSuccCnt);
            if (!hi)
            {
                fprintf(stderr, "no more space to new HI\n");
                ::exit(1);
            }
            _hostMap[key] = hi;
            //add to running list
            _runningList.push_back(hi);
        }
    }
    for (HostMapIt it = _hostMap.begin();it != _hostMap.end(); ++it)
        if (remote.find(it->first) == remote.end())
            todel.insert(it->first);
    if (!todel.empty())
        updated = true;
    //delete old
    for (std::set<uint64_t>::iterator it = todel.begin();it != todel.end(); ++it)
    {
        uint64_t key = *it;
        HI* hi = _hostMap[key];

        if (hi->overload)
            _downList.remove(hi);
        else
            _runningList.remove(hi);
        _hostMap.erase(*it);
        delete hi;
    }
    //重置effectData,表明路由更新时间\有效期开始
    effectData = currenTs;
    status = ISNEW;
    if (resetRptTime)
        lstRptTime = currenTs;
    if (updated)//确实发生了路由更新，于是更新版本号
        version = currenTs;
}

void LB::pull()
{
    elb::GetRouteReq pullReq;
    pullReq.set_modid(_modid);
    pullReq.set_cmdid(_cmdid);
    pullQueue->send_msg(pullReq);
    //标记:路由正在拉取
    status = LB::ISPULLING;
}

void LB::report2Rpter()
{
    if (empty())
        return ;
    long currenTs = time(NULL);
    if (currenTs - lstRptTime < LbConfig.reportTimo)
        return ;
    lstRptTime = currenTs;

    elb::ReportStatusReq req;
    req.set_modid(_modid);
    req.set_cmdid(_cmdid);
    req.set_ts(time(NULL));
    req.set_caller(MyIp);

    for (ListIt it = _runningList.begin();it != _runningList.end(); ++it)
    {
        HI* hi = *it;
        elb::HostCallResult callRes;
        callRes.set_ip(hi->ip);
        callRes.set_port(hi->port);
        callRes.set_succ(hi->rSucc);
        callRes.set_err(hi->rErr);
        callRes.set_overload(false);
        req.add_results()->CopyFrom(callRes);
    }
    for (ListIt it = _downList.begin();it != _downList.end(); ++it)
    {
        HI* hi = *it;
        elb::HostCallResult callRes;
        callRes.set_ip(hi->ip);
        callRes.set_port(hi->port);
        callRes.set_succ(hi->rSucc);
        callRes.set_err(hi->rErr);
        callRes.set_overload(true);
        req.add_results()->CopyFrom(callRes);
    }
    reptQueue->send_msg(req);
}

RouteLB::RouteLB(int id): _id(id)
{
    ::pthread_once(&onceLoad, initLbEnviro);
    ::pthread_mutex_init(&_mutex, NULL);
}

int RouteLB::getHost(int modid, int cmdid, elb::GetHostRsp& rsp)
{
    uint64_t key = ((uint64_t)modid << 32) + cmdid;
    ::pthread_mutex_lock(&_mutex);
    if (_routeMap.find(key) != _routeMap.end())
    {
        LB* lb = _routeMap[key];
        if (lb->empty())
        {
            //说明此[modid,cmdid]正在被首次拉取且还没回来，于是直接回复不存在
            assert(lb->status == LB::ISPULLING);
            rsp.set_retcode(NOEXIST);
        }
        else
        {
            int ret = lb->getHost(rsp);
            rsp.set_retcode(ret);
            //检查是否需要重拉路由
            //若路由并没有正在拉取，且有效期至今已超时，则重拉取
            if (lb->status == LB::ISNEW && time(NULL) - lb->effectData > LbConfig.updateTimo)
            {
                lb->pull();
            }
        }
        ::pthread_mutex_unlock(&_mutex);
    }
    else
    {
        LB* lb = new LB(modid, cmdid);
        if (!lb)
        {
            fprintf(stderr, "no more space to new LB\n");
            ::exit(1);
        }
        _routeMap[key] = lb;
        //拉取一下路由
        lb->pull();
        ::pthread_mutex_unlock(&_mutex);
        rsp.set_retcode(NOEXIST);
        return NOEXIST;
    }
    return 0;
}

void RouteLB::report(elb::ReportReq& req)
{
    int modid = req.modid();
    int cmdid = req.cmdid();
    int retcode = req.retcode();
    int ip = req.host().ip();
    int port = req.host().port();
    uint32_t errcnt = 1;
    if (req.has_tcost())
    {
        assert(req.retcode() != 0);
        //100ms视为一次err
        errcnt = req.tcost() / 100;
        if (errcnt == 0)
            errcnt = 1;
        else if (errcnt > 50)
            errcnt = 50;
    }

    uint64_t key = ((uint64_t)modid << 32) + cmdid;
    ::pthread_mutex_lock(&_mutex);
    if (_routeMap.find(key) != _routeMap.end())
    {
        LB* lb = _routeMap[key];
        
        if (errcnt == 1)
            lb->report(ip, port, retcode);
        else
            lb->reportSomeErr(ip, port, errcnt);
        //try to report to reporter
        lb->report2Rpter();
    }
    ::pthread_mutex_unlock(&_mutex);
}

void RouteLB::batchReport(elb::CacheBatchRptReq& req)
{
    int modid = req.modid();
    int cmdid = req.cmdid();
    uint64_t key = ((uint64_t)modid << 32) + cmdid;
    ::pthread_mutex_lock(&_mutex);
    if (_routeMap.find(key) != _routeMap.end())
    {
        LB* lb = _routeMap[key];
        for (int i = 0;i < req.results_size(); ++i)
        {
            const elb::HostBatchCallRes& cr = req.results(i);
            int ip = cr.ip();
            int port = cr.port();
            unsigned succCnt = cr.succcnt();
            lb->reportSomeSucc(ip, port, succCnt);
        }
        //try to report to reporter
        lb->report2Rpter();
    }
    ::pthread_mutex_unlock(&_mutex);
}

void RouteLB::getRoute(int modid, int cmdid, elb::GetRouteRsp& rsp)
{
    uint64_t key = ((uint64_t)modid << 32) + cmdid;
    ::pthread_mutex_lock(&_mutex);
    if (_routeMap.find(key) != _routeMap.end())
    {
        LB* lb = _routeMap[key];
        std::vector<HI*> vec;
        lb->getRoute(vec);

        //检查是否需要重拉路由
        //若路由并没有正在拉取，且有效期至今已超时，则重拉取
        if (lb->status == LB::ISNEW && time(NULL) - lb->effectData > LbConfig.updateTimo)
        {
            lb->pull();
        }
        ::pthread_mutex_unlock(&_mutex);

        for (std::vector<HI*>::iterator it = vec.begin();it != vec.end(); ++it)
        {
            elb::HostAddr host;
            host.set_ip((*it)->ip);
            host.set_port((*it)->port);
            rsp.add_hosts()->CopyFrom(host);
        }
    }
    else
    {
        LB* lb = new LB(modid, cmdid);
        if (!lb)
        {
            fprintf(stderr, "no more space to new LB\n");
            ::exit(1);
        }
        _routeMap[key] = lb;
        //拉取一下路由
        lb->pull();
        ::pthread_mutex_unlock(&_mutex);
    }
}

void RouteLB::cacheGetRoute(int modid, int cmdid, long version, elb::CacheGetRouteRsp& rsp)
{
    uint64_t key = ((uint64_t)modid << 32) + cmdid;
    ::pthread_mutex_lock(&_mutex);
    if (_routeMap.find(key) != _routeMap.end())
    {
        LB* lb = _routeMap[key];
        if (lb->empty())
        {
            //说明此[modid,cmdid]正在被首次拉取且还没回来，于是直接回复不存在
            assert(lb->status == LB::ISPULLING);
            rsp.set_version(-1);
            ::pthread_mutex_unlock(&_mutex);
        }
        else
        {
            rsp.set_overload(lb->hasOvHost());
            rsp.set_version(lb->version);

            std::vector<HI*> vec;
            if (lb->version != version)//路由有变化，需要更新
            {
                lb->getRoute(vec);
            }
            //检查是否需要重拉路由
            //若路由并没有正在拉取，且有效期至今已超时，则重拉取
            if (lb->status == LB::ISNEW && time(NULL) - lb->effectData > LbConfig.updateTimo)
            {
                lb->pull();
            }
            ::pthread_mutex_unlock(&_mutex);

            for (std::vector<HI*>::iterator it = vec.begin();it != vec.end(); ++it)
            {
                elb::HostAddr host;
                host.set_ip((*it)->ip);
                host.set_port((*it)->port);
                rsp.add_route()->CopyFrom(host);
            }
        }
    }
    else
    {
        //no exist
        rsp.set_version(-1);
        LB* lb = new LB(modid, cmdid);
        if (!lb)
        {
            fprintf(stderr, "no more space to new LB\n");
            ::exit(1);
        }
        _routeMap[key] = lb;
        //拉取一下路由
        lb->pull();
        ::pthread_mutex_unlock(&_mutex);
    }
}

void RouteLB::update(int modid, int cmdid, elb::GetRouteRsp& rsp)
{
    uint64_t key = ((uint64_t)modid << 32) + cmdid;
    ::pthread_mutex_lock(&_mutex);
    if (_routeMap.find(key) != _routeMap.end())
    {
        LB* lb = _routeMap[key];
        if (rsp.hosts_size() == 0)
        {
            //delete this[modid,cmdid]
            delete lb;
            _routeMap.erase(key);
        }
        else
        {
            lb->update(rsp);
        }
    }
    ::pthread_mutex_unlock(&_mutex);
}

void RouteLB::clearPulling()
{
    ::pthread_mutex_lock(&_mutex);
    for (RouteMapIt it = _routeMap.begin();
        it != _routeMap.end(); ++it)
    {
        LB* lb = it->second;
        if (lb->status == LB::ISPULLING)
            lb->status = LB::ISNEW;
    }
    ::pthread_mutex_unlock(&_mutex);
}

void RouteLB::persistRoute()
{
    FILE* fp = NULL;
    switch (_id)
    {
        case 1:
            fp = fopen("/tmp/backupRoute.dat.1", "w");
            break;
        case 2:
            fp = fopen("/tmp/backupRoute.dat.2", "w");
            break;
        case 3:
            fp = fopen("/tmp/backupRoute.dat.3", "w");
    }
    if (fp)
    {
        ::pthread_mutex_lock(&_mutex);
        for (RouteMapIt it = _routeMap.begin();
            it != _routeMap.end(); ++it)
        {
            LB* lb = it->second;
            lb->persist(fp);
        }
        ::pthread_mutex_unlock(&_mutex);
        fclose(fp);
    }
}