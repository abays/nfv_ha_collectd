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

/*
 * Private data types
 */

typedef struct interfacelist_s interfacelist_t;

typedef struct {
    uint8_t * const buffer;
    int head;
    int tail;
    const int maxLen;
} circbuf_t;

/*
 * Private variables
 */

static int sysevent_thread_loop = 0;
static int sysevent_thread_error = 0;
static pthread_t sysevent_thread_id;
static pthread_mutex_t sysevent_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sysevent_cond = PTHREAD_COND_INITIALIZER;

static const char *config_keys[] = {"Interface"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

/*
 * Private functions
 */

static void *sysevent_thread(void *arg) /* {{{ */
{
  pthread_mutex_lock(&sysevent_lock);

  while (sysevent_thread_loop > 0) 
  {
    int status;

    pthread_mutex_unlock(&sysevent_lock);

    // read here
    status = 0;
    
    pthread_mutex_lock(&sysevent_lock);

    if (status < 0)
    {
      sysevent_thread_error = 1;
      break;
    }
    
    if (sysevent_thread_loop <= 0)
      break;
  } /* while (sysevent_thread_loop > 0) */

  pthread_mutex_unlock(&sysevent_lock);

  return ((void *)0);
} /* }}} void *sysevent_thread */

static int start_thread(void) /* {{{ */
{
  int status;

  pthread_mutex_lock(&sysevent_lock);

  if (sysevent_thread_loop != 0) {
    pthread_mutex_unlock(&sysevent_lock);
    return (0);
  }

  sysevent_thread_loop = 1;
  sysevent_thread_error = 0;

  // TODO: create socket if null

  status = plugin_thread_create(&sysevent_thread_id, /* attr = */ NULL, sysevent_thread,
                                /* arg = */ (void *)0, "netlink2");
  if (status != 0) {
    sysevent_thread_loop = 0;
    ERROR("sysevent plugin: Starting thread failed.");
    pthread_mutex_unlock(&sysevent_lock);
    // TODO: close socket
    return (-1);
  }

  pthread_mutex_unlock(&sysevent_lock);
  return (0);
} /* }}} int start_thread */

static int stop_thread(int shutdown) /* {{{ */
{
  int status;

  // TODO: close socket if necessary

  pthread_mutex_lock(&sysevent_lock);

  if (sysevent_thread_loop == 0) {
    pthread_mutex_unlock(&sysevent_lock);
    return (-1);
  }

  sysevent_thread_loop = 0;
  pthread_cond_broadcast(&sysevent_cond);
  pthread_mutex_unlock(&sysevent_lock);

  if (shutdown == 1)
  {
    // Since the thread is blocking, calling pthread_join
    // doesn't actually succeed in stopping it.  It will stick around
    // until a message is received on the socket (at which 
    // it will realize that "sysevent_thread_loop" is 0 and will 
    // break out of the read loop and be allowed to die).  This is
    // fine when the process isn't supposed to be exiting, but in 
    // the case of a process shutdown, we don't want to have an
    // idle thread hanging around.  Calling pthread_cancel here in 
    // the case of a shutdown is just assures that the thread is 
    // gone and that the process has been fully terminated.

    INFO("sysevent plugin: Canceling thread for process shutdown");

    status = pthread_cancel(sysevent_thread_id);

    if (status != 0)
    {
      ERROR("sysevent plugin: Unable to cancel thread: %d", status);
      status = -1;
    }
  } else {
    status = pthread_join(sysevent_thread_id, /* return = */ NULL);
    if (status != 0) {
      ERROR("sysevent plugin: Stopping thread failed.");
      status = -1;
    }
  }

  pthread_mutex_lock(&sysevent_lock);
  memset(&sysevent_thread_id, 0, sizeof(sysevent_thread_id));
  sysevent_thread_error = 0;
  pthread_mutex_unlock(&sysevent_lock);

  INFO("sysevent plugin: Finished requesting stop of thread");

  return (status);
} /* }}} int stop_thread */

static int sysevent_init(void) /* {{{ */
{
  // TODO

  return (start_thread());
} /* }}} int sysevent_init */

static int sysevent_config(const char *key, const char *value) /* {{{ */
{
  // TODO

  return (0);
} /* }}} int sysevent_config */

// TODO
static void submit(const char *something, const char *type, /* {{{ */
                   gauge_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &(value_t){.gauge = value};
  vl.values_len = 1;
  sstrncpy(vl.plugin, "sysevent", sizeof(vl.plugin));
  sstrncpy(vl.type_instance, something, sizeof(vl.type_instance));
  sstrncpy(vl.type, type, sizeof(vl.type));

  struct timeval tv;

  gettimeofday(&tv, NULL);

  unsigned long long millisecondsSinceEpoch =
  (unsigned long long)(tv.tv_sec) * 1000 +
  (unsigned long long)(tv.tv_usec) / 1000;

  INFO("sysevent plugin (%llu): dispatching something", millisecondsSinceEpoch);

  plugin_dispatch_values(&vl);
} /* }}} void sysevent_submit */

static int sysevent_read(void) /* {{{ */
{
  if (sysevent_thread_error != 0) {
    ERROR("sysevent plugin: The sysevent thread had a problem. Restarting it.");

    stop_thread(0);

    // TODO: clean up buffer data?

    start_thread();

    return (-1);
  } /* if (sysevent_thread_error != 0) */

  // TODO: lock buffer and read all data available, then unlock

  // TODO: publish (submit) new data (if any)
  submit("foo", "gauge", 1);

  return (0);
} /* }}} int sysevent_read */

static int sysevent_shutdown(void) /* {{{ */
{

  INFO("sysevent plugin: Shutting down thread.");
  if (stop_thread(1) < 0)
    return (-1);

  // Clean-up required here?

  return (0);
} /* }}} int sysevent_shutdown */

void module_register(void) {
  plugin_register_config("sysevent", sysevent_config, config_keys, config_keys_num);
  plugin_register_init("sysevent", sysevent_init);
  plugin_register_read("sysevent", sysevent_read);
  plugin_register_shutdown("sysevent", sysevent_shutdown);
} /* void module_register */