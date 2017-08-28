# nfv_ha_collectd
Collectd plugins for NFV service assurance monitoring

**INSTALL**
1. Clone this repo
2. Clone https://github.com/collectd/collectd.git
3. Move collectd.conf, configure.ac and Makefile.am from nfv_ha_collectd to collectd.  If you're concerned about wiping out changes in the (probably) more-up-to-date collectd repo's configure.ac and Makefile.am files, you can manually inject the plugin-related contents from the files in this repo into the files from the collectd repo.  Just "grep -i &lt;plugin name&gt;" against this repo's files and you'll get an idea how you'll need to update the corresponding collectd repo versions.
4. Move connectivity.c to collectd/src
5. yum install -y flex bison libtool libmnl libmnl-devel gcc-c++
6. yum install -y ftp://195.220.108.108/linux/centos/7.3.1611/os/x86_64/Packages/libmnl-devel-1.0.3-7.el7.x86_64.rpm
7. yum install -y ftp://rpmfind.net/linux/centos/7.3.1611/os/x86_64/Packages/yajl-devel-2.0.4-4.el7.x86_64.rpm
8. yum install -y ftp://195.220.108.108/linux/epel/7/x86_64/l/librdkafka-0.9.5-1.el7.x86_64.rpm
9. yum install -y ftp://195.220.108.108/linux/epel/7/x86_64/l/librdkafka-devel-0.9.5-1.el7.x86_64.rpm
10. cd ~/collectd
11. ./build.sh
12. ./configure
13. make
14. make install

**CONFIGURE**
1. cd ~/collectd
2. vi collectd.conf
3. Change the "Interface" key's associated value in the connectivity plugin section to target the interface you wish to monitor.  If desired, you can simply add multiple "Interface interface" lines to monitor multiple interfaces.
4. The "Listen" key's associated values in the sysevent plugin section should be set to the IP and port where you have configured rsyslog to publish data.  See this repo's rsyslog.conf.example file for an example of rsyslog publishing to the local machine.
5. The sysevent plugin uses a ringer buffer to write and read messages it receives from rsyslog.  The "BufferSize" configuration value indicates how many maximum KBs will be stored per message (if a given message overflows, the extra data is lost).  "BufferLength" sets the length of the ring buffer.
6. Change the "Property" "metadata.broker.list" key's associated value in the write_kafka plugin section to the IP and port of your Kafka broker.  You don't necessarily have to use this plugin if you're fine with just seeing events in the syslog and collectd log.

**RUN**
1. cd ~/collectd
2. ./collectd -C collectd.conf -f
3. In another terminal, ifdown/ifup the interfaces you have chosen to monitor.  This will trigger the connectivity plugin, as well as the sysevent plugin.
4. Check /var/log/messages and /var/log/collectd.log to see the connectivity plugin report the interface status.
5. Check /var/log/messages and /var/log/collectd.log to see the sysevent plugin report syslog data received from rsyslog.
