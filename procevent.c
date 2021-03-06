/**
 * collectd - src/procevent.c
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
 *     Andrew Bays <abays at redhat.com>
 **/

#include "collectd.h"

#include "common.h"
#include "plugin.h"
#include "utils_complain.h"

#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <dirent.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <yajl/yajl_common.h>
#include <yajl/yajl_gen.h>
#if HAVE_YAJL_YAJL_VERSION_H
#include <yajl/yajl_version.h>
#endif
#if defined(YAJL_MAJOR) && (YAJL_MAJOR > 1)
#define HAVE_YAJL_V2 1
#endif

#define PROCEVENT_EXITED 0
#define PROCEVENT_STARTED 1
#define PROCEVENT_FIELDS 4 // pid, status, extra, timestamp
#define BUFSIZE 512
#define PROCDIR "/proc"
#define PROCEVENT_REGEX_MATCHES 1
#define PROCEVENT_VES_VERSION "1.0"
#define PROCEVENT_VES_FAULT_FIELDS_VERSION "1.0"

/*
 * Private data types
 */

typedef struct {
  int head;
  int tail;
  int maxLen;
  cdtime_t **buffer;
} circbuf_t;

struct processlist_s {
  char *process;
  char *process_regex;

  regex_t process_regex_obj;

  uint32_t is_regex;
  uint32_t pid;

  struct processlist_s *next;
};
typedef struct processlist_s processlist_t;

/*
 * Private variables
 */

static int procevent_thread_loop = 0;
static int procevent_thread_error = 0;
static pthread_t procevent_thread_id;
static pthread_mutex_t procevent_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t procevent_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t procevent_list_lock = PTHREAD_MUTEX_INITIALIZER;
static int nl_sock = -1;
static int buffer_length;
static circbuf_t ring;
static processlist_t *processlist_head = NULL;
static int event_id = 0;

