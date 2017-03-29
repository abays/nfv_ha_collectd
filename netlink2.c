/**
 * collectd - src/netlink2.c
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Red Hat NFVPE
 **/

#include "collectd.h"

#include "common.h"
#include "plugin.h"
#include "utils_complain.h"

#include <pthread.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <netinet/in.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
//#include <netlink/object-api.h>
//#include <netlink/object.h>
//#include <netlink/socket.h>
//#include <netlink/route/addr.h>
//#include <netlink/route/rtnl.h>
//#include <netlink/route/link.h>
#include <libmnl/libmnl.h>

#define MYPROTO NETLINK_ROUTE

/*
 * Private data types
 */
struct interfacelist_s {
  char *interface;

  uint32_t status;

  struct interfacelist_s *next;
};
typedef struct interfacelist_s interfacelist_t;

/*
 * Private variables
 */
static interfacelist_t *interfacelist_head = NULL;

static int interface_thread_loop = 0;
static int interface_thread_error = 0;
static pthread_t interface_thread_id;
static pthread_mutex_t interface_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interface_cond = PTHREAD_COND_INITIALIZER;

static const char *config_keys[] = {"Interface"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

/*
 * Private functions
 */

// static int open_netlink()
// {
//     int sock = socket(AF_NETLINK,SOCK_RAW,MYPROTO);
//     struct sockaddr_nl addr;

//     memset((void *)&addr, 0, sizeof(addr));

//     if (sock<0)
//         return sock;

//     addr.nl_family = AF_NETLINK;
//     addr.nl_pid = getpid();
//     addr.nl_groups = RTMGRP_LINK|RTMGRP_IPV4_IFADDR|RTMGRP_IPV6_IFADDR;

//     if (bind(sock,(struct sockaddr *)&addr,sizeof(addr))<0)
//         return -1;

//     return sock;
// }

// static int open_netlink2(struct mnl_socket *nl)
// {
//   *nl = mnl_socket_open(NETLINK_ROUTE);
  
//   if (*nl == NULL) {
//     ERROR("netlink2 plugin: open_netlink: mnl_socket_open failed.");
//     return (-1);
//   }

//   if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
//     ERROR("netlink2 plugin: open_netlink: mnl_socket_bind failed.");
//     return (-1);
//   }

//   return (0);
// }

static int netlink_link_state(struct nlmsghdr *msg)
{
    int retval;
    struct ifinfomsg *ifi = mnl_nlmsg_get_payload(msg);
    struct nlattr *attr;
    const char *dev = NULL;

    pthread_mutex_lock(&interface_lock);

    // Get cache of interface names?
    // struct nl_cache * cache;
    // struct nl_handle * handle;

    // handle = nl_handle_alloc();

    // if (handle == NULL)
    // {
    //   char errbuf[1024];
    //   ERROR("netlink2 plugin: failed to allocate link handle: %s",
    //         sstrerror(errno, errbuf, sizeof(errbuf)));
    //   interface_thread_error = 1;
    //   retval = -1;
    // } else {

    //   cache = rtnl_link_alloc_cache(handle);

    //   if (cache == NULL)
    //   {
    //     char errbuf[1024];
    //     ERROR("netlink2 plugin: failed to allocate link cache: %s",
    //           sstrerror(errno, errbuf, sizeof(errbuf)));
    //     interface_thread_error = 1;
    //     retval = -1;
    //   } else {

    //     char namebuf[IFNAMSIZ];
    //     char *ifname = rtnl_link_i2name(cache, ifi->ifi_index, namebuf, IFNAMSIZ);

    //     interfacelist_t *il;

    //     for (il = interfacelist_head; il != NULL; il = il->next)
    //       if (strcmp(ifname, il->interface) == 0)
    //         break;

    //     if (il == NULL) 
    //       INFO("netlink2 plugin: Ignoring link state change for unmonitored interface: %s", ifname);
    //     else 
    //       il->status = ((ifi->ifi_flags & IFF_RUNNING) ? 1 : 0);

    //     sfree(cache);

    //     retval = 0;
    //   }

    //   //nl_handle_destroy(handle);
    // }

    interfacelist_t *il;

    /* Scan attribute list for device name. */
    mnl_attr_for_each(attr, msg, sizeof(*ifi)) 
    {
      if (mnl_attr_get_type(attr) != IFLA_IFNAME)
        continue;

      if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0) {
        ERROR("netlink2 plugin: netlink_link_state: IFLA_IFNAME mnl_attr_validate "
              "failed.");
        return MNL_CB_ERROR;
      }

      dev = mnl_attr_get_str(attr);

      for (il = interfacelist_head; il != NULL; il = il->next)
        if (strcmp(dev, il->interface) == 0)
          break;

      if (il == NULL) 
      {
        INFO("netlink2 plugin: Ignoring link state change for unmonitored interface: %s", dev);
        //printf("netlink2 plugin: Ignoring link state change for unmonitored interface: %s\n", dev);
      } else {
        time_t current_time;
        struct tm * time_info;
        char timeString[9];  // space for "HH:MM:SS\0"

        time(&current_time);
        time_info = localtime(&current_time);

        strftime(timeString, sizeof(timeString), "%H:%M:%S", time_info);
        
        INFO("netlink2 plugin: Interface %s status is now %s", dev, ((ifi->ifi_flags & IFF_RUNNING) ? "UP" : "DOWN"));
        printf("netlink2 plugin: (%s): Interface %s status is now %s\n", timeString, dev, ((ifi->ifi_flags & IFF_RUNNING) ? "UP" : "DOWN"));

        il->status = ((ifi->ifi_flags & IFF_RUNNING) ? 1 : 0);
      }

      // no need to loop again, we found the interface name
      // (otherwise the first if-statement in the loop would
      // have moved us on with 'continue')
      break;
    }

    pthread_mutex_unlock(&interface_lock);

    return retval;
}

