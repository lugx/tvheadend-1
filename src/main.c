/*
 *  TV headend
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iconv.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <limits.h>

#include <pwd.h>
#include <grp.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <libavformat/avformat.h>

#include "tvhead.h"
#include "tcp.h"
#include "access.h"
#include "http.h"
#include "webui/webui.h"
#include "dvb/dvb.h"
#include "xmltv.h"
#include "spawn.h"
#include "subscriptions.h"
#include "serviceprobe.h"
#include "cwc.h"
#include "dvr/dvr.h"
#include "htsp.h"

#include "parachute.h"
#include "settings.h"

int running;
extern const char *htsversion;
extern const char *htsversion_full;
time_t dispatch_clock;
static LIST_HEAD(, gtimer) gtimers;
pthread_mutex_t global_lock;

static void
handle_sigpipe(int x)
{
  return;
}

static void
doexit(int x)
{
  running = 0;
}

static void
pull_chute (int sig)
{
  char pwd[PATH_MAX];

  getcwd(pwd, sizeof(pwd));
  syslog(LOG_ERR, "HTS Tvheadend crashed on signal %i (pwd \"%s\")",
	 sig, pwd);
}

/**
 *
 */
static int
gtimercmp(gtimer_t *a, gtimer_t *b)
{
  if(a->gti_expire < b->gti_expire)
    return -1;
  else if(a->gti_expire > b->gti_expire)
    return 1;
 return 0;
}


/**
 *
 */
void
gtimer_arm_abs(gtimer_t *gti, gti_callback_t *callback, void *opaque,
	       time_t when)
{
  lock_assert(&global_lock);

  if(gti->gti_callback != NULL)
    LIST_REMOVE(gti, gti_link);
    
  gti->gti_callback = callback;
  gti->gti_opaque = opaque;
  gti->gti_expire = when;

  LIST_INSERT_SORTED(&gtimers, gti, gti_link, gtimercmp);
}

/**
 *
 */
void
gtimer_arm(gtimer_t *gti, gti_callback_t *callback, void *opaque, int delta)
{
  time_t now;
  time(&now);
  
  gtimer_arm_abs(gti, callback, opaque, now + delta);
}

/**
 *
 */
void
gtimer_disarm(gtimer_t *gti)
{
  if(gti->gti_callback) {
    LIST_REMOVE(gti, gti_link);
    gti->gti_callback = NULL;
  }
}


/**
 *
 */
static void
mainloop(void)
{
  gtimer_t *gti;
  gti_callback_t *cb;

  while(running) {
    sleep(1);
    spawn_reaper();

    time(&dispatch_clock);

    comet_flush(); /* Flush idle comet mailboxes */

    pthread_mutex_lock(&global_lock);
    
    while((gti = LIST_FIRST(&gtimers)) != NULL) {
      if(gti->gti_expire > dispatch_clock)
	break;
      
      cb = gti->gti_callback;
      LIST_REMOVE(gti, gti_link);
      gti->gti_callback = NULL;

      cb(gti->gti_opaque);
      
    }
    pthread_mutex_unlock(&global_lock);
  }
}


/**
 *
 */
