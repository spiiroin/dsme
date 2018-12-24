/**
 * @file dsmetool.c
 *
 * Dsmetool can be used to send commands to DSME.
 * <p>
 * Copyright (C) 2004-2011 Nokia Corporation.
 * Copyright (C) 2013-2017 Jolla Ltd.
 *
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
 * @author Semi Malinen <semi.malinen@nokia.com>
 * @author Matias Muhonen <ext-matias.muhonen@nokia.com>
 * @author Jarkko Nikula <jarkko.nikula@jollamobile.com>
 * @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 *
 * This file is part of Dsme.
 *
 * Dsme is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * Dsme is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../modules/lifeguard.h"
#include "../modules/dbusproxy.h"
#include "../modules/state-internal.h"
#include "../include/dsme/logging.h"

#include <dsme/state.h>
#include <dsme/protocol.h>

#include <linux/rtc.h>

#include <sys/types.h>
#include <sys/ioctl.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>

#define STRINGIFY(x)  STRINGIFY2(x)
#define STRINGIFY2(x) #x

/* ========================================================================= *
 * DIAGNOSTIC_OUTPUT
 * ========================================================================= */

static bool log_verbose = false;

#define log_error(FMT,ARGS...)\
     fprintf(stderr, "E: "FMT"\n", ## ARGS)

#define log_debug(FMT,ARGS...)\
     do {\
         if( log_verbose )\
             fprintf(stderr, "D: "FMT"\n", ## ARGS);\
     }while(0)

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTILS
 * ------------------------------------------------------------------------- */

static const char        *dsme_state_repr(dsme_state_t state);
static int64_t            boottime_get_ms(void);

/* ------------------------------------------------------------------------- *
 * DSMEIPC_CONNECTION
 * ------------------------------------------------------------------------- */

#define DSMEIPC_WAIT_DEFAULT -1

static void               dsmeipc_connect(void);
static void               dsmeipc_disconnect(void);
static void               dsmeipc_send_full(const void *msg, const void *data, size_t size);
static void               dsmeipc_send(const void *msg);
static void               dsmeipc_send_with_string(const void *msg, const char *str);
static void               dsmeipc_send_with_extra(const void *msg, size_t size, const void *extra);
static bool               dsmeipc_wait(int64_t *tmo);
static dsmemsg_generic_t *dsmeipc_read(void);

/* ------------------------------------------------------------------------- *
 * DSME_OPTIONS
 * ------------------------------------------------------------------------- */

static void               xdsme_query_version(bool testmode);
static void               xdsme_query_runlevel(void);
static void               xdsme_request_dbus_connect(void);
static void               xdsme_request_dbus_disconnect(void);
static void               xdsme_request_reboot(void);
static void               xdsme_request_shutdown(void);
static void               xdsme_request_powerup(void);
static void               xdsme_request_runlevel(const char *runlevel);
static void               xdsme_request_loglevel(unsigned level);
static void               xdsme_request_log_include(const char *pattern);
static void               xdsme_request_log_exclude(const char *pattern);
static void               xdsme_request_log_defaults(void);

static int                xdsme_request_process_start(const char*       command,
                                                      process_actions_t action,
                                                      int               maxcount,
                                                      int               maxperiod,
                                                      uid_t             uid,
                                                      gid_t             gid,
                                                      int               nice,
                                                      int               oom_adj);

static int                xdsme_request_process_stop(const char* command, int signal);

/* ------------------------------------------------------------------------- *
 * RTC_OPTIONS
 * ------------------------------------------------------------------------- */

static bool               rtc_clear_alarm(void);

/* ------------------------------------------------------------------------- *
 * OPTION_PARSING
 * ------------------------------------------------------------------------- */

static unsigned           parse_unsigned(char *str);
static unsigned           parse_loglevel(char *str);
static const char        *parse_runlevel(char *str);

static void               output_usage(const char *name);

/* ------------------------------------------------------------------------- *
 * MAIN_ENTRY_POINT
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[]);

/* ========================================================================= *
 * MISC_UTILS
 * ========================================================================= */

static int64_t boottime_get_ms(void)
{
        int64_t res = 0;

        struct timespec ts;

        if( clock_gettime(CLOCK_BOOTTIME, &ts) == 0 ) {
                res = ts.tv_sec;
                res *= 1000;
                res += ts.tv_nsec / 1000000;
        }

        return res;
}

static const char *dsme_state_repr(dsme_state_t state)
{
    const char *repr = "UNKNOWN";

    switch( state ) {
    case DSME_STATE_SHUTDOWN:   repr = "SHUTDOWN"; break;
    case DSME_STATE_USER:       repr = "USER";     break;
    case DSME_STATE_ACTDEAD:    repr = "ACTDEAD";  break;
    case DSME_STATE_REBOOT:     repr = "REBOOT";   break;
    case DSME_STATE_BOOT:       repr = "BOOT";     break;
    case DSME_STATE_NOT_SET:    repr = "NOT_SET";  break;
    case DSME_STATE_TEST:       repr = "TEST";     break;
    case DSME_STATE_MALF:       repr = "MALF";     break;
    case DSME_STATE_LOCAL:      repr = "LOCAL";    break;
    default: break;
    }

    return repr;
}

/* ========================================================================= *
 * DSMEIPC_CONNECTION
 * ========================================================================= */

static dsmesock_connection_t *dsmeipc_conn = 0;

static void dsmeipc_connect(void)
{
    /* Already connected? */
    if( dsmeipc_conn )
        goto EXIT;

    if( !(dsmeipc_conn = dsmesock_connect()) ) {
        log_error("dsmesock_connect: %m");
        exit(EXIT_FAILURE);
    }

    log_debug("connected");

    /* This gives enough time for DSME to check
     * the socket permissions before we close the socket
     * connection */
    (void)xdsme_query_version(true);

EXIT:
    return;
}

static void dsmeipc_disconnect(void)
{
    if( !dsmeipc_conn )
        goto EXIT;

    log_debug("disconnecting");
    dsmesock_close(dsmeipc_conn), dsmeipc_conn = 0;

EXIT:
    return;
}

static void dsmeipc_send_full(const void *msg_, const void *data, size_t size)
{
    const dsmemsg_generic_t *msg = msg_;

    dsmeipc_connect();

    log_debug("send: %s", dsmemsg_id_name(msg->type_));

    if( dsmesock_send_with_extra(dsmeipc_conn, msg, size, data) == -1 ) {
        log_error("dsmesock_send: %m");
        exit(EXIT_FAILURE);
    }

}

static void dsmeipc_send(const void *msg)
{
    dsmeipc_send_full(msg, 0, 0);
}

static void dsmeipc_send_with_string(const void *msg, const char *str)
{
    dsmeipc_send_full(msg, str, strlen(str) + 1);
}

static void
dsmeipc_send_with_extra(const void* msg, size_t size, const void *extra)
{
    dsmeipc_send_full(msg, extra, size);
}

static bool dsmeipc_wait(int64_t *tmo)
{
    bool have_input = false;
    int  wait_input = 0;

    struct pollfd pfd =
    {
        .fd = dsmeipc_conn->fd,
        .events = POLLIN,
    };

    int64_t now = boottime_get_ms();

    /* Called with uninitialized timeout; use now + 5 seconds */
    if( *tmo == DSMEIPC_WAIT_DEFAULT )
        *tmo = now + 5000;

    /* If timeout is in the future, wait for input - otherwise
     * just check if there already is something to read */
    if( *tmo > now )
        wait_input = (int)(now - *tmo);

    if( poll(&pfd, 1, wait_input) == 1 )
        have_input = true;

    return have_input;
}

static dsmemsg_generic_t *dsmeipc_read(void)
{
    dsmemsg_generic_t *msg = dsmesock_receive(dsmeipc_conn);
    if( !msg ) {
        log_error("dsmesock_receive: %m");
        exit(EXIT_FAILURE);
    }

    log_debug("recv: %s", dsmemsg_id_name(msg->type_));

    return msg;
}

/* ========================================================================= *
 * DSME_OPTIONS
 * ========================================================================= */

static void xdsme_query_version(bool testmode)
{
    DSM_MSGTYPE_GET_VERSION req =
          DSME_MSG_INIT(DSM_MSGTYPE_GET_VERSION);

    int64_t timeout = DSMEIPC_WAIT_DEFAULT;
    char   *version = 0;

    dsmeipc_send(&req);

    while( dsmeipc_wait(&timeout) ) {
        dsmemsg_generic_t *msg = dsmeipc_read();

        DSM_MSGTYPE_DSME_VERSION *rsp =
            DSMEMSG_CAST(DSM_MSGTYPE_DSME_VERSION, msg);

        if( rsp ) {
            const char *data = DSMEMSG_EXTRA(rsp);
            size_t      size = DSMEMSG_EXTRA_SIZE(rsp);
            version = strndup(data, size);
        }

        free(msg);

        if( rsp )
            break;
    }

    if( !testmode ) {
        printf("dsmetool version: %s\n", STRINGIFY(PRG_VERSION));
        printf("DSME version: %s\n", version ?: "unknown");
    }

    free(version);
}

static void xdsme_query_runlevel(void)
{
    DSM_MSGTYPE_STATE_QUERY req = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);

    int64_t      timeout = DSMEIPC_WAIT_DEFAULT;
    dsme_state_t state   = DSME_STATE_NOT_SET;

    dsmeipc_send(&req);

    while( dsmeipc_wait(&timeout) ) {
        dsmemsg_generic_t *msg = dsmeipc_read();
        DSM_MSGTYPE_STATE_CHANGE_IND *rsp =
            DSMEMSG_CAST(DSM_MSGTYPE_STATE_CHANGE_IND, msg);

        if( rsp )
            state = rsp->state;

        free(msg);

        if( rsp )
            break;
    }

    printf("%s\n", dsme_state_repr(state));
}

static void xdsme_request_dbus_connect(void)
{
    DSM_MSGTYPE_DBUS_CONNECT req = DSME_MSG_INIT(DSM_MSGTYPE_DBUS_CONNECT);

    dsmeipc_send(&req);
}

static void xdsme_request_dbus_disconnect(void)
{
    DSM_MSGTYPE_DBUS_DISCONNECT req =
        DSME_MSG_INIT(DSM_MSGTYPE_DBUS_DISCONNECT);

    dsmeipc_send(&req);
}

static void xdsme_request_reboot(void)
{
    DSM_MSGTYPE_REBOOT_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_REBOOT_REQ);

    dsmeipc_send(&req);
}