static const char *config_keys[] = {"BufferLength", "Process", "RegexProcess"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

/*
 * Private functions
 */

static int gen_ves_payload(int state, int pid, char * process, cdtime_t timestamp, char ** buf)
{
  const unsigned char *buf2;
  yajl_gen g;

#if !defined(HAVE_YAJL_V2)
  yajl_gen_config conf = {};

  conf.beautify = 0;
#endif

#if HAVE_YAJL_V2
  size_t len;
  g = yajl_gen_alloc(NULL);
  yajl_gen_config(g, yajl_gen_beautify, 0);
#else
  unsigned int len;
  g = yajl_gen_alloc(&conf, NULL);
#endif

  yajl_gen_clear(g);

  // *** BEGIN common event header ***

  if (yajl_gen_map_open(g) != yajl_gen_status_ok)
    goto err;

  // domain
  if (yajl_gen_string(g, (u_char *)"domain", strlen("domain")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)"fault", strlen("fault")) !=
      yajl_gen_status_ok)
    goto err;

  // eventId
  if (yajl_gen_string(g, (u_char *)"eventId", strlen("eventId")) !=
      yajl_gen_status_ok)
    goto err;

  event_id = event_id + 1;
  int event_id_len = sizeof(char) * sizeof(int) * 4 + 1;
  char * event_id_str = malloc(event_id_len);
  snprintf(event_id_str, event_id_len, "%d", event_id);

  if (yajl_gen_number(g, event_id_str, strlen(event_id_str)) !=
      yajl_gen_status_ok)
  {
    sfree(event_id_str);
    goto err;
  }

  sfree(event_id_str);

  // eventName
  if (yajl_gen_string(g, (u_char *)"eventName", strlen("eventName")) !=
      yajl_gen_status_ok)
    goto err;

  int event_name_len = 0;
  event_name_len = event_name_len + (sizeof(char) * sizeof(int) * 4); // pid
  event_name_len = event_name_len + strlen(process); // process name
  event_name_len = event_name_len + (state == 0 ? 4 : 2); // "down" or "up"
  event_name_len = event_name_len + 13; // "process", 3 spaces, 2 parentheses and null-terminator 
  char * event_name_str = malloc(event_name_len);
  memset(event_name_str, '\0', event_name_len);
  snprintf(event_name_str, event_name_len, "process %s (%d) %s", process, pid, (state == 0 ? "down" : "up"));

  if (yajl_gen_string(g, (u_char *)event_name_str, strlen(event_name_str)) !=
      yajl_gen_status_ok)
  {
    sfree(event_name_str);
    goto err;
  }

  sfree(event_name_str);

  // lastEpochMicrosec
  if (yajl_gen_string(g, (u_char *)"lastEpochMicrosec", strlen("lastEpochMicrosec")) !=
      yajl_gen_status_ok)
    goto err;

  int last_epoch_microsec_len = sizeof(char) * sizeof(long long unsigned int) * 4 + 1;
  char * last_epoch_microsec_str = malloc(last_epoch_microsec_len);
  snprintf(last_epoch_microsec_str, last_epoch_microsec_len, "%llu", (long long unsigned int) CDTIME_T_TO_US(cdtime()));

  if (yajl_gen_number(g, last_epoch_microsec_str, strlen(last_epoch_microsec_str)) !=
      yajl_gen_status_ok)
  {
    sfree(last_epoch_microsec_str);
    goto err;
  }

  sfree(last_epoch_microsec_str);

  // priority
  if (yajl_gen_string(g, (u_char *)"priority", strlen("priority")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)"high", strlen("high")) !=
      yajl_gen_status_ok)
    goto err;

  // reportingEntityName
  if (yajl_gen_string(g, (u_char *)"reportingEntityName", strlen("reportingEntityName")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)"collectd procevent plugin", strlen("collectd procevent plugin")) !=
      yajl_gen_status_ok)
    goto err;

  // sequence
  if (yajl_gen_string(g, (u_char *)"sequence", strlen("sequence")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_number(g, "0", strlen("0")) !=
      yajl_gen_status_ok)
    goto err;

  // sourceName
  if (yajl_gen_string(g, (u_char *)"sourceName", strlen("sourceName")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)process, strlen(process)) !=
      yajl_gen_status_ok)
    goto err;

  // startEpochMicrosec
  if (yajl_gen_string(g, (u_char *)"startEpochMicrosec", strlen("startEpochMicrosec")) !=
      yajl_gen_status_ok)
    goto err;

  int start_epoch_microsec_len = sizeof(char) * sizeof(long long unsigned int) * 4 + 1;
  char * start_epoch_microsec_str = malloc(start_epoch_microsec_len);
  snprintf(start_epoch_microsec_str, start_epoch_microsec_len, "%llu", (long long unsigned int) timestamp);

  if (yajl_gen_number(g, start_epoch_microsec_str, strlen(start_epoch_microsec_str)) !=
      yajl_gen_status_ok)
  {
    sfree(start_epoch_microsec_str);
    goto err;
  }

  sfree(start_epoch_microsec_str);

  // version
  if (yajl_gen_string(g, (u_char *)"version", strlen("version")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_number(g, PROCEVENT_VES_VERSION, strlen(PROCEVENT_VES_VERSION)) !=
      yajl_gen_status_ok)
    goto err;

  // *** END common event header ***

  // *** BEGIN fault fields ***

  if (yajl_gen_string(g, (u_char *)"faultFields", strlen("faultFields")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_map_open(g) != yajl_gen_status_ok)
    goto err;

  // alarmCondition
  if (yajl_gen_string(g, (u_char *)"alarmCondition", strlen("alarmCondition")) !=
      yajl_gen_status_ok)
    goto err;

  int alarm_condition_len = 0;
  alarm_condition_len = alarm_condition_len + (sizeof(char) * sizeof(int) * 4); // pid
  alarm_condition_len = alarm_condition_len + strlen(process); // process name
  alarm_condition_len = alarm_condition_len + 25; // "process", "state", "change", 4 spaces, 2 parentheses and null-terminator 
  char * alarm_condition_str = malloc(alarm_condition_len);
  memset(alarm_condition_str, '\0', alarm_condition_len);
  snprintf(alarm_condition_str, alarm_condition_len, "process %s (%d) state change", process, pid);

  if (yajl_gen_string(g, (u_char *)alarm_condition_str, strlen(alarm_condition_str)) !=
      yajl_gen_status_ok)
  {
    sfree(alarm_condition_str);
    goto err;
  }

  sfree(alarm_condition_str);

  // alarmInterfaceA
  if (yajl_gen_string(g, (u_char *)"alarmInterfaceA", strlen("alarmInterfaceA")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)process, strlen(process)) !=
      yajl_gen_status_ok)
    goto err;

  // eventSeverity
  if (yajl_gen_string(g, (u_char *)"eventSeverity", strlen("eventSeverity")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)(state == 0 ? "CRITICAL" : "NORMAL"), strlen((state == 0 ? "CRITICAL" : "NORMAL"))) !=
      yajl_gen_status_ok)
    goto err;

  // eventSourceType
  if (yajl_gen_string(g, (u_char *)"eventSourceType", strlen("eventSourceType")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)"process", strlen("process")) !=
      yajl_gen_status_ok)
    goto err;

  // faultFieldsVersion
  if (yajl_gen_string(g, (u_char *)"faultFieldsVersion", strlen("faultFieldsVersion")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_number(g, PROCEVENT_VES_FAULT_FIELDS_VERSION, strlen(PROCEVENT_VES_FAULT_FIELDS_VERSION)) !=
      yajl_gen_status_ok)
    goto err;

  // specificProblem
  if (yajl_gen_string(g, (u_char *)"specificProblem", strlen("specificProblem")) !=
      yajl_gen_status_ok)
    goto err;

  int specific_problem_len = 0;
  specific_problem_len = specific_problem_len + (sizeof(char) * sizeof(int) * 4); // pid
  specific_problem_len = specific_problem_len + strlen(process); // process name
  specific_problem_len = specific_problem_len + (state == 0 ? 4 : 2); // "down" or "up"
  specific_problem_len = specific_problem_len + 13; // "process", 3 spaces, 2 parentheses and null-terminator 
  char * specific_problem_str = malloc(specific_problem_len);
  memset(specific_problem_str, '\0', specific_problem_len);
  snprintf(specific_problem_str, specific_problem_len, "process %s (%d) %s", process, pid, (state == 0 ? "down" : "up"));

  if (yajl_gen_string(g, (u_char *)specific_problem_str, strlen(specific_problem_str)) !=
      yajl_gen_status_ok)
  {
    sfree(specific_problem_str);
    goto err;
  }

  sfree(specific_problem_str);

  // vfStatus
  if (yajl_gen_string(g, (u_char *)"vfStatus", strlen("vfStatus")) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_string(g, (u_char *)(state == 0 ? "Ready to terminate" : "Active"), strlen((state == 0 ? "Ready to terminate" : "Active"))) !=
      yajl_gen_status_ok)
    goto err;

  if (yajl_gen_map_close(g) != yajl_gen_status_ok)
    goto err;

  // *** END fault fields ***

  if (yajl_gen_map_close(g) != yajl_gen_status_ok)
    goto err;

  if (yajl_gen_get_buf(g, &buf2, &len) != yajl_gen_status_ok)
    goto err;

  INFO("AJB buf: %s", buf2);

  *buf = malloc(strlen((char *)buf2) + 1);

  sstrncpy(*buf, (char *)buf2, strlen((char *)buf2) + 1);

  yajl_gen_free(g);

  return 0;

err:
  yajl_gen_free(g);
  ERROR("procevent plugin: gen_ves_payload failed");
  return -1;
}

