/**
   @file runlevel.c

   DSME internal runlevel control
   <p>
   Copyright (c) 2009 - 2010 Nokia Corporation.
   Copyright (c) 2012 - 2020 Jolla Ltd.
   Copyright (c) 2020 Open Mobile Platform LLC.

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

#include "runlevel.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/mainloop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

#define PFIX "runlevel: "

static bool change_runlevel(dsme_runlevel_t runlevel);
static bool remount_mmc_readonly(void);

/** Wrapper for system() calls
 *
 * Executes the command, normalizes the return value
 * and performs some common diagnostic logging.
 *
 * Note: Getting the child process killed with SIGTERM
 *       is equated with it making exit(0) - this avoids
 *       some fallback logic assuming that shutdown/reboot
 *       did not commence because e.g. telinit got killed
 *       by init due to shutdown/reboot ...
 *
 * \param command  command line to execute
 * \return exit code from child process, or
 *         -1 if execution fails / child process is killed,
 */
static int
system_wrapper(const char *command)
{
    int         result      = -1;
    int         status      = -1;
    char        exited[32]  = "";
    char        trapped[32] = "";
    const char *dumped      = "";

    dsme_log(LOG_NOTICE, PFIX "Executing: %s", command);

    if( (status = system(command)) == -1 ) {
        snprintf(exited, sizeof exited, " exec=failed");
    }
    else {
        if( WIFSIGNALED(status) ) {
            snprintf(trapped, sizeof trapped, " signal=%s",
                     strsignal(WTERMSIG(status)));
            if( WTERMSIG(status) == SIGTERM )
                result = 0;
        }

        if( WCOREDUMP(status) )
            dumped = " core=dumped";

        if( WIFEXITED(status) ) {
            result = WEXITSTATUS(status);
            snprintf(exited, sizeof exited, " exit_code=%d", result);
        }
    }

    dsme_log(LOG_NOTICE, PFIX "Executed:  %s -%s%s%s result=%d",
             command, exited, trapped, dumped, result);

    return result;
}

/** Locate where systemd systemctl binary is installed
 *
 * Try currently expected systemctl location first, then where
 * older systemd versions are known to have kept the binary.
 *
 * \return path to systemctl, or NULL in case it is not found
 */
static const char *
locate_systemctl_binary(void)
{
    static const char * const lut[] = {
        "/usr/bin/systemctl",
        "/bin/systemctl",
        0
    };
    const char *path = 0;
    for( size_t i = 0; (path = lut[i]); ++i ) {
        if( access(path, X_OK) == 0 )
            break;
    }

    dsme_log(LOG_DEBUG, PFIX "systemctl binary = %s", path ?: "unknown");
    return path;
}

/**
   This function is used to tell init to change to new runlevel.
   Currently telinit is used.
   @param new_state State corresponding to the new runlevel
   @return Returns the return value from system(), -1 == error
   @todo Make sure that the runlevel change takes place
*/
static bool change_runlevel(dsme_runlevel_t runlevel)
{
  char command[32];

  if (access("/sbin/telinit", X_OK) == 0) {
      snprintf(command, sizeof(command), "/sbin/telinit %i", runlevel);
  } else if (access("/usr/sbin/telinit", X_OK) == 0) {
      snprintf(command, sizeof(command), "/usr/sbin/telinit %i", runlevel);
  } else {
      return false;
  }
  if (system_wrapper(command) != 0) {
      dsme_log(LOG_CRIT, PFIX "failed to change runlevel, trying again in 2s");
      sleep(2);
      return system_wrapper(command) == 0;
  }

  return true;
}

/*
 * This function will do the shutdown or reboot (based on desired runlevel).
 * If the telinit is present, runlevel change is requested.
 * Otherwise function will shutdown/reboot by itself.
 * TODO: How to make sure runlevel transition work
 * TODO: Is checking telinit reliable enough?
 */
