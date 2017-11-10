#include "log.h"
#include "logo.h"
#include "elb.pb.h"
#include "Server.h"
#include "HeartBeat.h"
#include <pthread.h>
#include "easy_reactor.h"

thread_queue<elb::GetRouteReq>* pullQueue = NULL;
thread_queue<elb::ReportStatusReq>* reptQueue = NULL;

RouteLB* routeLB[3];

//timeout event 1: record current time in shared memory
static void recordTs(event_loop* loop, void* usrData)
{
    HeartBeat* hb = (HeartBeat*)usrData;
    hb->recordTs();
}

int main(int argc, const char** argv)
{
    if (argc != 2)
    {
        printf("USAGE: ./lbagent configPath\n");
        ::exit(1);
    }
    dispLogo();
    
    config_reader::setPath(argv[1]);

    for (int i = 0;i < 3; ++i)
    {
        int id = i + 1;
        routeLB[i] = new RouteLB(id);
        if (!routeLB[i])
        {
            fprintf(stderr, "no more space to new RouteLB\n");
            return 1;
        }
    }

    _init_log_("lbagent", ".");
    int log_level = config_reader::ins()->GetNumber("log", "level", 3);
    _set_log_level_(log_level);

    pullQueue = new thread_queue<elb::GetRouteReq>();
    if (!pullQueue)
    {
        log_error("no space to create thread_queue<elb::GetRouteReq>");
        return 1;
    }

    reptQueue = new thread_queue<elb::ReportStatusReq>();
    if (!reptQueue)
    {
        log_error("no space to create thread_queue<elb::ReportStatusReq>");
        return 1;
    }

    //init three UDP servers and create three thread for localhost[8888~8890] run in loop
    initUDPServers();
    //init connector who connects to reporter, create a thread and run in loop
    rptConnectorDomain();

    event_loop mainLoop;
    //install timeout event 1: record current time in shared memory, 1 second 1 do
    HeartBeat hb(true);
    mainLoop.run_every(recordTs, &hb, 1);
    //init connector who connects to dns server, and run in loop [main thread]
    dssConnectorDomain(mainLoop);
    return 0;
}