// Does /proc/<pid>/comm contain a process name we are interested in?
static processlist_t *process_check(int pid) {
  int len, is_match, status, retval;
  char file[BUFSIZE];
  FILE *fh;
  char buffer[BUFSIZE];
  regmatch_t matches[PROCEVENT_REGEX_MATCHES];

  len = snprintf(file, sizeof(file), PROCDIR "/%d/comm", pid);

  if ((len < 0) || (len >= BUFSIZE)) {
    WARNING("procevent process_check: process name too large");
    return NULL;
  }

  if (NULL == (fh = fopen(file, "r"))) {
    // No /proc/<pid>/comm for this pid, just ignore
    DEBUG("procevent plugin: no comm file available for pid %d", pid);
    return NULL;
  }

  retval = fscanf(fh, "%[^\n]", buffer);

  if (retval < 0) {
    WARNING("procevent process_check: unable to read comm file for pid %d",
            pid);
    return NULL;
  }

  //
  // Go through the processlist linked list and look for the process name
  // in /proc/<pid>/comm.  If found:
  // 1. If pl->pid is -1, then set pl->pid to <pid>
  // 2. If pl->pid is not -1, then another <process name> process was already
  //    found.  If <pid> == pl->pid, this is an old match, so do nothing.
  //    If the <pid> is different, however,  make a new processlist_t and
  //    associate <pid> with it (with the same process name as the existing).
  //

  pthread_mutex_lock(&procevent_list_lock);

  processlist_t *pl;
  processlist_t *match = NULL;

  for (pl = processlist_head; pl != NULL; pl = pl->next) {
    if (pl->is_regex != 0) {
      is_match = (regexec(&pl->process_regex_obj, buffer,
                          PROCEVENT_REGEX_MATCHES, matches, 0) == 0
                      ? 1
                      : 0);
    } else {
      is_match = (strcmp(buffer, pl->process) == 0 ? 1 : 0);
    }

    if (is_match == 1) {
      DEBUG("procevent plugin: process %d name match (pattern: %s) for %s", pid,
            (pl->is_regex == 0 ? pl->process : pl->process_regex), buffer);

      if (pl->is_regex == 1) {
        // If this is a regex name, copy the actual process name into the object
        // for cleaner log reporting

        if (pl->process != NULL)
          sfree(pl->process);
        pl->process = strdup(buffer);
        if (pl->process == NULL) {
          char errbuf[1024];
          ERROR("procevent plugin: strdup failed during process_check: %s",
                sstrerror(errno, errbuf, sizeof(errbuf)));
          pthread_mutex_unlock(&procevent_list_lock);
          return NULL;
        }
      }

      if (pl->pid == pid) {
        // this is a match, and we've already stored the exact pid/name combo
        match = pl;
        break;
      } else if (pl->pid == -1) {
        // this is a match, and we've found a candidate processlist_t to store
        // this new pid/name combo
        pl->pid = pid;
        match = pl;
        break;
      } else if (pl->pid != -1) {
        // this is a match, but another instance of this process has already
        // claimed this pid/name combo,
        // so keep looking
        match = pl;
        continue;
      }
    }
  }

  if (match != NULL && match->pid != -1 && match->pid != pid) {
    // if there was a match but the associated processlist_t object already
    // contained a pid/name combo,
    // then make a new one and add it to the linked list

    DEBUG(
        "procevent plugin: allocating new processlist_t object for PID %d (%s)",
        pid, match->process);

    processlist_t *pl2;
    char *process;
    char *process_regex;

    pl2 = malloc(sizeof(*pl2));
    if (pl2 == NULL) {
      char errbuf[1024];
      ERROR("procevent plugin: malloc failed during process_check: %s",
            sstrerror(errno, errbuf, sizeof(errbuf)));
      pthread_mutex_unlock(&procevent_list_lock);
      return NULL;
    }

    process = strdup(match->process);
    if (process == NULL) {
      char errbuf[1024];
      sfree(pl2);
      ERROR("procevent plugin: strdup failed during process_check: %s",
            sstrerror(errno, errbuf, sizeof(errbuf)));
      pthread_mutex_unlock(&procevent_list_lock);
      return NULL;
    }

    if (match->is_regex == 1) {
      pl2->is_regex = 1;
      status =
          regcomp(&pl2->process_regex_obj, match->process_regex, REG_EXTENDED);

      if (status != 0) {
        ERROR("procevent plugin: invalid regular expression: %s",
              match->process_regex);
        return NULL;
      }

      process_regex = strdup(match->process_regex);
      if (process_regex == NULL) {
        char errbuf[1024];
        sfree(pl);
        ERROR("procevent plugin: strdup failed during process_check: %s",
              sstrerror(errno, errbuf, sizeof(errbuf)));
        return NULL;
      }

      pl2->process_regex = process_regex;
    }

    pl2->process = process;
    pl2->pid = pid;
    pl2->next = processlist_head;
    processlist_head = pl2;

    match = pl2;
  }

  pthread_mutex_unlock(&procevent_list_lock);

  if (fh != NULL) {
    fclose(fh);
    fh = NULL;
  }

  return match;
}