static void xdsme_request_shutdown(void)
{
    DSM_MSGTYPE_SHUTDOWN_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_SHUTDOWN_REQ);

    dsmeipc_send(&req);
}

static void xdsme_request_powerup(void)
{
    DSM_MSGTYPE_POWERUP_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_POWERUP_REQ);

    dsmeipc_send(&req);
}

static void xdsme_request_runlevel(const char *runlevel)
{
    DSM_MSGTYPE_TELINIT req = DSME_MSG_INIT(DSM_MSGTYPE_TELINIT);

    dsmeipc_send_with_string(&req, runlevel);
}

static void xdsme_request_loglevel(unsigned level)
{
    DSM_MSGTYPE_SET_LOGGING_VERBOSITY req =
        DSME_MSG_INIT(DSM_MSGTYPE_SET_LOGGING_VERBOSITY);
    req.verbosity = level;

    dsmeipc_send(&req);
}

static void xdsme_request_log_include(const char *pattern)
{
    DSM_MSGTYPE_ADD_LOGGING_INCLUDE req = DSME_MSG_INIT(DSM_MSGTYPE_ADD_LOGGING_INCLUDE);

    dsmeipc_send_with_string(&req, pattern);
}

static void xdsme_request_log_exclude(const char *pattern)
{
    DSM_MSGTYPE_ADD_LOGGING_EXCLUDE req = DSME_MSG_INIT(DSM_MSGTYPE_ADD_LOGGING_EXCLUDE);

    dsmeipc_send_with_string(&req, pattern);
}

