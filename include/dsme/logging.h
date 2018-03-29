/**
   @file logging.h

   Prototypes for logging functions.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
   Copyright (C) 2015-2017 Jolla Ltd.

   @author Tuukka Tikkanen
   @author Semi Malinen <semi.malinen@nokia.com>
   @author Simo Piiroinen <simo.piiroinen@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
   @author Antti Virtanen <antti.i.virtanen@nokia.com>
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

#ifndef  DSME_LOGGING_H
# define DSME_LOGGING_H

# include <dsme/messages.h>
/* Even if syslog is not used, use the message levels therein */
# include <syslog.h>
# include <stdbool.h>
# include <unistd.h>

/* Logging methods */
typedef enum {
    LOG_METHOD_NONE,   /* Suppress all the messages */
    LOG_METHOD_STDERR, /* Print messages to stderr */
    LOG_METHOD_SYSLOG, /* Use syslog(3) */
    LOG_METHOD_FILE    /* Output messages to the file */
} log_method;


/* libdsm/dsmesocke IPC */
typedef struct
{
    DSMEMSG_PRIVATE_FIELDS
    int verbosity;
} DSM_MSGTYPE_SET_LOGGING_VERBOSITY;

typedef dsmemsg_generic_t DSM_MSGTYPE_ADD_LOGGING_INCLUDE;
typedef dsmemsg_generic_t DSM_MSGTYPE_ADD_LOGGING_EXCLUDE;
typedef dsmemsg_generic_t DSM_MSGTYPE_USE_LOGGING_DEFAULTS;

enum
{
    /* NOTE: dsme message types are defined in:
     * - libdsme
     * - libiphb
     * - dsme
     *
     * When adding new message types
     * 1) uniqueness of the identifiers must be
     *    ensured accross all these source trees
     * 2) the dsmemsg_id_name() function in libdsme
     *    must be made aware of the new message type
     */

    DSME_MSG_ENUM(DSM_MSGTYPE_SET_LOGGING_VERBOSITY, 0x00001103),
    DSME_MSG_ENUM(DSM_MSGTYPE_ADD_LOGGING_INCLUDE,   0x00001104),
    DSME_MSG_ENUM(DSM_MSGTYPE_ADD_LOGGING_EXCLUDE,   0x00001105),
    DSME_MSG_ENUM(DSM_MSGTYPE_USE_LOGGING_DEFAULTS,  0x00001106),
};

/* Logging functionality */
bool dsme_log_open(log_method method, int verbosity, int usetime,
                   const char *prefix, int facility, int option,
                   const char *filename);
void dsme_log_stop(void);
void dsme_log_close(void);
void dsme_log_set_verbosity(int verbosity);
void dsme_log_include(const char *pat);
void dsme_log_exclude(const char *pat);
void dsme_log_clear_rules(void);
bool dsme_log_p_ (int level, const char *file, const char *func);
void dsme_log_queue(int level, const char *file, const char *func, const char *fmt, ...) __attribute__((format(printf,4,5)));

#  define dsme_log(LEV_, FMT_, ARGS_...) \
     do {\
         if( dsme_log_p_(LEV_, __FILE__, __FUNCTION__) ) {\
             dsme_log_queue(LEV_, __FILE__, __FUNCTION__, FMT_, ## ARGS_);\
         }\
     }while(0)

char* pid2text(pid_t pid);

#endif /* DSME_LOGGING_H */
