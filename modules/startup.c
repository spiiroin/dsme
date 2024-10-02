/**
   @file startup.c

   This file implements a policy that is used to load
   all other policies and do startup tasks for DSME.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
   Copyright (C) 2012-2017 Jolla Ltd.

   @author Ari Saastamoinen
   @author Ismo Laitinen <ismo.laitinen@nokia.com>
   @author Semi Malinen <semi.malinen@nokia.com>
   @author Simo Piiroinen <simo.piiroinen@nokia.com>
   @author Markus Lehtonen <markus.lehtonen@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
   @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
   @author Kalle Jokiniemi <kalle.jokiniemi@jolla.com>
   @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
   @author marko lemmetty <marko.lemmetty@jollamobile.com>

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

/**
 * @defgroup modules DSME Modules
 */

/**
 * @defgroup startup Startup
 * @ingroup modules
 *
 * Startup module loads other modules on DSME startup.
 */

#include <dsme/messages.h>
#include "../include/dsme/modulebase.h"
#include "../include/dsme/logging.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <limits.h>

#define PFIX "startup: "

#define STRINGIFY(x)  STRINGIFY2(x)
#define STRINGIFY2(x) #x

/**
 * @ingroup startup
 * Configuration file that has the list of modules that are loaded on startup.
 * DSME tries to load the modules from the same directory where startup-module
 * was loaded.
 */
#define MODULES_CONF "/etc/dsme/modules.conf"

/**
 * @ingroup startup
 * This array defines which modules are started on startup in case
 * /etc/dsme/modules.conf is not readable.
 */
const char *modules[] = {
    "heartbeat.so",
#ifdef DSME_WANT_LIBUPSTART
    "upstart.so",            // upstart provides "init"
#else
#ifdef DSME_WANT_LIBRUNLEVEL
    "runlevel.so",           // runlevel provides "init"
#endif
#endif
    "dbusproxy.so",
    "malf.so",               // malf depends on "init" (& state via enter_malf)
    "state.so",              // state depends on malf, dbusproxy & init
    "iphb.so",
    "processwd.so",
    "alarmtracker.so",
#ifdef DSME_BOOTREASON_LOGGER
    "bootreasonlogger.so",
#endif
#ifdef DSME_BATTERY_TRACKER
    "batterytracker.so",
#endif
    "thermalflagger.so",
    "thermalmanager.so",
#ifdef DSME_GENERIC_THERMAL_MGMT
    "thermalsensor_generic.so",
#endif
    "emergencycalltracker.so",
    "usbtracker.so",
#ifdef DSME_POWERON_TIMER
    "powerontimer.so",
#endif
#ifdef DSME_VALIDATOR_LISTENER
    "validatorlistener.so",
#endif
    "diskmonitor.so",
#ifdef DSME_PWRKEY_MONITOR
    "pwrkeymonitor.so",
#endif
#ifdef DSME_VIBRA_FEEDBACK
    "shutdownfeedback.so",
#endif
#ifdef DSME_WLAN_LOADER
    "wlanloader.so",
#endif
#ifdef DSME_ABOOTSETTINGS
    "abootsettings.so",
#endif
    /* autoconnector plugin must be the last one to load */
    "dbusautoconnector.so",
    NULL
};

DSME_HANDLER(DSM_MSGTYPE_GET_VERSION, client, ind)
{
	static const char*       version = STRINGIFY(PRG_VERSION);
	DSM_MSGTYPE_DSME_VERSION msg     =
          DSME_MSG_INIT(DSM_MSGTYPE_DSME_VERSION);

        dsme_log(LOG_DEBUG, PFIX "version requested, sending '%s'", version);
	endpoint_send_with_extra(client, &msg, strlen(version) + 1, version);
}

module_fn_info_t message_handlers[] = {
  DSME_HANDLER_BINDING(DSM_MSGTYPE_GET_VERSION),
  {0}
};

void module_init(module_t *handle)
{
	dsme_log(LOG_DEBUG, PFIX "DSME %s starting up", STRINGIFY(PRG_VERSION));

	FILE       *conffile   = 0;
	char       *modulename = 0;
	const char *moduledir  = 0;
	char        modulepath[PATH_MAX];

	if( !(modulename = strdup(module_name(handle))) ) {
		dsme_log(LOG_CRIT, PFIX "strdup failed");
		exit(EXIT_FAILURE);
	}
	moduledir = dirname(modulename);

	if( !(conffile = fopen(MODULES_CONF, "r")) ) {
		dsme_log(LOG_DEBUG, PFIX "Unable to read conffile (%s), using compiled-in startup list", MODULES_CONF);

		for( size_t i = 0; modules[i]; ++i ) {
			int rc = snprintf(modulepath, sizeof modulepath, "%s/%s", moduledir, modules[i]);
			if( rc < 0 || rc >= sizeof modulepath ) {
				/* Skip on error / truncate */
				continue;
			}
			if( !modulebase_load_module(modulepath, 0) ) {
				dsme_log(LOG_ERR, PFIX "error loading module %s", modulepath);
			}
		}
	}
	else {
		dsme_log(LOG_DEBUG, PFIX "Conf file exists, reading modulenames from %s", MODULES_CONF);
		size_t size = 0;
		char  *line = NULL;
		while( getline(&line, &size, conffile) > 0 ) {
			line[strcspn(line, "\r\n")] = 0;
			int rc = snprintf(modulepath, sizeof modulepath, "%s/%s", moduledir, line);
			if( rc < 0 || rc >= sizeof modulepath ) {
				/* Skip on error / truncate */
				continue;
			}
			if (!modulebase_load_module(modulepath, 0) ) {
				dsme_log(LOG_ERR, PFIX "error loading module %s", modulepath);
			}
		}
		free(line);
		fclose(conffile);
	}

	free(modulename);
	dsme_log(LOG_DEBUG, PFIX "Module loading finished.");
}
