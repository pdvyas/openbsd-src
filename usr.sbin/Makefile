#	$OpenBSD: Makefile,v 1.197 2016/11/03 16:39:28 ajacoutot Exp $

.include <bsd.own.mk>

SUBDIR=	ac accton acme-client acpidump adduser amd apm apmd arp \
	authpf bgpctl bgpd bind chroot config cron crunchgen dev_mkdb \
	dhcpd dhcrelay dvmrpctl dvmrpd edquota eeprom eigrpd eigrpctl \
	fdformat ftp-proxy gpioctl hostapd hostctl hotplugd httpd identd \
	ifstated ikectl inetd installboot iostat iscsictl iscsid kgmon \
	kvm_mkdb ldapd ldapctl ldomctl ldomd ldpd ldpctl lpr makefs \
	mailwrapper map-mbone memconfig mksuncd mkuboot mopd mrinfo \
	mrouted mtrace mtree ndp netgroup_mkdb \
	npppctl npppd nsd ntpd ospfctl ospfd ospf6d ospf6ctl \
	pcidump pkg_add portmap pppd procmap pstat pwd_mkdb \
	quot quotaon radiusctl radiusd rarpd rbootd \
	rcctl rdate rebound relayctl relayd repquota ripctl ripd \
	rmt route6d rpc.bootparamd rpc.lockd rpc.statd rtadvd \
	sa sasyncd sensorsd slowcgi smtpd \
	snmpctl snmpd spamdb switchctl switchd syslogc syslogd sysmerge \
	syspatch tcpdrop tcpdump tftp-proxy tftpd tokenadm tokeninit \
	traceroute trpt unbound usbdevs user vmd vmctl vipw watchdogd \
	wsconscfg wsfontload wsmoused zdump zic

.if (${YP:L} == "yes")
SUBDIR+=ypbind ypldap yppoll ypset ypserv
.endif

.include <bsd.subdir.mk>