static void xdsme_request_log_defaults(void)
{
    DSM_MSGTYPE_USE_LOGGING_DEFAULTS req = DSME_MSG_INIT(DSM_MSGTYPE_USE_LOGGING_DEFAULTS);

    dsmeipc_send(&req);
}

static int
xdsme_request_process_start(const char*       command,
                            process_actions_t action,
                            int               maxcount,
                            int               maxperiod,
                            uid_t             uid,
                            gid_t             gid,
                            int               nice,
                            int               oom_adj)
{
    DSM_MSGTYPE_PROCESS_START        msg =
        DSME_MSG_INIT(DSM_MSGTYPE_PROCESS_START);
    DSM_MSGTYPE_PROCESS_STARTSTATUS* retmsg;
    fd_set rfds;
    int    ret;

    msg.action         = action;
    msg.restart_limit  = maxcount;
    msg.restart_period = maxperiod;
    msg.uid            = uid;
    msg.gid            = gid;
    msg.nice           = nice;
    msg.oom_adj        = oom_adj;
    dsmeipc_send_with_extra(&msg, strlen(command) + 1, command);

    while (dsmeipc_conn) {
        FD_ZERO(&rfds);
        FD_SET(dsmeipc_conn->fd, &rfds);

        ret = select(dsmeipc_conn->fd+1, &rfds, NULL, NULL, NULL);
        if (ret == -1) {
            printf("Error in select()\n");
            return -1;
        }

        retmsg = (DSM_MSGTYPE_PROCESS_STARTSTATUS*)dsmesock_receive(dsmeipc_conn);

        if (DSMEMSG_CAST(DSM_MSGTYPE_CLOSE, retmsg)) {
            printf("Dsme closed socket\n");
            free(retmsg);
            return -1;
        }

        if (DSMEMSG_CAST(DSM_MSGTYPE_PROCESS_STARTSTATUS, retmsg) == 0) {
            printf("Received invalid message (type: %i)\n",
                   dsmemsg_id((dsmemsg_generic_t*)retmsg));
            free(retmsg);
            continue;
        }

        /* printf("PID=%d, startval=%d\n", retmsg->pid, retmsg->return_value); */

        ret = retmsg->status;
        free(retmsg);

        return ret;
    }

    return -1;
}