// Does our map have this PID or name?
static processlist_t *process_map_check(int pid, char *process) {
  processlist_t *pl;

  pthread_mutex_lock(&procevent_list_lock);

  for (pl = processlist_head; pl != NULL; pl = pl->next) {
    int match_pid = 0;
    int match_process = 0;
    int match = 0;

    if (pid > 0) {
      if (pl->pid == pid)
        match_pid = 1;
    }

    if (process != NULL) {
      if (strcmp(pl->process, process) == 0)
        match_process = 1;
    }

    if (pid > 0 && process == NULL && match_pid == 1)
      match = 1;
    else if (pid < 0 && process != NULL && match_process == 1)
      match = 1;
    else if (pid > 0 && process != NULL && match_pid == 1 && match_process == 1)
      match = 1;

    if (match == 1) {
      pthread_mutex_unlock(&procevent_list_lock);
      return pl;
    }
  }

  pthread_mutex_unlock(&procevent_list_lock);

  return NULL;
}

static int process_map_refresh(void) {
  DIR *proc;

  errno = 0;
  proc = opendir(PROCDIR);
  if (proc == NULL) {
    char errbuf[1024];
    ERROR("procevent plugin: fopen (%s): %s", PROCDIR,
          sstrerror(errno, errbuf, sizeof(errbuf)));
    return -1;
  }

  while (42) {
    struct dirent *dent;
    int len;
    char file[BUFSIZE];

    struct stat statbuf;

    int status;

    errno = 0;
    dent = readdir(proc);
    if (dent == NULL) {
      char errbuf[4096];

      if (errno == 0) /* end of directory */
        break;

      ERROR("procevent plugin: failed to read directory %s: %s", PROCDIR,
            sstrerror(errno, errbuf, sizeof(errbuf)));
      closedir(proc);
      return -1;
    }

    if (dent->d_name[0] == '.')
      continue;

    len = snprintf(file, sizeof(file), PROCDIR "/%s", dent->d_name);
    if ((len < 0) || (len >= BUFSIZE))
      continue;

    status = stat(file, &statbuf);
    if (status != 0) {
      char errbuf[4096];
      WARNING("procevent plugin: stat (%s) failed: %s", file,
              sstrerror(errno, errbuf, sizeof(errbuf)));
      continue;
    }

    if (!S_ISDIR(statbuf.st_mode))
      continue;

    len = snprintf(file, sizeof(file), PROCDIR "/%s/comm", dent->d_name);
    if ((len < 0) || (len >= BUFSIZE))
      continue;

    int not_number = 0;

    for (int i = 0; i < strlen(dent->d_name); i++) {
      if (!isdigit(dent->d_name[i])) {
        not_number = 1;
        break;
      }
    }

    if (not_number != 0)
      continue;

    // Check if we need to store this pid/name combo in our processlist_t linked
    // list
    int this_pid = atoi(dent->d_name);
    processlist_t *pl = process_check(this_pid);

    if (pl != NULL)
      DEBUG("procevent plugin: process map refreshed for PID %d and name %s",
            this_pid, pl->process);
  }

  closedir(proc);

  return 0;
}

