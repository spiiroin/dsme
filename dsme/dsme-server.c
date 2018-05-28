/**
   @file dsme.c

   This file implements the main function and main loop of DSME component.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
   Copyright (C) 2012-2017 Jolla Ltd.

   @author Ari Saastamoinen
   @author Ismo Laitinen <ismo.laitinen@nokia.com>
   @author Yuri Zaporogets
   @author Semi Malinen <semi.malinen@nokia.com>
   @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
   @author Simo Piiroinen <simo.piiroinen@jollamobile.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * TODO: - things to glibify:
 *         -- plug-ins
 */

#include "dsme-server.h"

#include "../include/dsme/mainloop.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/dsmesock.h"
#include <dsme/protocol.h>
#include "../include/dsme/logging.h"
#include <dsme/messages.h>
#include <dsme/processwd.h>
#include "../include/dsme/oom.h"

#include <glib.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>

static void signal_handler(int  signum);
static void usage(const char *  progname);

#define DSME_PRIORITY (-1)

#define ArraySize(a) ((int)(sizeof(a)/sizeof(*a)))

#define ME "DSME: "

/**
   Usage
*/
static void usage(const char *  progname)
{
  printf("USAGE: %s -p <startup-module> "
	 "[-p <optional-module>] [...] options\n",
         progname);
  printf("Valid options:\n");
  printf(" -l  --logging     "
	 "Logging type (syslog, stderr, none)\n");
  printf(" -v  --verbosity   Log verbosity (3..7)\n");
  printf(" -t  --log-include   <file-pattern>:<func-pattern>\n");
  printf(" -e  --log-exclude   <file-pattern>:<func-pattern>\n");
#ifdef DSME_SYSTEMD_ENABLE
  printf(" -s  --systemd     "
	 "Signal systemd when initialization is done\n");
#endif
  printf("     --valgrind    Enable running with valgrind\n");
  printf(" -h  --help        Help\n");
}


/**
 * Signal_Handler
 *
 * @param sig signal_type
 */
static void signal_handler(int sig)
{
  switch (sig) {
    case SIGINT:
    case SIGTERM:
      dsme_main_loop_quit(EXIT_SUCCESS);
      break;
  }
}

static int        logging_verbosity = LOG_NOTICE;
static log_method logging_method    = LOG_METHOD_SYSLOG;

#ifdef DSME_SYSTEMD_ENABLE
static int signal_systemd = 0;
#endif

static bool valgrind_mode_enabled = false;

bool dsme_in_valgrind_mode(void)
{
    return valgrind_mode_enabled;
}

static void parse_options(int      argc,           /* in  */
                          char*    argv[],         /* in  */
                          GSList** module_names)   /* out */
{
    const char*  program_name  = argv[0];
    const char*  short_options = "dhsp:l:v:i:e:";
    const struct option long_options[] =
    {
        { "startup-module", 1, NULL, 'p' },
        { "help",           0, NULL, 'h' },
        { "verbosity",      1, NULL, 'v' },
#ifdef DSME_SYSTEMD_ENABLE
        { "systemd",        0, NULL, 's' },
#endif
        { "logging",        1, NULL, 'l' },
        { "log-include",    1, NULL, 'i' },
        { "log-exclude",    1, NULL, 'e' },
        { "valgrind",       0, NULL, 901  },
        { 0, 0, 0, 0 }
    };

    for( ;; ) {
        int opt = getopt_long(argc, argv, short_options, long_options, 0);

        if( opt == -1 )
            break;

        switch( opt ) {
        case 901:
            fprintf(stderr, ME"enabling valgrind mode");
            valgrind_mode_enabled = true;
            break;

        case 'p': /* -p or --startup-module, allow only once */
            if (module_names)
                *module_names = g_slist_append(*module_names, optarg);
            break;

        case 'l': /* -l or --logging */
            {
                const char *log_method_name[] = {
                    "none",   /* LOG_METHOD_NONE */
                    "stderr", /* LOG_METHOD_STDERR */
                    "syslog", /* LOG_METHOD_SYSLOG */
                    "file"    /* LOG_METHOD_FILE */
                };
                int i;

                for (i = 0; i < ArraySize(log_method_name); i++) {
                    if (!strcmp(optarg, log_method_name[i])) {
                        logging_method = (log_method)i;
                        break;
                    }
                }
                if (i == ArraySize(log_method_name))
                    fprintf(stderr,
                            ME "Ignoring invalid logging method %s\n",
                            optarg);
            }
            break;

        case 'v': /* -v or --verbosity */
            if (strlen(optarg) == 1 && isdigit(optarg[0]))
                logging_verbosity = atoi(optarg);
            break;

        case 'i': /* -i or --log-include */
            dsme_log_include(optarg);
            break;

        case 'e': /* -e or --log-exclude */
            dsme_log_exclude(optarg);
            break;
#ifdef DSME_SYSTEMD_ENABLE
        case 's': /* -s or --systemd */
            signal_systemd = 1;
            break;
#endif
        case 'h': /* -h or --help */
            usage(program_name);
            exit(EXIT_SUCCESS);

        case '?': /* Unrecognized option */
            exit(EXIT_FAILURE);
        }
    }

    /* check if unknown parameters were given */
    if (optind < argc) {
        usage(program_name);
        exit(EXIT_FAILURE);
    }
}

