# nfv_ha_collectd
Collectd plugin for network interface service assurance monitoring

**INSTALL**
1. Clone this repo
2. Clone https://github.com/collectd/collectd.git
3. Move collectd.conf, configure.ac and Makefile.am from nfv_ha_collectd to collectd.  If you're concerned about wiping out changes in the (probably) more-up-to-date collectd repo's configure.ac and Makefile.am files, you can manually inject the "netlink2" contents from the files in this repo into the files from the collectd repo.  Just "grep -i netlink2" against this repo's files and you'll get an idea how you'll need to update the corresponding collectd repo versions.
4. Move netlink2.c to collectd/src
5. yum install -y flex bison libtool libmnl libmnl-devel gcc-c++
6. yum install -y ftp://195.220.108.108/linux/remi/enterprise/7/remi/x86_64/librdkafka-0.9.4-1.el7.remi.x86_64.rpm
7. Clone https://github.com/edenhill/librdkafka.git
8. cd ~/librdkafka
9. ./configure
10. make
11. make install
12. cd ~/collectd
13. ./build.sh
14. ./configure
15. make
16. make install