static int nl_connect() {
  int rc;
  struct sockaddr_nl sa_nl;

  nl_sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
  if (nl_sock == -1) {
    ERROR("procevent plugin: socket open failed.");
    return -1;
  }

  sa_nl.nl_family = AF_NETLINK;
  sa_nl.nl_groups = CN_IDX_PROC;
  sa_nl.nl_pid = getpid();

  rc = bind(nl_sock, (struct sockaddr *)&sa_nl, sizeof(sa_nl));
  if (rc == -1) {
    ERROR("procevent plugin: socket bind failed.");
    close(nl_sock);
    return -1;
  }

  return 0;
}

static int set_proc_ev_listen(bool enable) {
  int rc;
  struct __attribute__((aligned(NLMSG_ALIGNTO))) {
    struct nlmsghdr nl_hdr;
    struct __attribute__((__packed__)) {
      struct cn_msg cn_msg;
      enum proc_cn_mcast_op cn_mcast;
    };
  } nlcn_msg;

  memset(&nlcn_msg, 0, sizeof(nlcn_msg));
  nlcn_msg.nl_hdr.nlmsg_len = sizeof(nlcn_msg);
  nlcn_msg.nl_hdr.nlmsg_pid = getpid();
  nlcn_msg.nl_hdr.nlmsg_type = NLMSG_DONE;

  nlcn_msg.cn_msg.id.idx = CN_IDX_PROC;
  nlcn_msg.cn_msg.id.val = CN_VAL_PROC;
  nlcn_msg.cn_msg.len = sizeof(enum proc_cn_mcast_op);

  nlcn_msg.cn_mcast = enable ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE;

  rc = send(nl_sock, &nlcn_msg, sizeof(nlcn_msg), 0);
  if (rc == -1) {
    ERROR("procevent plugin: subscribing to netlink process events failed.");
    return -1;
  }

  return 0;
}