static int
xdsme_request_process_stop(const char* command, int signal)
{
    DSM_MSGTYPE_PROCESS_STOP        msg =
        DSME_MSG_INIT(DSM_MSGTYPE_PROCESS_STOP);
    DSM_MSGTYPE_PROCESS_STOPSTATUS* retmsg;
    fd_set rfds;
    int    ret;

    msg.signal = signal;
    dsmeipc_send_with_extra(&msg, strlen(command) + 1, command);

    while (dsmeipc_conn) {
        FD_ZERO(&rfds);
        FD_SET(dsmeipc_conn->fd, &rfds);
        struct timeval tv;

        tv.tv_sec  = 5;
        tv.tv_usec = 0;

        ret = select(dsmeipc_conn->fd+1, &rfds, NULL, NULL, &tv);
        if (ret == -1) {
            printf("Error in select()\n");
            return -1;
        }
        if (ret == 0) {
            printf("Timeout waiting for process stop status from DSME\n");
            dsmeipc_disconnect();
            return -1;
        }

        retmsg = (DSM_MSGTYPE_PROCESS_STOPSTATUS*)dsmesock_receive(dsmeipc_conn);

        if (DSMEMSG_CAST(DSM_MSGTYPE_CLOSE, retmsg)) {
            printf("Dsme closed socket\n");
            free(retmsg);
            return -1;
        }

        if (DSMEMSG_CAST(DSM_MSGTYPE_PROCESS_STOPSTATUS, retmsg) == 0) {
            printf("Received invalid message (type: %i)\n",
                   dsmemsg_id((dsmemsg_generic_t*)retmsg));
            free(retmsg);
            continue;
        }

        if (retmsg->killed) {
            ret = EXIT_SUCCESS;
        } else {
            printf("Process not killed: %s\n",
                   (const char*)DSMEMSG_EXTRA(retmsg));
            ret = EXIT_FAILURE;
        }

        free(retmsg);

        return ret;
    }

    return -1;
}

/* ========================================================================= *
 * RTC_OPTIONS
 * ========================================================================= */