static int msg_handler(struct nlmsghdr *msg)
{
    switch (msg->nlmsg_type)
    {
        case RTM_NEWADDR:
            printf("msg_handler: RTM_NEWADDR\n");
            break;
        case RTM_DELADDR:
            printf("msg_handler: RTM_DELADDR\n");
            break;
        case RTM_NEWROUTE:
            printf("msg_handler: RTM_NEWROUTE\n");
            break;
        case RTM_DELROUTE:
            printf("msg_handler: RTM_DELROUTE\n");
            break;
        case RTM_NEWLINK:
            netlink_link_state(msg);
            break;
        case RTM_DELLINK:
            printf("msg_handler: RTM_DELLINK\n");
            break;
        default:
            printf("msg_handler: Unknown netlink nlmsg_type %d\n",
                   msg->nlmsg_type);
            break;
    }
    return 0;
}

static int read_event(struct mnl_socket * nl, int (*msg_handler)(struct nlmsghdr *))
{
    int status;
    int ret = 0;
    char buf[4096];
    //struct iovec iov = { buf, sizeof buf };
    //struct sockaddr_nl snl;
    //struct msghdr msg = { (void*)&snl, sizeof snl, &iov, 1, NULL, 0, 0};
    struct nlmsghdr *h;
    // struct rtgenmsg *rt;
    // unsigned int seq;

    // //unsigned int portid = mnl_socket_get_portid(nl);

    // h = mnl_nlmsg_put_header(buf);
    // h->nlmsg_type = RTM_GETLINK;
    // h->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    // h->nlmsg_seq = seq = time(NULL);
    // rt = mnl_nlmsg_put_extra_header(h, sizeof(*rt));
    // rt->rtgen_family = AF_PACKET;

    // if (mnl_socket_sendto(nl, h, h->nlmsg_len) < 0) {
    //   FILE *f = fopen("/tmp/senderror.txt", "w");
    //   if (f == NULL)
    //   {
    //       printf("Error opening file!\n");
    //       exit(1);
    //   }

    //   /* print some text */
    //   const char *text = "netlink plugin: ir_read: rtnl_wilddump_request failed.";
    //   fprintf(f, "%s\n", text);
    //   fclose(f);

    //   ERROR("netlink plugin: ir_read: rtnl_wilddump_request failed.");
    //   return (-1);
    // }

    status = mnl_socket_recvfrom(nl, buf, sizeof(buf));

    if(status < 0)
    {
        /* Socket non-blocking so bail out once we have read everything */
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return ret;

        /* Anything else is an error */
        ERROR("read_netlink: Error mnl_socket_recvfrom: %d\n", status);
        return status;
    }
        
    if(status == 0)
    {
        printf("read_netlink: EOF\n");
    }

    // while (ret > 0) {
    //   ret = mnl_cb_run(buf, ret, seq, portid, link_filter_cb, NULL);
    //   if (ret <= MNL_CB_STOP)
    //     break;
    //   ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
    // }

    /* We need to handle more than one message per 'recvmsg' */
    for(h = (struct nlmsghdr *) buf; NLMSG_OK (h, (unsigned int)status); 
    h = NLMSG_NEXT (h, status))
    {
        /* Finish reading */
        if (h->nlmsg_type == NLMSG_DONE)
            return ret;

        /* Message is some kind of error */
        if (h->nlmsg_type == NLMSG_ERROR)
        {
            printf("read_netlink: Message is an error - decode TBD\n");
            return -1; // Error
        }

        /* Call message handler */
        if(msg_handler)
        {
            ret = (*msg_handler)(h);
            if(ret < 0)
            {
                printf("read_netlink: Message hander error %d\n", ret);
                return ret;
            }
        }
        else
        {
            printf("read_netlink: Error NULL message handler\n");
            return -1;
        }
    }

    return ret;
}