static int read_event() {
  int status;
  int ret = 0;
  int proc_id = -1;
  int proc_status = -1;
  int proc_extra = -1;
  struct __attribute__((aligned(NLMSG_ALIGNTO))) {
    struct nlmsghdr nl_hdr;
    struct __attribute__((__packed__)) {
      struct cn_msg cn_msg;
      struct proc_event proc_ev;
    };
  } nlcn_msg;

  if (nl_sock == -1)
    return ret;

  status = recv(nl_sock, &nlcn_msg, sizeof(nlcn_msg), 0);

  if (status == 0) {
    return 0;
  } else if (status == -1) {
    if (errno != EINTR) {
      ERROR("procevent plugin: socket receive error: %d", errno);
      return -1;
    }
  }

  switch (nlcn_msg.proc_ev.what) {
  case PROC_EVENT_NONE:
    // printf("set mcast listen ok\n");
    break;
  case PROC_EVENT_FORK:
    // printf("fork: parent tid=%d pid=%d -> child tid=%d pid=%d\n",
    //         nlcn_msg.proc_ev.event_data.fork.parent_pid,
    //         nlcn_msg.proc_ev.event_data.fork.parent_tgid,
    //         nlcn_msg.proc_ev.event_data.fork.child_pid,
    //         nlcn_msg.proc_ev.event_data.fork.child_tgid);
    // proc_status = PROCEVENT_STARTED;
    // proc_id = nlcn_msg.proc_ev.event_data.fork.child_pid;
    break;
  case PROC_EVENT_EXEC:
    // printf("exec: tid=%d pid=%d\n",
    //         nlcn_msg.proc_ev.event_data.exec.process_pid,
    //         nlcn_msg.proc_ev.event_data.exec.process_tgid);
    proc_status = PROCEVENT_STARTED;
    proc_id = nlcn_msg.proc_ev.event_data.exec.process_pid;
    break;
  case PROC_EVENT_UID:
    // printf("uid change: tid=%d pid=%d from %d to %d\n",
    //         nlcn_msg.proc_ev.event_data.id.process_pid,
    //         nlcn_msg.proc_ev.event_data.id.process_tgid,
    //         nlcn_msg.proc_ev.event_data.id.r.ruid,
    //         nlcn_msg.proc_ev.event_data.id.e.euid);
    break;
  case PROC_EVENT_GID:
    // printf("gid change: tid=%d pid=%d from %d to %d\n",
    //         nlcn_msg.proc_ev.event_data.id.process_pid,
    //         nlcn_msg.proc_ev.event_data.id.process_tgid,
    //         nlcn_msg.proc_ev.event_data.id.r.rgid,
    //         nlcn_msg.proc_ev.event_data.id.e.egid);
    break;
  case PROC_EVENT_EXIT:
    proc_id = nlcn_msg.proc_ev.event_data.exit.process_pid;
    proc_status = PROCEVENT_EXITED;
    proc_extra = nlcn_msg.proc_ev.event_data.exit.exit_code;
    break;
  default:
    break;
  }

  // If we're interested in this process status event, place the event
  // in the ring buffer for consumption by the main polling thread.

  if (proc_status != -1) {
    pthread_mutex_unlock(&procevent_lock);

    int next = ring.head + 1;
    if (next >= ring.maxLen)
      next = 0;

    if (next == ring.tail) {
      WARNING("procevent plugin: ring buffer full");
    } else {
      DEBUG("procevent plugin: Process %d status is now %s at %llu", proc_id,
            (proc_status == PROCEVENT_EXITED ? "EXITED" : "STARTED"), (long long unsigned int) CDTIME_T_TO_US(cdtime()));

      if (proc_status == PROCEVENT_EXITED) {
        ring.buffer[ring.head][0] = proc_id;
        ring.buffer[ring.head][1] = proc_status;
        ring.buffer[ring.head][2] = proc_extra;
        ring.buffer[ring.head][3] = (long long unsigned int) CDTIME_T_TO_US(cdtime());
      } else {
        ring.buffer[ring.head][0] = proc_id;
        ring.buffer[ring.head][1] = proc_status;
        ring.buffer[ring.head][2] = 0;
        ring.buffer[ring.head][3] = (long long unsigned int) CDTIME_T_TO_US(cdtime());
      }

      ring.head = next;
    }

    pthread_mutex_unlock(&procevent_lock);
  }

  return ret;
}

static void *procevent_thread(void *arg) /* {{{ */
{
  pthread_mutex_lock(&procevent_lock);

  while (procevent_thread_loop > 0) {
    int status;

    pthread_mutex_unlock(&procevent_lock);

    usleep(1000);

    status = read_event();

    pthread_mutex_lock(&procevent_lock);

    if (status < 0) {
      procevent_thread_error = 1;
      break;
    }

    if (procevent_thread_loop <= 0)
      break;
  } /* while (procevent_thread_loop > 0) */

  pthread_mutex_unlock(&procevent_lock);

  return ((void *)0);
} /* }}} void *procevent_thread */

static int start_thread(void) /* {{{ */
{
  int status;

  pthread_mutex_lock(&procevent_lock);

  if (procevent_thread_loop != 0) {
    pthread_mutex_unlock(&procevent_lock);
    return (0);
  }

  if (nl_sock == -1) {
    status = nl_connect();

    if (status != 0)
      return status;

    status = set_proc_ev_listen(true);
    if (status == -1)
      return status;
  }

  DEBUG("procevent plugin: socket created and bound");

  procevent_thread_loop = 1;
  procevent_thread_error = 0;

  status = plugin_thread_create(&procevent_thread_id, /* attr = */ NULL,
                                procevent_thread,
                                /* arg = */ (void *)0, "procevent");
  if (status != 0) {
    procevent_thread_loop = 0;
    ERROR("procevent plugin: Starting thread failed.");
    pthread_mutex_unlock(&procevent_lock);
    return (-1);
  }

  pthread_mutex_unlock(&procevent_lock);
  return (0);
} /* }}} int start_thread */