int
main(int argc, char **argv)
{
  int c;
  int forkaway = 0;
  FILE *pidfile;
  struct group *grp;
  struct passwd *pw;
  const char *usernam = NULL;
  const char *groupnam = NULL;
  int logfacility = LOG_DAEMON;
  sigset_t set;
  const char *settingspath = NULL;
  struct stat st;
  const char *contentpath = TVHEADEND_CONTENT_PATH;

  signal(SIGPIPE, handle_sigpipe);

  while((c = getopt(argc, argv, "fu:g:s:c:")) != -1) {
    switch(c) {
    case 'f':
      forkaway = 1;
      break;
    case 'u':
      usernam = optarg;
      break;
    case 'g':
      groupnam = optarg;
      break;
    case 's':
      settingspath = optarg;
      break;
    case 'c':
      contentpath = optarg;
      break;
    }
  }


  grp = getgrnam(groupnam ?: "video");
  pw = usernam ? getpwnam(usernam) : NULL;

  /**
   * Select path where to store settings
   */
  if(settingspath == NULL) {
    settingspath = "/var/lib/hts/tvheadend";
    
    if(stat(settingspath, &st)) {
      /* Path does not exist */

      if(mkdir("/var/lib/hts", 0777) && errno != EEXIST) {
	settingspath = NULL;
      } else if(mkdir("/var/lib/hts/tvheadend", 0777) && errno != EEXIST) {
	settingspath = NULL;
      } else {
	
      }
    }

    if(settingspath != NULL && 
       chown(settingspath, pw ? pw->pw_uid : 1, grp ? grp->gr_gid : 1))
      settingspath = NULL;
  }

  if(forkaway) {

    if(daemon(0, 0)) {
      exit(2);
    }

    pidfile = fopen("/var/run/tvheadend.pid", "w+");
    if(pidfile != NULL) {
      fprintf(pidfile, "%d\n", getpid());
      fclose(pidfile);
    }

   if(grp != NULL) {
      setgid(grp->gr_gid);
    } else {
      setgid(1);
    }

    
    if(pw != NULL) {
      setuid(pw->pw_uid);
    } else {
      setuid(1);
    }

    umask(0);
  }

  sigfillset(&set);
  sigprocmask(SIG_BLOCK, &set, NULL);

  openlog("tvheadend", LOG_PID, logfacility);

  hts_settings_init("tvheadend", settingspath);

  pthread_mutex_init(&global_lock, NULL);

  pthread_mutex_lock(&global_lock);

  htsparachute_init(pull_chute);
  
  /**
   * Initialize subsystems
   */
  av_register_all();

  xmltv_init();   /* Must be initialized before channels */

  channels_init();

  access_init();

  tcp_server_init();

  dvb_init();

  http_server_init();

  webui_init(contentpath);

  subscriptions_init();

  serviceprobe_init();

  cwc_init();

  epg_init();

  dvr_init();

  htsp_init();

  pthread_mutex_unlock(&global_lock);


  /**
   * Wait for SIGTERM / SIGINT, but only in this thread
   */

  running = 1;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);

  signal(SIGTERM, doexit);
  signal(SIGINT, doexit);

  pthread_sigmask(SIG_UNBLOCK, &set, NULL);

  syslog(LOG_NOTICE, "HTS Tvheadend version %s started, "
	 "running as pid:%d uid:%d gid:%d, settings located in '%s'",
	 htsversion_full,
	 getpid(), getuid(), getgid(), hts_settings_get_root());
  
 if(!forkaway)
    fprintf(stderr, "\nHTS Tvheadend version %s started.\n"
	    "Settings located in '%s'\n",
	    htsversion_full, hts_settings_get_root());

  mainloop();

  syslog(LOG_NOTICE, "Exiting HTS Tvheadend");

  if(forkaway)
    unlink("/var/run/tvheadend.pid");

  return 0;

}

/**
 *
 */
void
tvhlog(int severity, const char *subsys, const char *fmt, ...)
{
  va_list ap;
  htsmsg_t *m;
  char buf[512];
  char buf2[512];
  char t[50];
  int l;
  struct tm tm;
  time_t now;

  l = snprintf(buf, sizeof(buf), "%s: ", subsys);

  va_start(ap, fmt);
  vsnprintf(buf + l, sizeof(buf) - l, fmt, ap);
  va_end(ap);

  syslog(severity, "%s", buf);

  /**
   *
   */

  time(&now);
  localtime_r(&now, &tm);
  strftime(t, sizeof(t), "%b %d %H:%M:%S", &tm);

  snprintf(buf2, sizeof(buf2), "%s %s", t, buf);

  m = htsmsg_create_map();
  htsmsg_add_str(m, "notificationClass", "logmessage");
  htsmsg_add_str(m, "logtxt", buf2);
  comet_mailbox_add_message(m);
  htsmsg_destroy(m);
}


/**
 *
 */
void
tvh_str_set(char **strp, const char *src)
{
  free(*strp);
  *strp = src ? strdup(src) : NULL;
}


/**
 *
 */
void
tvh_str_update(char **strp, const char *src)
{
  if(src == NULL)
    return;
  free(*strp);
  *strp = strdup(src);
}