static void *interface_thread(void *arg) /* {{{ */
{
  pthread_mutex_lock(&interface_lock);

  struct mnl_socket * sock;// = mnl_socket_open(PF_NETLINK);

  // if (sock == NULL)
  // {
  //     char errbuf[1024];
  //     ERROR("netlink2 plugin: open netlink failed (socket open failed): %s",
  //           sstrerror(errno, errbuf, sizeof(errbuf)));
  //     interface_thread_error = 1;
  //     pthread_mutex_unlock(&interface_lock);
  //     return ((void *)-1);
  // }

  // int bind_status = mnl_socket_bind(sock, RTMGRP_LINK, 0);

  // if (bind_status < 0)
  // {
  //     char errbuf[1024];
  //     ERROR("netlink2 plugin: open netlink failed (socket bind failed): %s",
  //           sstrerror(errno, errbuf, sizeof(errbuf)));
  //     interface_thread_error = 1;
  //     pthread_mutex_unlock(&interface_lock);
  //     return ((void *)-1);
  // }

  sock = mnl_socket_open(NETLINK_ROUTE);   //PF_NETLINK
  if (sock == NULL) {
    ERROR("netlink2 plugin: interface_thread: mnl_socket_open failed.");
    return ((void *)-1);
  }

  // RTMGRP_LINK
  if (mnl_socket_bind(sock, RTMGRP_LINK, MNL_SOCKET_AUTOPID) < 0) {
    ERROR("netlink2 plugin: interface_thread: mnl_socket_bind failed.");
    return ((void *)-1);
  }

  while (interface_thread_loop > 0) 
  {
    int status;

    pthread_mutex_unlock(&interface_lock);

    status = read_event(sock, msg_handler);
    
    pthread_mutex_lock(&interface_lock);

    if (status < 0)
    {
      interface_thread_error = 1;
      break;
    }
    
    if (interface_thread_loop <= 0)
      break;
  } /* while (interface_thread_loop > 0) */

  mnl_socket_close(sock);
  pthread_mutex_unlock(&interface_lock);

  return ((void *)0);
} /* }}} void *interface_thread */

static int start_thread(void) /* {{{ */
{
  int status;

  pthread_mutex_lock(&interface_lock);

  if (interface_thread_loop != 0) {
    pthread_mutex_unlock(&interface_lock);
    return (0);
  }

  interface_thread_loop = 1;
  interface_thread_error = 0;
  status = plugin_thread_create(&interface_thread_id, /* attr = */ NULL, interface_thread,
                                /* arg = */ (void *)0, "netlink2");
  if (status != 0) {
    interface_thread_loop = 0;
    ERROR("netlink2 plugin: Starting thread failed.");
    pthread_mutex_unlock(&interface_lock);
    return (-1);
  }

  pthread_mutex_unlock(&interface_lock);
  return (0);
} /* }}} int start_thread */

static int stop_thread(void) /* {{{ */
{
  int status;

  pthread_mutex_lock(&interface_lock);

  if (interface_thread_loop == 0) {
    pthread_mutex_unlock(&interface_lock);
    return (-1);
  }

  interface_thread_loop = 0;
  pthread_cond_broadcast(&interface_cond);
  pthread_mutex_unlock(&interface_lock);

  status = pthread_join(interface_thread_id, /* return = */ NULL);
  if (status != 0) {
    ERROR("netlink2 plugin: Stopping thread failed.");
    status = -1;
  }

  pthread_mutex_lock(&interface_lock);
  memset(&interface_thread_id, 0, sizeof(interface_thread_id));
  interface_thread_error = 0;
  pthread_mutex_unlock(&interface_lock);

  return (status);
} /* }}} int stop_thread */