static int stop_thread(int shutdown) /* {{{ */
{
  int status;

  if (nl_sock != -1) {
    status = close(nl_sock);
    if (status != 0) {
      ERROR("procevent plugin: failed to close socket %d: %d (%s)", nl_sock,
            status, strerror(errno));
      return (-1);
    } else
      nl_sock = -1;
  }

  pthread_mutex_lock(&procevent_lock);

  if (procevent_thread_loop == 0) {
    pthread_mutex_unlock(&procevent_lock);
    return (-1);
  }

  procevent_thread_loop = 0;
  pthread_cond_broadcast(&procevent_cond);
  pthread_mutex_unlock(&procevent_lock);

  if (shutdown == 1) {
    // Calling pthread_cancel here in
    // the case of a shutdown just assures that the thread is
    // gone and that the process has been fully terminated.

    DEBUG("procevent plugin: Canceling thread for process shutdown");

    status = pthread_cancel(procevent_thread_id);

    if (status != 0) {
      ERROR("procevent plugin: Unable to cancel thread: %d", status);
      status = -1;
    }
  } else {
    status = pthread_join(procevent_thread_id, /* return = */ NULL);
    if (status != 0) {
      ERROR("procevent plugin: Stopping thread failed.");
      status = -1;
    }
  }

  pthread_mutex_lock(&procevent_lock);
  memset(&procevent_thread_id, 0, sizeof(procevent_thread_id));
  procevent_thread_error = 0;
  pthread_mutex_unlock(&procevent_lock);

  DEBUG("procevent plugin: Finished requesting stop of thread");

  return (status);
} /* }}} int stop_thread */

static int procevent_init(void) /* {{{ */
{
  int status;

  if (processlist_head == NULL) {
    NOTICE("procevent plugin: No processes have been configured.");
    return (-1);
  }

  ring.head = 0;
  ring.tail = 0;
  ring.maxLen = buffer_length;
  ring.buffer = (cdtime_t **)malloc(buffer_length * sizeof(cdtime_t *));

  for (int i = 0; i < buffer_length; i++) {
    ring.buffer[i] = (cdtime_t *)malloc(PROCEVENT_FIELDS * sizeof(cdtime_t));
  }

  status = process_map_refresh();

  if (status == -1) {
    ERROR("procevent plugin: Initial process mapping failed.");
    return (-1);
  }

  return (start_thread());
} /* }}} int procevent_init */

static int procevent_config(const char *key, const char *value) /* {{{ */
{
  int status;

  if (strcasecmp(key, "BufferLength") == 0) {
    buffer_length = atoi(value);
  } else if (strcasecmp(key, "Process") == 0 ||
             strcasecmp(key, "RegexProcess") == 0) {

    processlist_t *pl;
    char *process;
    char *process_regex;

    pl = malloc(sizeof(*pl));
    if (pl == NULL) {
      char errbuf[1024];
      ERROR("procevent plugin: malloc failed during procevent_config: %s",
            sstrerror(errno, errbuf, sizeof(errbuf)));
      return (1);
    }

    process = strdup(value);
    if (process == NULL) {
      char errbuf[1024];
      sfree(pl);
      ERROR("procevent plugin: strdup failed during procevent_config: %s",
            sstrerror(errno, errbuf, sizeof(errbuf)));
      return (1);
    }

    if (strcasecmp(key, "RegexProcess") == 0) {
      pl->is_regex = 1;
      status = regcomp(&pl->process_regex_obj, value, REG_EXTENDED);

      if (status != 0) {
        ERROR("procevent plugin: invalid regular expression: %s", value);
        return (1);
      }

      process_regex = strdup(value);
      if (process_regex == NULL) {
        char errbuf[1024];
        sfree(pl);
        ERROR("procevent plugin: strdup failed during procevent_config: %s",
              sstrerror(errno, errbuf, sizeof(errbuf)));
        return (1);
      }

      pl->process_regex = process_regex;
    } else {
      pl->is_regex = 0;
    }

    pl->process = process;
    pl->pid = -1;
    pl->next = processlist_head;
    processlist_head = pl;
  } else {
    return (-1);
  }

  return (0);
} /* }}} int procevent_config */

// static void submit(int pid, const char *type, /* {{{ */
//                    gauge_t value, const char *process) {
//   value_list_t vl = VALUE_LIST_INIT;
//   char hostname[1024];

//   vl.values = &(value_t){.gauge = value};
//   vl.values_len = 1;
//   sstrncpy(vl.plugin, "procevent", sizeof(vl.plugin));
//   sstrncpy(vl.plugin_instance, process, sizeof(vl.plugin_instance));
//   sstrncpy(vl.type, type, sizeof(vl.type));

//   DEBUG("procevent plugin: dispatching state %d for PID %d (%s)", (int)value,
//         pid, process);

//   // Create metadata to store JSON key-values
//   meta_data_t *meta = meta_data_create();

//   vl.meta = meta;

//   gethostname(hostname, sizeof(hostname));

//   if (value == 1) {
//     meta_data_add_string(meta, "condition", "process_up");
//     meta_data_add_string(meta, "entity", process);
//     meta_data_add_string(meta, "source", hostname);
//     meta_data_add_string(meta, "dest", "process_down");
//   } else {
//     meta_data_add_string(meta, "condition", "process_down");
//     meta_data_add_string(meta, "entity", process);
//     meta_data_add_string(meta, "source", hostname);
//     meta_data_add_string(meta, "dest", "process_up");
//   }