/** Clear possible RTC alarm wakeup */
static bool rtc_clear_alarm(void)
{
    static const char rtc_path[] = "/dev/rtc0";

    bool cleared = false;
    int  rtc_fd  = -1;

    struct rtc_wkalrm alrm;

    if ((rtc_fd = open(rtc_path, O_RDONLY)) == -1) {
        /* TODO: If open fails reason is most likely that dsme is running
         * and has opened rtc. In that case we should send message to dsme
         * and ask it to do the clearing. This functionality is not now
         * needed because rtc alarms are cleared only during preinit and
         * there dsme is not running. But to make this complete, that
         * functionality should be added.
         */
        log_error("Failed to open %s: %m", rtc_path);
        goto EXIT;
    }

    memset(&alrm, 0, sizeof(alrm));
    if (ioctl(rtc_fd, RTC_WKALM_RD, &alrm) == -1) {
        log_error("Failed to read rtc alarms %s: %s: %m", rtc_path,
                  "RTC_WKALM_RD");
        goto EXIT;
    }
    printf("Alarm was %s at %d.%d.%d %02d:%02d:%02d UTC\n",
           alrm.enabled ? "Enabled" : "Disabled",
           1900+alrm.time.tm_year, 1+alrm.time.tm_mon, alrm.time.tm_mday,
           alrm.time.tm_hour, alrm.time.tm_min, alrm.time.tm_sec);

    /* Kernel side bug in Jolla phone?
     * We need to enable alarm first before we can disable it.
     */
    alrm.enabled = 1;
    alrm.pending = 0;
    if (ioctl(rtc_fd, RTC_WKALM_SET, &alrm) == -1)
        log_error("Failed to enable rtc alarms %s: %s: %m", rtc_path,
                  "RTC_WKALM_SET");

    /* Now disable the alarm */
    alrm.enabled = 0;
    alrm.pending = 0;
    if (ioctl(rtc_fd, RTC_WKALM_SET, &alrm) == -1) {
        log_error("Failed to clear rtc alarms %s: %s: %m", rtc_path,
                  "RTC_WKALM_SET");
        goto EXIT;
    }

    printf("RTC alarm cleared ok\n");
    cleared = true;

EXIT:
    if( rtc_fd != -1 )
        close(rtc_fd);

    return cleared;
}

/* ========================================================================= *
 * OPTION_PARSING
 * ========================================================================= */

static unsigned parse_unsigned(char *str)
{
    char     *pos = str;
    unsigned  val = strtoul(str, &pos, 0);

    if( pos == str || *pos != 0 ) {
        log_error("%s: not a valid unsigned integer", str);
        exit(EXIT_FAILURE);
    }

    return val;
}

static unsigned parse_loglevel(char *str)
{
    unsigned val = parse_unsigned(str);

    if( val > 7 ) {
        log_error("%s: not a valid log level", str);
        exit(EXIT_FAILURE);
    }

    return val;
}

static const char *parse_runlevel(char *str)
{
    static const char * const lut[] =
    {
        "SHUTDOWN", "USER", "ACTDEAD", "REBOOT", 0
    };

    for( size_t i = 0;  ; ++i ) {

        if( lut[i] == 0 ) {
            log_error("%s: not a valid run level", str);
            exit(EXIT_FAILURE);
        }

        if( !strcasecmp(lut[i], str) )
            return lut[i];
    }
}

static void output_usage(const char *name)
{
    printf("USAGE: %s <options>\n", name);
    printf(
"\n"
"  -h --help                       Print usage information\n"
"  -v --version                    Print the versions of DSME and dsmetool\n"
"  -V --verbose                    Make dsmetool more verbose\n"
"  -l --loglevel <0..7>            Change DSME's logging verbosity\n"
"  -i --log-include <file:func>    Include logging from matching functions\n"
"  -e --log-exclude <file:func>    Exclude logging from matching functions\n"
"  -L --log-defaults               Clear include/exclude patterns\n"
"\n"
"  -g --get-state                  Print device state, i.e. one of\n"
"                                   SHUTDOWN USER ACTDEAD REBOOT BOOT\n"
"                                   TEST MALF LOCAL NOT_SET or UNKNOWN\n"
"  -b --reboot                     Reboot the device\n"
"  -o --shutdown                   Shutdown (or switch to ACTDEAD)\n"
"  -u --powerup                    Switch from ACTDEAD to USER state\n"
"  -t --telinit <runlevel name>    Change runlevel, valid names are:\n"
"                                   SHUTDOWN USER ACTDEAD REBOOT\n"
"\n"
"  -c --clear-rtc                  Clear RTC alarms\n"
"\n"
"  -d --start-dbus                 Start DSME's D-Bus services\n"
"  -s --stop-dbus                  Stop DSME's D-Bus services\n"
"\n"

           /* lifeguard */
"  -r --start-reset=<cmd>          Start a process\n"
"                                   (on process exit, do SW reset)\n"
"  -t --start-restart=<cmd>        Start a process\n"
"                                   (on process exit, restart max N times,\n"
"                                    then do SW reset)\n"
"  -f --start-restart-fail=<cmd>   Start a process\n"
"                                   (on process exit, restart max N times,\n"
"                                    then stop trying)\n"
"  -o --start-once=<cmd>           Start a process only once\n"
"  -c --max-count=N                Restart process only maximum N times\n"
"                                   in defined period of time\n"
"                                   (the default is 10 times in 60 s)\n"
"  -T --count-time=N               Set period for restart check\n"
"                                   (default 60 s)\n"
"  -k --stop=<cmd>                 Stop a process started with cmd\n"
"                                   (if started with dsme)\n"
"  -S --signal=N                   Set used signal for stopping processes\n"
"  -u --uid=N                      Set used uid for started process\n"
"  -U --user=<username>            Set used uid for started process\n"
"                                   from username\n"
"  -g --gid=N                      Set used gid for started process\n"
"  -G --group=<groupname>          Set used gid for started process\n"
"                                   from groupname\n"
"  -n --nice=N                     Set used nice value (priority)\n"
"                                   for started process\n"
"  -m --oom-adj=N                  Set oom_adj value for started process\n"

          );
}