static bool receive_and_queue_message(dsmesock_connection_t* conn)
{
    bool keep_connection = true;
    dsmemsg_generic_t *msg = 0;

    DSM_MSGTYPE_SET_LOGGING_VERBOSITY *logverb;

    if( !(msg = dsmesock_receive(conn)) )
        goto EXIT;

    if( msg->type_ == DSM_MSGTYPE_PROCESSWD_PING_ID_ ) {
	dsme_log(LOG_WARNING, "got unexpected PING; "
		 "assuming it os PONG from old client");
	msg->type_ = DSM_MSGTYPE_PROCESSWD_PONG_ID_;
    }

    broadcast_internally_from_socket(msg, conn);

    if( DSMEMSG_CAST(DSM_MSGTYPE_CLOSE, msg) ) {
        keep_connection = false;
    }
    else if( DSMEMSG_CAST(DSM_MSGTYPE_ADD_LOGGING_INCLUDE, msg) ) {
        const char *pattern = DSMEMSG_EXTRA(msg);
        dsme_log_include(pattern);
    }
    else if( DSMEMSG_CAST(DSM_MSGTYPE_ADD_LOGGING_EXCLUDE, msg) ) {
        const char *pattern = DSMEMSG_EXTRA(msg);
        dsme_log_exclude(pattern);
    }
    else if( DSMEMSG_CAST(DSM_MSGTYPE_USE_LOGGING_DEFAULTS, msg) ) {
        dsme_log_clear_rules();
    }
    else if( (logverb = DSMEMSG_CAST(DSM_MSGTYPE_SET_LOGGING_VERBOSITY, msg)) )
    {
        dsme_log_set_verbosity(logverb->verbosity);
    }

EXIT:
    free(msg);

    return keep_connection;
}

/**
  @todo Possibility to alter priority of initial module somehow
  */
int main(int argc, char *argv[])
{
  int exit_code = EXIT_FAILURE;
  GSList* module_names = 0;

  if( !dsme_log_init() )
      goto EXIT;

  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP,  signal_handler);
  signal(SIGPIPE, signal_handler);

  /* protect DSME from oom; notice that this must be done before any
   * calls to pthread_create() in order to have all threads protected
   */
  if (!protect_from_oom()) {
      fprintf(stderr, ME "Couldn't protect from oom: %s\n", strerror(errno));
  }

  /* Set static priority for RT-scheduling */
  int scheduler;
  struct sched_param param;
  scheduler = sched_getscheduler(0);
  if(sched_getparam(0, &param) == 0) {
      param.sched_priority = sched_get_priority_min(scheduler);
      if(sched_setparam(0, &param) != 0) {
          fprintf(stderr, ME "Couldn't set static priority: %s\n", strerror(errno));
      }
  }
  else {
      fprintf(stderr, ME "Couldn't get scheduling params: %s\n", strerror(errno));
  }

  /* Set nice value for cases when dsme-server is not under RT-scheduling*/
  if (setpriority(PRIO_PROCESS, 0, DSME_PRIORITY) != 0) {
      fprintf(stderr, ME "Couldn't set dynamic priority: %s\n", strerror(errno));
  }

  parse_options(argc, argv, &module_names);

  if (!module_names) {
      usage(argv[0]);
      goto EXIT;
  }

  dsme_log_open(logging_method,
                logging_verbosity,
                0,
                "DSME",
                0,
                0,
                "/var/log/dsme.log");

  /* load modules */
  if (!modulebase_init(module_names)) {
      g_slist_free(module_names);
      goto EXIT;
  }
  g_slist_free(module_names);

  /* init socket communication */
  if (dsmesock_listen(receive_and_queue_message) == -1) {
      dsme_log(LOG_CRIT, "Error creating DSM socket: %s", strerror(errno));
      goto EXIT;
  }

  /* set running directory */
  if (chdir("/") == -1) {
      dsme_log(LOG_CRIT, "chdir failed: %s", strerror(errno));
      goto EXIT;
  }
#ifdef DSME_SYSTEMD_ENABLE
  /* Inform main process that we are ready 
   * Main process will inform systemd
   */
  if (signal_systemd) {
      kill(getppid(), SIGUSR1);
  }
#endif
  dsme_log(LOG_DEBUG, "Entering main loop");
  dsme_main_loop_run(process_message_queue);

  /* To eaze shutdown analysis, always log when dsme exits */
  dsme_log(LOG_WARNING, "Exited main loop, quitting");

  dsmesock_shutdown();

  modulebase_shutdown();

  exit_code = dsme_main_loop_exit_code();

EXIT:
  dsme_log_close();
  return exit_code;
}
