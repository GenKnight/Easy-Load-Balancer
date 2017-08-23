DROP DATABASE if exists dnsserver;
CREATE DATABASE dnsserver;
USE dnsserver;

DROP TABLE IF EXISTS `DnsServerRoute`;
CREATE TABLE `DnsServerRoute` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT,
  `modid` int(10) unsigned NOT NULL,
  `cmdid` int(10) unsigned NOT NULL,
  `serverip` int(10) unsigned NOT NULL,
  `serverport` int(10) unsigned NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=116064 DEFAULT CHARSET=utf8;

DROP TABLE IF EXISTS `ServerCallStatus`;
CREATE TABLE `ServerCallStatus` (
  `modid` int(11) NOT NULL,
  `cmdid` int(11) NOT NULL,
  `ip` int(11) NOT NULL,
  `port` int(11) NOT NULL,
  `caller` int(11) NOT NULL,
  `succ_cnt` int(11) NOT NULL,
  `err_cnt` int(11) NOT NULL,
  `ts` bigint(20) NOT NULL,
  `overload` char(1) NOT NULL,
  PRIMARY KEY (`modid`,`cmdid`,`ip`,`port`,`caller`),
  KEY `mlb_index` (`modid`,`cmdid`,`ip`,`port`,`caller`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