/* ========================================================================= *
 * MAIN_ENTRY_POINT
 * ========================================================================= */

int main(int argc, char *argv[])
{
    const char *program_name  = argv[0];
    int         retval        = EXIT_FAILURE;
    const char *short_options =
	//"hdsbvact:l:guoVi:e:L"
	"hdsbval:Vi:e:L"
	"r:t:f:o:c:T:k:S:u:g:U:G:n:m:"
	;
    const struct option long_options[] = {
        {"help",         no_argument,       NULL, 'h'},
        {"start-dbus",   no_argument,       NULL, 'd'},
        {"stop-dbus",    no_argument,       NULL, 's'},
        {"reboot",       no_argument,       NULL, 'b'},
        {"version",      no_argument,       NULL, 'v'},
        {"clear-rtc",    no_argument,       NULL, 'c'|0x100},
        {"get-state",    no_argument,       NULL, 'g'|0x100},
        {"powerup",      no_argument,       NULL, 'u'|0x100},
        {"shutdown",     no_argument,       NULL, 'o'|0x100},
        {"telinit",      required_argument, NULL, 't'|0x100},
        {"loglevel",     required_argument, NULL, 'l'},
        {"log-include",  required_argument, NULL, 'i'},
        {"log-exclude",  required_argument, NULL, 'e'},
        {"log-defaults", no_argument,       NULL, 'L'},
        {"verbose",      no_argument,       NULL, 'V'},

        /* lifeguard */
        {"start-reset",        1, NULL, 'r'},
        {"start-restart",      1, NULL, 't'}, //
        {"start-restart-fail", 1, NULL, 'f'},
        {"start-once",         1, NULL, 'o'}, //
        {"max-count",          1, NULL, 'c'}, //
        {"count-time",         1, NULL, 'T'},
        {"stop",               1, NULL, 'k'},
        {"signal",             1, NULL, 'S'},
        {"uid",                1, NULL, 'u'}, //
        {"gid",                1, NULL, 'g'}, //
        {"user",               1, NULL, 'U'},
        {"group",              1, NULL, 'G'},
        {"nice",               1, NULL, 'n'},
        {"oom-adj",            1, NULL, 'm'},

        {0, 0, 0, 0}
    };

    /* Treat no args as if --help option were given */
    if( argc == 1 ) {
        output_usage(program_name);
        goto DONE;
    }

    /* Lifeguard values */
    int         maxcount      = 10;
    int         countperiod   = 60;
    int         signum        = SIGTERM;
    uid_t       uid           = getuid();
    gid_t       gid           = getgid();
    int         group_set     = 0;
    const char* username      = 0;
    const char* group         = 0;
    int         nice          = 0;
    int         oom_adj       = 0;
    enum { NONE, START, STOP } action = NONE;
    const char* program       = "";
    process_actions_t policy  = ONCE;

    /* Handle options */
    for( ;; ) {
        int opt = getopt_long(argc, argv, short_options, long_options, 0);

        if( opt == -1 )
            break;

        switch( opt ) {
        case 'd':
            xdsme_request_dbus_connect();
            break;

        case 's':
            xdsme_request_dbus_disconnect();
            break;

        case 'b':
            xdsme_request_reboot();
            break;

        case 'u'|0x100:
            xdsme_request_powerup();
            break;

        case 'o'|0x100:
            xdsme_request_shutdown();
            break;

        case 'v':
            xdsme_query_version(false);
            break;

        case 't'|0x100:
            xdsme_request_runlevel(parse_runlevel(optarg));
            break;

        case 'g'|0x100:
            xdsme_query_runlevel();
            break;

        case 'l':
            xdsme_request_loglevel(parse_loglevel(optarg));
            break;

        case 'i':
            xdsme_request_log_include(optarg);
            break;

        case 'e':
            xdsme_request_log_exclude(optarg);
            break;

        case 'L':
            xdsme_request_log_defaults();
            break;

        case 'c'|0x100:
            if( !rtc_clear_alarm() )
                goto EXIT;
            break;

        case 'V':
            log_verbose = true;
            break;

        case 'h':
            output_usage(program_name);
            goto DONE;

        case '?':
            fprintf(stderr, "(use --help for instructions)\n");
            goto EXIT;

            /* - - - - - - - - - - - - - - - - - - - *
             * lifeguard
             * - - - - - - - - - - - - - - - - - - - */

        case 'k':
            program = optarg;
            action = STOP;
            break;
        case 'S':
            signum = atoi(optarg);
            break;
        case 'u':
            uid = atoi(optarg);
            break;
        case 'U':
            username = optarg;
            break;
        case 'g':
            gid = atoi(optarg);
            group_set = 1;
            break;
        case 'G':
            group = optarg;
            group_set = 1;
            break;
        case 'n':
            nice = atoi(optarg);
            break;
        case 'm':
            oom_adj = atoi(optarg);
            break;
        case 'c':
            maxcount = atoi(optarg);
            break;
        case 'T':
            countperiod = atoi(optarg);
            break;
        case 'r':
            program = optarg;
            policy = RESET;
            action = START;
            break;
        case 't':
            program = optarg;
            policy = RESPAWN;
            action = START;
            break;
        case 'f':
            program = optarg;
            policy = RESPAWN_FAIL;
            action = START;
            break;
        case 'o':
            program = optarg;
            policy = ONCE;
            action = START;
            break;

        }
    }

    /* Complain about excess args */
    if( optind < argc ) {
        fprintf(stderr, "%s: unknown argument\n", argv[optind]);
        fprintf(stderr, "(use --help for instructions)\n");
        goto EXIT;
    }

    /* Lifeguard */
    if (username != 0) {
        struct passwd *pw_entry = getpwnam(username);

        if (uid != getuid())
            printf("warning, username overrides specified uid\n");

        if (!pw_entry) {
            printf("Can't get a UID for username: %s\n", username);
            return EXIT_FAILURE;
        }
        uid = pw_entry->pw_uid;
    }

    if (group != 0) {
        struct group* gr_entry = getgrnam(group);

        if (gid != getgid())
            printf("warning, group overrides specified gid\n");

        if (!gr_entry) {
            printf("Can't get a GID for groupname: %s\n", group);
            return EXIT_FAILURE;
        }
        gid = gr_entry->gr_gid;
    }

    if (uid != getuid() && !group_set) {
        struct passwd *pw_entry = getpwuid(uid);
        if (!pw_entry) {
            printf("Can't get pwentry for UID: %d\n", uid);
            return EXIT_FAILURE;
        }
        if (pw_entry->pw_gid)
            gid = pw_entry->pw_gid;
        else
            printf("Default group not found for UID: %d. Using current one.\n", uid);
    }

    if( action == START ) {
        int rc = xdsme_request_process_start(program,
                                             policy,
                                             maxcount,
                                             countperiod,
                                             uid,
                                             gid,
                                             nice,
                                             oom_adj);
        if( rc != 0 )
            goto EXIT;
    } else if (action == STOP) {
        int rc = xdsme_request_process_stop(program, signum);
        if( rc != 0 )
            goto EXIT;
    }

DONE:
    retval = EXIT_SUCCESS;

EXIT:

    dsmeipc_disconnect();

    return retval;
}
