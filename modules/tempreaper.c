/**
   @file tempreaper.c

   DSME module to clean up orphaned temporary files.
   <p>
   Copyright (C) 2011 Nokia Corporation.

   @author Matias Muhonen <ext-matias.muhonen@nokia.com>

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

#ifndef __cplusplus
#define _GNU_SOURCE
#endif

#include "diskmonitor.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"

#include <errno.h>
#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define GETPWNAM_BUFLEN 1024
#define MIN_PRIORITY 5
#define RPDIR_PATH DSME_SBIN_PATH"/rpdir"

#define LOGPFIX "tempreaper: "

/* DSME's own (threaded) logging must not be used from the forked
 * child process. Logging to stderr will be caught to journal by
 * systemd and also gets attributed to the dsme.service unit.
 */
#define childlog(FMT, ARGS...) fprintf(stderr, FMT"\n", ## ARGS)

static pid_t reaper_pid = -1;

static bool drop_privileges(void)
{
    static const char * const users[] = {
        "nemo",   /* Default user in mer / sailfish devices */
        "user",   /* Default used in maemo / meego devices */
        "nobody", /* Standard limited privileges "user" */
        0
    };

    bool success = false;

    struct passwd  pwd;
    char           buf[GETPWNAM_BUFLEN];

    for( size_t i = 0; ; ++i ) {
        const char *username = users[i];

        if( username == 0 ) {
            childlog(LOGPFIX"unable to retrieve passwd entry");
            goto out;
        }

        memset(buf, 0, sizeof buf);
        memset(&pwd, 0, sizeof pwd);

        struct passwd *pwd_found = 0;
        getpwnam_r(username, &pwd, buf, GETPWNAM_BUFLEN, &pwd_found);
        if( pwd_found )
            break;
    }

    if (setgid(pwd.pw_gid) != 0) {
        childlog(LOGPFIX"setgid() failed with pw_gid %d (%m)",
                 (int)pwd.pw_gid);
        goto out;
    }
    if (setuid(pwd.pw_uid) != 0) {
        childlog(LOGPFIX"setuid() failed with pw_uid %d (%m)",
                 (int)pwd.pw_uid);
        goto out;
    }

    success = true;

out:
    return success;
}

static void reaper_child_process(void)
{
    /* The tempdirs we will cleanup are given as an argument.
     */
    const char *argv[] =
    {
        "rpdir",
        "/tmp",
        "/run/log",
        "/var/log",
        "/var/cache/core-dumps",
        0
    };

    closelog();

    for( int fd = 3; fd < 1024; ++fd )
        close(fd);

    /* Child; set a reasonably low priority, DSME runs with the priority -1
     so we don't want to use the inherited priority */
    if (setpriority(PRIO_PROCESS, 0, MIN_PRIORITY) != 0) {
        childlog(LOGPFIX"setpriority() failed");
        _exit(EXIT_FAILURE);
    }

    if (!drop_privileges()) {
        childlog(LOGPFIX"drop_privileges() failed");
        _exit(EXIT_FAILURE);
    }

    execv(RPDIR_PATH, (char * const *)argv);
    childlog(LOGPFIX"execv failed. path: " RPDIR_PATH);
}

static pid_t reaper_process_new(void)
{
    fflush(0);

    pid_t pid = fork();

    if (pid == 0) {
        reaper_child_process();
        _exit(EXIT_FAILURE);
    }

    if (pid == -1) {
        /* error */
        dsme_log(LOG_CRIT, LOGPFIX"fork() failed: %s", strerror(errno));
    } else {
        /* parent */
    }
    return pid;
}

static void temp_reaper_finished(GPid pid, gint status, gpointer unused)
{
    reaper_pid = -1;

    if (WEXITSTATUS(status) != 0) {
        dsme_log(LOG_WARNING, LOGPFIX"reaper process failed (PID %d).",
                 (int)pid);
    }
    else {
        dsme_log(LOG_INFO, LOGPFIX"reaper process finished (PID %d).",
                 (int)pid);
    }
}

static bool temp_reaper_applicable(const char *mount_path)
{
    /* TODO: we should actually check the mount entries to figure out
       on which mount(s) temp_dirs are mounted on. We now assume that all
       temp_dirs are mounted on the root / few other partitions. */
    static const char * const lut[] = {
        "/",
        "/tmp",
        "/var",
        "/run",
        0
    };

    bool applicable = false;

    if( !mount_path )
        goto EXIT;

    for( size_t i = 0; !applicable && lut[i]; ++i )
        applicable = !strcmp(lut[i], mount_path);

EXIT:
    return applicable;
}

DSME_HANDLER(DSM_MSGTYPE_DISK_SPACE, conn, msg)
{
    /* Only diskspace low conditions should trigger temp reaper */
    switch( msg->diskspace_state ) {
    case DISKSPACE_STATE_UNDEF:
    case DISKSPACE_STATE_NORMAL:
        goto EXIT;

    default:
        break;
    }

    /* Ignore mountpoints temp reaper is not going to touch */
    const char *mount_path = DSMEMSG_EXTRA(msg);

    if( !temp_reaper_applicable(mount_path) )
        goto EXIT;

    /* Allow only one reaper process - it will scan all configured
     * directory trees regardless of which mountpoint triggered the
     * execution. */
    if( reaper_pid != -1 ) {
        dsme_log(LOG_DEBUG, LOGPFIX"reaper process already running (PID %d). Return.",
                 (int)reaper_pid);
        goto EXIT;
    }

    reaper_pid = reaper_process_new();

    if( reaper_pid != -1 ) {
        g_child_watch_add(reaper_pid, temp_reaper_finished, temp_reaper_finished);

        dsme_log(LOG_INFO, LOGPFIX"reaper process started (PID %d).",
                 (int)reaper_pid);
    }

EXIT:
    return;
}

module_fn_info_t message_handlers[] = {
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DISK_SPACE),
    { 0 }
};

void module_init(module_t* module)
{
    dsme_log(LOG_DEBUG, "tempreaper.so loaded");
}

void module_fini(void)
{
    if (reaper_pid != -1) {
        dsme_log(LOG_INFO, LOGPFIX"killing temp reaper with pid %i", reaper_pid);
        kill(reaper_pid, SIGKILL);
    }

    dsme_log(LOG_DEBUG, "tempreaper.so unloaded");
}