static int interface_init(void) /* {{{ */
{
  if (interfacelist_head == NULL) {
    NOTICE("netlink2 plugin: No interfaces have been configured.");
    return (-1);
  }

  return (start_thread());
} /* }}} int interface_init */

// static int config_set_string(const char *name, /* {{{ */
//                              char **var, const char *value) {
//   char *tmp;

//   tmp = strdup(value);
//   if (tmp == NULL) {
//     char errbuf[1024];
//     ERROR("netlink2 plugin: Setting `%s' to `%s' failed: strdup failed: %s", name,
//           value, sstrerror(errno, errbuf, sizeof(errbuf)));
//     return (1);
//   }

//   if (*var != NULL)
//     free(*var);
//   *var = tmp;
//   return (0);
// } /* }}} int config_set_string */

static int interface_config(const char *key, const char *value) /* {{{ */
{
  if (strcasecmp(key, "Interface") == 0) {
    interfacelist_t *il;
    char *interface;

    il = malloc(sizeof(*il));
    if (il == NULL) {
      char errbuf[1024];
      ERROR("netlink2 plugin: malloc failed: %s",
            sstrerror(errno, errbuf, sizeof(errbuf)));
      return (1);
    }

    interface = strdup(value);
    if (interface == NULL) {
      char errbuf[1024];
      sfree(il);
      ERROR("link2 plugin: strdup failed: %s",
            sstrerror(errno, errbuf, sizeof(errbuf)));
      return (1);
    }

    il->interface = interface;
    il->status = 2;
    il->next = interfacelist_head;
    interfacelist_head = il;

  } else {
    return (-1);
  }

  return (0);
} /* }}} int interface_config */

static void submit(const char *interface, const char *type, /* {{{ */
                   gauge_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &(value_t){.gauge = value};
  vl.values_len = 1;
  sstrncpy(vl.plugin, "netlink2", sizeof(vl.plugin));
  sstrncpy(vl.type_instance, interface, sizeof(vl.type_instance));
  sstrncpy(vl.type, type, sizeof(vl.type));

  INFO("Dispatching state %d for interface %s", (int) value, interface);

  plugin_dispatch_values(&vl);
} /* }}} void interface_submit */

static int interface_read(void) /* {{{ */
{
  if (interface_thread_error != 0) {
    ERROR("netlink2 plugin: The interface thread had a problem. Restarting it.");

    stop_thread();

    for (interfacelist_t *il = interfacelist_head; il != NULL; il = il->next)
      il->status = 2;

    start_thread();

    return (-1);
  } /* if (interface_thread_error != 0) */

  for (interfacelist_t *il = interfacelist_head; il != NULL; il = il->next) /* {{{ */
  {
    uint32_t status;

    /* Locking here works, because the structure of the linked list is only
     * changed during configure and shutdown. */
    pthread_mutex_lock(&interface_lock);

    status = il->status;

    pthread_mutex_unlock(&interface_lock);

    submit(il->interface, "gauge", status);
  } /* }}} for (il = interfacelist_head; il != NULL; il = il->next) */

  return (0);
} /* }}} int interface_read */

static int interface_shutdown(void) /* {{{ */
{
  interfacelist_t *il;

  INFO("netlink2 plugin: Shutting down thread.");
  if (stop_thread() < 0)
    return (-1);

  il = interfacelist_head;
  while (il != NULL) {
    interfacelist_t *il_next;

    il_next = il->next;

    sfree(il->interface);
    sfree(il);

    il = il_next;
  }

  // if (ping_data != NULL) {
  //   free(ping_data);
  //   ping_data = NULL;
  // }

  return (0);
} /* }}} int interface_shutdown */

void module_register(void) {
  plugin_register_config("netlink2", interface_config, config_keys, config_keys_num);
  plugin_register_init("netlink2", interface_init);
  plugin_register_read("netlink2", interface_read);
  plugin_register_shutdown("netlink2", interface_shutdown);
} /* void module_register */