static void shutdown(dsme_runlevel_t runlevel)
{
  char command[64];

  if ((runlevel != DSME_RUNLEVEL_REBOOT)   &&
      (runlevel != DSME_RUNLEVEL_SHUTDOWN) &&
      (runlevel != DSME_RUNLEVEL_MALF))
  {
      dsme_log(LOG_WARNING, PFIX "Shutdown request to bad runlevel (%d)", runlevel);
      return;
  }
  dsme_log(LOG_NOTICE, PFIX "%s",
           runlevel == DSME_RUNLEVEL_SHUTDOWN ? "Shutdown" :
           runlevel == DSME_RUNLEVEL_REBOOT   ? "Reboot"   :
                                                "Malf");

  /* If we have systemd, use systemctl commands */
  const char *systemctl;
  if( (systemctl = locate_systemctl_binary()) ) {
      if (runlevel == DSME_RUNLEVEL_SHUTDOWN) {
          snprintf(command, sizeof command, "%s --no-block poweroff", systemctl);
      } else if (runlevel == DSME_RUNLEVEL_REBOOT) {
          snprintf(command, sizeof command, "%s --no-block reboot", systemctl);
      } else {
          dsme_log(LOG_WARNING, PFIX "MALF not supported by our systemd implementation");
          goto fail_and_exit;
      }
      if (system_wrapper(command) != 0) {
          dsme_log(LOG_WARNING, PFIX "command %s failed: %m", command);
          /* We ignore error. No retry or anything else */
      }
  }
  /* If runlevel change fails, handle the shutdown/reboot by DSME */
  else if (!change_runlevel(runlevel))
  {
      dsme_log(LOG_CRIT, PFIX "Doing forced shutdown/reboot");
      sync();

      (void)remount_mmc_readonly();

      if (runlevel == DSME_RUNLEVEL_SHUTDOWN ||
          runlevel == DSME_RUNLEVEL_MALF)
      {
          if (access("/sbin/poweroff", X_OK) == 0) {
              snprintf(command, sizeof(command), "/sbin/poweroff");
          } else {
              snprintf(command, sizeof(command), "/usr/sbin/poweroff");
          }
          if (system_wrapper(command) != 0) {
              dsme_log(LOG_ERR, PFIX "%s failed, trying again in 3s", command);
              sleep(3);
              if (system_wrapper(command) != 0) {
                  dsme_log(LOG_ERR, PFIX "%s failed again", command);
                  goto fail_and_exit;
              }
          }
      } else {
          if (access("/sbin/reboot", X_OK) == 0) {
              snprintf(command, sizeof(command), "/sbin/reboot");
          } else {
              snprintf(command, sizeof(command), "/usr/sbin/reboot");
          }
          if (system_wrapper(command) != 0) {
              dsme_log(LOG_ERR, PFIX "%s failed, trying again in 3s", command);
              sleep(3);
              if (system_wrapper(command) != 0) {
                  dsme_log(LOG_ERR, PFIX "%s failed again", command);
                  goto fail_and_exit;
              }
          }
      }
  }

  return;

fail_and_exit:
  dsme_log(LOG_CRIT, PFIX "Closing to clean-up!");
  dsme_main_loop_quit(EXIT_FAILURE);
}

/*
 * This function tries to find mounted MMC (mmcblk) and remount it
 * read-only if mounted.
 * @return true on success, false on failure
 */
static bool remount_mmc_readonly(void)
{
  bool   mounted = false;
  char*  args[] = { (char*)"mount", NULL, NULL, (char*)"-o", (char*)"remount,ro", 0 };
  char   device[256];
  char   mntpoint[256];
  char*  line = NULL;
  size_t len = 0;
  FILE*  mounts_file = NULL;

  /* Let's try to find the MMC in /proc/mounts */
  mounts_file = fopen("/proc/mounts", "r");
  if (!mounts_file) {
      dsme_log(LOG_WARNING, PFIX "Can't open /proc/mounts. Leaving MMC as is");
      return false;
  }

  while (getline(&line, &len, mounts_file) != -1) {
      if (strstr(line, "mmcblk")) {
          sscanf(line, "%s %s", device, mntpoint);
          mounted = true;
      }
  }

  if (line) {
      free(line);
      line = NULL;
  }
  fclose(mounts_file);

  /* If mmc was found, try to umount it */
  if (mounted) {
      int   status = -1;
      pid_t pid;
      pid_t rc;

      dsme_log(LOG_WARNING, PFIX "MMC seems to be mounted, trying to mount read-only (%s %s).", device, mntpoint);

      args[1] = (char*)&device;
      args[2] = (char*)&mntpoint;
      /* try to remount read-only */
      if ((pid = fork()) < 0) {
          dsme_log(LOG_CRIT, PFIX "fork failed, exiting");
          return false;
      } else if (pid == 0) {
          execv("/bin/mount", args);
          execv("/sbin/mount", args);

          dsme_log(LOG_ERR, PFIX "remount failed, no mount cmd found");
          return false;
      }
      while ((rc = wait(&status)) != pid)
          if (rc < 0 && errno == ECHILD)
              break;
      if (rc != pid || WEXITSTATUS(status) != 0) {
          dsme_log(LOG_ERR, PFIX "mount return value != 0, no can do.");
          return false;
      }

      dsme_log(LOG_NOTICE, PFIX "MMC remounted read-only");
      return true;
  } else {
      dsme_log(LOG_NOTICE, PFIX "MMC not mounted");
      return true;
  }
}

DSME_HANDLER(DSM_MSGTYPE_CHANGE_RUNLEVEL, conn, msg)
{
  (void)change_runlevel(msg->runlevel);
}

DSME_HANDLER(DSM_MSGTYPE_SHUTDOWN, conn, msg)
{
  shutdown(msg->runlevel);
}

module_fn_info_t message_handlers[] = {
  DSME_HANDLER_BINDING(DSM_MSGTYPE_CHANGE_RUNLEVEL),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_SHUTDOWN),
  { 0 }
};

void module_init(module_t* module)
{
  dsme_log(LOG_DEBUG, PFIX "runlevel.so loaded");
}

void module_fini(void)
{
  dsme_log(LOG_DEBUG, PFIX "runlevel.so unloaded");
}