//   plugin_dispatch_values(&vl);
// } /* }}} void interface_submit */

static void
procevent_dispatch_notification(int pid, const char *type, /* {{{ */
                   gauge_t value, char *process, cdtime_t timestamp) {
  char *buf = NULL;
  notification_t n = {
      NOTIF_FAILURE, cdtime(), "", "", "procevent", "", "", "", NULL};

  if (value == 1)
    n.severity = NOTIF_OKAY;

  char hostname[1024];
  gethostname(hostname, sizeof(hostname));

  sstrncpy(n.host, hostname, sizeof(n.host));
  sstrncpy(n.plugin_instance, process, sizeof(n.plugin_instance));
  sstrncpy(n.type, "gauge", sizeof(n.type));
  sstrncpy(n.type_instance, "process_status", sizeof(n.type_instance));

  gen_ves_payload(value, pid, process, timestamp, &buf);

  notification_meta_t *m = calloc(1, sizeof(*m));

  if (m == NULL)
  {
    char errbuf[1024];
    sfree(buf);
    ERROR("procevent plugin: unable to allocate metadata: %s",
          sstrerror(errno, errbuf, sizeof(errbuf)));
    return;
  }

  sstrncpy(m->name, "ves", sizeof(m->name));
  m->nm_value.nm_string = sstrdup(buf);
  m->type = NM_TYPE_STRING;
  n.meta = m;

  INFO("AJB VES message: %s", n.meta->nm_value.nm_string);

  INFO("procevent plugin: dispatching state %d for PID %d (%s)", (int)value,
        pid, process);

  plugin_dispatch_notification(&n);
  plugin_notification_meta_free(n.meta);

  // malloc'd in gen_ves_payload
  if (buf != NULL)
    sfree(buf);
}

static int procevent_read(void) /* {{{ */
{
  if (procevent_thread_error != 0) {
    ERROR(
        "procevent plugin: The interface thread had a problem. Restarting it.");

    stop_thread(0);

    start_thread();

    return (-1);
  } /* if (procevent_thread_error != 0) */

  pthread_mutex_lock(&procevent_lock);

  while (ring.head != ring.tail) {
    int next = ring.tail + 1;

    if (next >= ring.maxLen)
      next = 0;

    if (ring.buffer[ring.tail][1] == PROCEVENT_EXITED) {
      processlist_t *pl = process_map_check(ring.buffer[ring.tail][0], NULL);

      if (pl != NULL) {
        // This process is of interest to us, so publish its EXITED status
        procevent_dispatch_notification(ring.buffer[ring.tail][0], "gauge", ring.buffer[ring.tail][1],
               pl->process, ring.buffer[ring.tail][3]);
        INFO("procevent plugin: PID %d (%s) EXITED, removing PID from process "
              "list",
              pl->pid, pl->process);
        pl->pid = -1;
      }
    } else if (ring.buffer[ring.tail][1] == PROCEVENT_STARTED) {
      // a new process has started, so check if we should monitor it
      processlist_t *pl = process_check(ring.buffer[ring.tail][0]);

      if (pl != NULL) {
        // This process is of interest to us, so publish its STARTED status
        procevent_dispatch_notification(ring.buffer[ring.tail][0], "gauge", ring.buffer[ring.tail][1],
               pl->process, ring.buffer[ring.tail][3]);
        INFO(
            "procevent plugin: PID %d (%s) STARTED, adding PID to process list",
            pl->pid, pl->process);
      }
    }

    ring.tail = next;
  }

  pthread_mutex_unlock(&procevent_lock);

  return (0);
} /* }}} int procevent_read */

static int procevent_shutdown(void) /* {{{ */
{
  // int status = 0;
  processlist_t *pl;

  DEBUG("procevent plugin: Shutting down thread.");

  if (stop_thread(1) < 0)
    return (-1);

  for (int i = 0; i < buffer_length; i++) {
    free(ring.buffer[i]);
  }

  free(ring.buffer);

  pl = processlist_head;
  while (pl != NULL) {
    processlist_t *pl_next;

    pl_next = pl->next;

    if (pl->is_regex == 1) {
      sfree(pl->process_regex);
      regfree(&pl->process_regex_obj);
    }

    sfree(pl->process);
    sfree(pl);

    pl = pl_next;
  }

  return (0);
} /* }}} int procevent_shutdown */

void module_register(void) {
  plugin_register_config("procevent", procevent_config, config_keys,
                         config_keys_num);
  plugin_register_init("procevent", procevent_init);
  plugin_register_read("procevent", procevent_read);
  plugin_register_shutdown("procevent", procevent_shutdown);
} /* void module_register */
