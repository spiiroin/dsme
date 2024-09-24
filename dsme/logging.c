/**
   @file logging.c

   Implements DSME logging functionality.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
   Copyright (C) 2013-2017 Jolla Ltd.

   @author Yuri Zaporogets
   @author Semi Malinen <semi.malinen@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
   @author Antti Virtanen <antti.i.virtanen@nokia.com>
   @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
   @author Jarkko Nikula <jarkko.nikula@jollamobile.com>

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

#include "../include/dsme/logging.h"

#include <sys/eventfd.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syslog.h>
#include <sys/socket.h>
#include <asm/types.h>
#include <linux/netlink.h>

#include <pthread.h>
#include <fnmatch.h>

#include <glib.h>

/* ========================================================================= *
 * Types
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * log_entry_t
 * ------------------------------------------------------------------------- */

/** Length of the logging ring buffer */
# define DSME_LOG_ENTRY_COUNT 128 /* must be a power of 2! */

/** Size of one entry in the logging ring buffer */
# define DSME_LOG_ENTRY_SIZE  128

/** Entry in logging ringbuffer */
typedef union  log_entry_t
{
    struct
    {
        int         prio;   /**< LOG_EMERG ... LOG_DEBUG */
        const char *file;   /**< Source code path */
        const char *func;   /**< Calling function */
        char        text[]; /**< Message + context, see log_entry_vformat() */
    };
    char data[DSME_LOG_ENTRY_SIZE];
} log_entry_t;

/* ------------------------------------------------------------------------- *
 * log_state_t
 * ------------------------------------------------------------------------- */

/** Include/Exclude module:function rule state */
typedef enum
{
  LOG_STATE_UNKNOWN  = 0, /**< Not evaluated yet, must be zero */
  LOG_STATE_INCLUDED = 1, /**< Included to logging */
  LOG_STATE_EXCLUDED = 2, /**< Excluded from logging */
  LOG_STATE_DEFAULT  = 3, /**< Normal verbosity vs priority applies */
} log_state_t;

/* ------------------------------------------------------------------------- *
 * log_rule_t
 * ------------------------------------------------------------------------- */

typedef struct
{
    gchar       *pattern; /**< "file:func" glob pattern */
    log_state_t  state;   /**< State to apply if pattern matches */
} log_rule_t;

/* ========================================================================= *
 * Functionality
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * log_entry_t
 * ------------------------------------------------------------------------- */

static void        log_entry_vformat          (log_entry_t *self, int prio, const char *file, const char *func, const char *fmt, va_list va);
static void        log_entry_format           (log_entry_t *self, int prio, const char *file, const char *func, const char *fmt, ...) __attribute__((format(printf,5,6)));;

/* ------------------------------------------------------------------------- *
 * log_state_t
 * ------------------------------------------------------------------------- */

static const char *log_state_repr             (log_state_t state);

/* ------------------------------------------------------------------------- *
 * Logging Priorities
 * ------------------------------------------------------------------------- */

static int         log_prio_cap               (int prio);
static const char *log_prio_str               (int prio);

/* ------------------------------------------------------------------------- *
 * Logging Backends
 * ------------------------------------------------------------------------- */

static void        log_to_null                (const log_entry_t *entry);
static void        log_to_stderr              (const log_entry_t *entry);
static void        log_to_syslog              (const log_entry_t *entry);
static void        log_to_file                (const log_entry_t *entry);

/* ------------------------------------------------------------------------- *
 * log_rule_t
 * ------------------------------------------------------------------------- */

static log_rule_t *log_rule_create            (const char *pattern, log_state_t state);
static void        log_rule_delete            (log_rule_t *self);
static void        log_rule_delete_cb         (gpointer self);

/* ------------------------------------------------------------------------- *
 * Logging Control
 * ------------------------------------------------------------------------- */

void               dsme_log_clear_rules       (void);
static void        dsme_log_add_rule          (const char *pattern, log_state_t state);
void               dsme_log_include           (const char *pattern);
void               dsme_log_exclude           (const char *pattern);
static log_state_t dsme_log_evaluate          (const char *file, const char *func);
void               dsme_log_set_verbosity     (int verbosity);
bool               dsme_log_p_                (int prio, const char *file, const char *func);

/* ------------------------------------------------------------------------- *
 * Logging Queue
 * ------------------------------------------------------------------------- */

void               dsme_log_queue             (int prio, const char *file, const char *func, const char *fmt, ...);
static void       *dsme_log_thread            (void *param);

/* ------------------------------------------------------------------------- *
 * Logging Start/Stop
 * ------------------------------------------------------------------------- */

bool               dsme_log_open              (log_method method, int verbosity, int usetime, const char *prefix, int facility, int option, const char *filename);
void               dsme_log_close             (void);
void               dsme_log_stop              (void);

/* ========================================================================= *
 * Dynamic Configuration
 * ========================================================================= */

static struct
{
    log_method  method;    /* Chosen logging method */
    int         verbosity; /* Verbosity level (corresponding to LOG_*) */
    int         usetime;   /* Timestamps on/off */
    const char* prefix;    /* Message prefix */
    FILE*       filep;     /* Log file stream */
} logopt =
{
    .method    = LOG_METHOD_STDERR,
    .verbosity = LOG_NOTICE,
    .usetime   = 0,
    .prefix    = "DSME",
    .filep     = 0,
};

/* ========================================================================= *
 * log_entry_t
 * ========================================================================= */

static void
log_entry_vformat(log_entry_t *self, int prio, const char *file,
                  const char *func, const char *fmt, va_list va)
{
    /* In normal operation mode (=using syslog) the file and function
     * data is not used. To minimize the size of the statically allocated
     * logging ring buffer, the file:func data is added to the unused
     * space that is left over after storing the actual log message.
     *
     * Note: Even though the file/func strings are constants resulting
     * from use of __FILE__ and __FUNCTION__ macros, they can't be used
     * as is because they still become invalid if pointing to modules that
     * have already been unloaded (for example during dsme exit).
     */

    self->prio = log_prio_cap(prio);
    self->file = "unknown";
    self->func = "unknown";

    char *end = (char *)(self + 1);
    char *pos = self->text;

    vsnprintf(pos, end - pos, fmt, va);
    pos = strchr(pos, 0) + 1;

    if( file && pos < end ) {
        self->file = pos;
        snprintf(pos, end - pos, "%s", file);
        pos = strchr(pos, 0) + 1;
    }

    if( func && pos < end ) {
        self->func = pos;
        snprintf(pos, end - pos, "%s", func);
    }
}

static void
log_entry_format(log_entry_t *self, int prio, const char *file,
                 const char *func, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_entry_vformat(self, prio, file, func, fmt, va);
    va_end(va);
}

/* ========================================================================= *
 * log_state_t
 * ========================================================================= */

static const char *
log_state_repr(log_state_t state)
{
    const char *repr = "LOG_STATE_INVALID";

    switch( state ) {
    case LOG_STATE_UNKNOWN:  repr = "LOG_STATE_UNKNOWN";  break;
    case LOG_STATE_INCLUDED: repr = "LOG_STATE_INCLUDED"; break;
    case LOG_STATE_EXCLUDED: repr = "LOG_STATE_EXCLUDED"; break;
    case LOG_STATE_DEFAULT:  repr = "LOG_STATE_DEFAULT";  break;
    default: break;
    }

    return repr;
}

/* ========================================================================= *
 * Logging Priorities
 * ========================================================================= */

/** Normalize priority to LOG_EMERG ... LOG_DEBUG range
 */
static int log_prio_cap(int prio)
{
    if( prio < LOG_EMERG )
        return LOG_EMERG;

    if( prio > LOG_DEBUG )
        return LOG_DEBUG;

    return prio;
}

/** Map logging priority to human readable string
 */
static const char* log_prio_str(int prio)
{
    switch( prio ) {
    case LOG_DEBUG:     return "debug";
    case LOG_INFO:      return "info";
    case LOG_NOTICE:    return "notice";
    case LOG_WARNING:   return "warning";
    case LOG_ERR:       return "error";
    case LOG_CRIT:      return "critical";
    case LOG_ALERT:     return "alert";
    case LOG_EMERG:     return "emergency";
    default:            return "log";
    }
}

/* ========================================================================= *
 * log_rule_t
 * ========================================================================= */

static log_rule_t *
log_rule_create(const char *pattern, log_state_t state)
{
    log_rule_t *self = g_malloc0(sizeof *self);

    self->pattern = g_strdup(pattern);
    self->state   = state;

    return self;
}

static void
log_rule_delete(log_rule_t *self)
{
    if( self != 0 ) {
        g_free(self->pattern);
        g_free(self);
    }
}

static void
log_rule_delete_cb(gpointer self)
{
    return log_rule_delete(self);
}

/* ========================================================================= *
 * Logging Backends
 * ========================================================================= */

/*
 * Empty routine for suppressing all log messages
 */
static void log_to_null(const log_entry_t *entry)
{
    (void)entry;
}

/*
 * This routine is used when stderr logging method is set
 */
static void log_to_stderr(const log_entry_t *entry)
{
    fprintf(stderr, "%s %s: %s: %s(): %s\n",
            logopt.prefix,
            log_prio_str(entry->prio),
            entry->file,
            entry->func,
            entry->text);
    fflush(stderr);
}

/*
 * This routine is used when syslog logging method is set
 */
static void log_to_syslog(const log_entry_t *entry)
{
    syslog(entry->prio, "%s", entry->text);
}

/*
 * This routine is used when file logging method is set
 */
static void log_to_file(const log_entry_t *entry)
{
    fprintf(logopt.filep, "%s %s: %s\n",
            logopt.prefix,
            log_prio_str(entry->prio),
            entry->text);
    fflush(logopt.filep);
}

/* ========================================================================= *
 * Logging Queue
 * ========================================================================= */

/* This variable holds the address of the logging functions */
static void (*dsme_log_routine)(const log_entry_t *entry) = log_to_stderr;

/** Ring buffer for queueing logging messages */
static log_entry_t ring_buffer[DSME_LOG_ENTRY_COUNT];

/** Eventfd for waking up the logger thread */
static volatile int ring_buffer_event_fd = -1;

/** Worker thread id */
static pthread_t worker_tid = 0;

/** Ring buffer write position
 *
 * This is modified from main thread only.
 *
 * Access only via read_count_get() / read_count_inc() functions.
 */
static volatile guint write_count_pvt = 0;

/** Ring buffer read position
 *
 * This is modified from logger thread only.
 *
 * Access only via write_count_get() / write_count_inc() functions.
 */
static volatile guint read_count_pvt  = 0;

/** Flag for: logger thread enabled
 *
 * This initialized to non-zero value and should be cleared only
 * when either the whole dsme server process or logging thread is
 * about to exit.
 */
static volatile int thread_enabled = 1;

/** Flag for: logger thread running
 *
 * This is modified from logger thread only.
 */
static volatile int thread_running = 0;

/** Atomic access to logging ring buffer read count
 *
 * If glib atomic helpers can be assumed provide
 * atomic access and use of hw-only memory barrier,
 * they are used.
 *
 * Otherwise we prefer hitting issues arising from
 * non-atomic access etc over chances of hitting
 * priority inversions due the use of mutexes.
 *
 * @return number of messages written to logging ring buffer.
 */
static inline guint read_count_get(void)
{
# ifdef G_ATOMIC_LOCK_FREE
    return g_atomic_int_get(&read_count_pvt);
# else
    return read_count_pvt;
# endif
}

/** Atomic increase of logging ring buffer read count
 *
 * @sa #read_count_get()
 */
static inline void read_count_inc(void)
{
# ifdef G_ATOMIC_LOCK_FREE
    g_atomic_int_inc(&read_count_pvt);
# else
    ++read_count_pvt;
# endif
}

/** Atomic access to logging ring buffer write count
 *
 * @sa #read_count_get()
 *
 * @return number of messages read from logging ring buffer.
 */
static inline guint write_count_get(void)
{
# ifdef G_ATOMIC_LOCK_FREE
    return g_atomic_int_get(&write_count_pvt);
# else
    return write_count_pvt;
# endif
}

/** Atomic increase of logging ring buffer write count
 *
 * @sa #read_count_get()
 */
static inline void write_count_inc(void)
{
# ifdef G_ATOMIC_LOCK_FREE
    g_atomic_int_inc(&write_count_pvt);
# else
    ++write_count_pvt;
# endif
}

/** Number of messages currently stored in logging ring buffer
 *
 * @return number of messages available for reading in logging ring buffer.
 */
static inline guint buffered_count(void)
{
    return write_count_get() - read_count_get();
}

/** Notify logger thread about new entry in ring buffer
 *
 * @param entry  logging entry pointer
 *
 * Expected usage:
 *
 * @li Entry is "allocated" at slot read_count_get()
 * @li Entry data is filled in via log_entry_format()
 * @li Buffered count is updated via Write_count_inc()
 * @li Then this function is called
 *
 * Assumptions built in:
 *
 * @li All logging occurs only from one (= the main) thread
 *
 * @note The entry pointer itself is not used except in situations where
 *       the worker thread is not available for one or another reason.
 */
static void
dsme_log_notify_worker(log_entry_t *entry)
{
    const uint64_t one = 1;
    bool           ack = false;

    if( thread_enabled ) {
        if( ring_buffer_event_fd == -1 ) {
            /* Logging functionality was used before dsme_log_init().
             *
             * Mostly harmless, but make some noise anyway.
             */
            static bool reported = false;
            if( !reported ) {
                reported = true;
                fprintf(stderr, "*** DSME LOGGER USED BEFORE INIT\n");
                fflush(stderr);
            }
        }
        else if( write(ring_buffer_event_fd, &one, sizeof one) == -1 ) {
            /* Disable logging thread - this and all future logging
             * will be handled from main thread.
             *
             * As this makes dsme server process suspectible to get
             * frozen / blocked by problems syslogd/journald might
             * be having - which in turn can lead to dsme wdd process
             * to stop feeding the watchdog devices - which can lead
             * to wd reboot -> make some noise about this.
             */
            thread_enabled = 0;
            fprintf(stderr, "*** DSME LOGGER THREAD DISABLED\n");
            fflush(stderr);
        }
        else {
            ack = true;
        }
    }

    if( !ack ) {
        /* Handle from main thread */
        dsme_log_routine(entry);
    }
}

/** Queue a logging message to logging ringbuffer
 *
 * Normally this function is used from dsme_log() macro.
 *
 * No locking is done under assumption that mainthread controls
 * the "tail" of ring buffer and worker thread "head" of the
 * ring buffer and additions & offset increments are done in
 * sane order and atomically enough.
 *
 * @param prio  syslog compatible LOG_EMERG ... LOG_DEBUG value
 * @param file  source code file path
 * @param func  calling function
 * @param fmt   printf style format string
 * @param ...   arguments needed for the format string
 */
void
dsme_log_queue(int prio, const char *file, const char *func, const char* fmt, ...)
{
    static bool     overflow = false;
    static unsigned skipped  = 0;

    log_entry_t *entry;

    /* Handle ring buffer overflows */
    unsigned buffered = buffered_count();

    if( buffered >= DSME_LOG_ENTRY_COUNT ) {
        overflow = true;
        ++skipped;
        goto EXIT;
    }

    if( overflow ) {
        /* must go down enough before overflow is cleared */
        if( buffered >= DSME_LOG_ENTRY_COUNT * 7 / 8 ) {
            ++skipped;
            goto EXIT;
        }

        /* Add log entry about the overflow itself */
        entry = &ring_buffer[write_count_get() % DSME_LOG_ENTRY_COUNT];
        log_entry_format(entry, LOG_ERR, __FILE__, __FUNCTION__,
                         "logging ringbuffer overflow; %u messages lost", skipped);
        write_count_inc();
        dsme_log_notify_worker(entry);

        overflow = false;
        skipped = 0;
    }

    /* Add log entry to the ring buffer */
    entry = &ring_buffer[write_count_get() % DSME_LOG_ENTRY_COUNT];

    va_list va;
    va_start(va, fmt);
    log_entry_vformat(entry, prio, file, func, fmt, va);
    va_end(va);

    write_count_inc();
    dsme_log_notify_worker(entry);

EXIT:
    return;
}

/** Thread function for dequeueing messages from logging ringbuffer
 *
 * This is the logging thread that reads log entries from
 * the ring buffer and writes them to their final destination.
 */
static void *
dsme_log_thread(void* param)
{
    (void)param;

    thread_running = 1;

    /* Deny thread cancellation */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, 0);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, 0);

    for( ;; ) {
        uint64_t cnt = 0;

        /* Arrange a cancelation point at eventfd read() */
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
        ssize_t rc = read(ring_buffer_event_fd, &cnt, sizeof cnt);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, 0);

        /* Ignore i/o error if we have been asked to exit */
        if( !thread_enabled )
            goto EXIT;

        /* Make noise if exiting due to i/o error */
        if( rc == -1 ) {
            static const char m[] = "*** DSME LOGGER READ ERROR\n";
            if( write(STDERR_FILENO, m, sizeof m - 1) == -1 ) {
                // dontcare
            }
            goto EXIT;
        }

        for( ; cnt; --cnt ) {
            /* While it should not be possible; if it looks like ring buffer
             * bookkeeping is out of sync, make some noise, exit logger
             * thread, and switch to log-from-main-thread logic.
             */
            if( buffered_count() == 0 ) {
                static const char m[] = "*** DSME LOGGER OUT OF SYNC\n";
                if( write(STDERR_FILENO, m, sizeof m - 1) == -1 ) {
                    // dontcare
                }
                goto EXIT;
            }

            /* Pop entry from ring buffer and process it */
            log_entry_t *entry = &ring_buffer[read_count_get() % DSME_LOG_ENTRY_COUNT];
            dsme_log_routine(entry);
            read_count_inc();

            /* Stop processing if we have been asked to exit */
            if( !thread_enabled )
                goto EXIT;
        }
    }

EXIT:
    thread_running = 0;

    /* Disable ring buffering (if we exit without getting cancelled) */
    thread_enabled = 0;

    return 0;
}

/* ========================================================================= *
 * Logging Control
 * ========================================================================= */

static GSList     *dsme_log_rule_list  = 0;
static GHashTable *dsme_log_rule_cache = 0;

/** Remove all include/exclude rules
 */
void
dsme_log_clear_rules(void)
{
    dsme_log_queue(LOG_DEBUG, __FILE__, __FUNCTION__, "log rules cleared");

    if( dsme_log_rule_list ) {
        g_slist_free_full(dsme_log_rule_list, log_rule_delete_cb),
            dsme_log_rule_list = 0;
    }

    if( dsme_log_rule_cache ) {
        g_hash_table_unref(dsme_log_rule_cache),
            dsme_log_rule_cache = 0;
    }
}

/** Add an include/exclude rule
 *
 * @param state     LOG_STATE_INCLUDED / LOG_STATE_EXCLUDED
 * @param pattern  glob pattern matching file:function
 */
static void
dsme_log_add_rule(const char *pattern, log_state_t state)
{
    dsme_log_queue(LOG_DEBUG, __FILE__, __FUNCTION__, "log rule '%s' -> %s",
                   pattern, log_state_repr(state));

    if( dsme_log_rule_cache )
        g_hash_table_remove_all(dsme_log_rule_cache);
    else
        dsme_log_rule_cache = g_hash_table_new_full(g_str_hash,
                                                    g_str_equal,
                                                    g_free, 0);

    /* Note: Use of prepend here and using the first matching rule
     *       in evaluation -> the last matching rule given at the
     *       command line wins.
     */
    dsme_log_rule_list = g_slist_prepend(dsme_log_rule_list,
                                         log_rule_create(pattern, state));
}

/** Add include rule
 *
 * @param pattern  glob pattern matching file:function
 */
void
dsme_log_include(const char *pattern)
{
    if( pattern )
        dsme_log_add_rule(pattern, LOG_STATE_INCLUDED);
}

/** Add Exclude rule
 *
 * @param pattern  glob pattern matching file:function
 */
void
dsme_log_exclude(const char *pattern)
{
    if( pattern )
        dsme_log_add_rule(pattern, LOG_STATE_EXCLUDED);
}

/** Evaluate if file/function is included to/excluded from logging
 *
 * @param file  module path
 * @param func  function name
 *
 * @return LOG_STATE_INCLUDED/LOG_STATE_EXCLUDED/...
 */
static log_state_t
dsme_log_evaluate(const char *file, const char *func)
{
    gpointer hit = GINT_TO_POINTER(LOG_STATE_DEFAULT);

    if( !dsme_log_rule_cache )
        goto EXIT;

    char key[256];
    snprintf(key, sizeof key, "%s:%s", file, func);

    if( (hit = g_hash_table_lookup(dsme_log_rule_cache, key)) )
        goto EXIT;

    /* Note: due to above lookup: hit == 0 == LOG_STATE_UNKNOWN */

    for( GSList *item = dsme_log_rule_list; item; item = item->next ) {
        log_rule_t *rule = item->data;

        if( fnmatch(rule->pattern, key, 0) != 0 )
            continue;

        hit = GINT_TO_POINTER(rule->state);
        break;
    }
    g_hash_table_replace(dsme_log_rule_cache, g_strdup(key), hit);

EXIT:
    return GPOINTER_TO_INT(hit);
}

/** Set overall logging vebosity
 *
 * @param verbosity syslog compatible LOG_EMERG ... LOG_DEBUG value
 */
void
dsme_log_set_verbosity(int verbosity)
{
    verbosity = log_prio_cap(verbosity);

    if( logopt.verbosity != verbosity ) {
        dsme_log_queue(LOG_DEBUG, __FILE__, __FUNCTION__, "verbosity: %s -> %s",
                       log_prio_str(logopt.verbosity),
                       log_prio_str(verbosity));
        logopt.verbosity = verbosity;
    }
}

/** Log level testing predicate
 *
 * Normally this function is used from dsme_log() macro.
 *
 * For testing whether given level of logging is allowed
 * before spending cpu time for gathering parameters etc
 *
 * @param prio  level of logging to perform
 * @param file  path to module containing the calling function
 * @param func  name of the function name
 *
 * @return true if logging is allowed, false if not
 */
bool
dsme_log_p_(int prio, const char *file, const char *func)
{
    /* Check file/function inclusion/exclusion rules 1st */
    if( dsme_log_rule_cache && file && func ) {
        switch( dsme_log_evaluate(file, func) ) {
        case LOG_STATE_INCLUDED:
            return true;
        case LOG_STATE_EXCLUDED:
            return false;
        default:
            break;
        }
    }

    /* By default check priority vs. verbosity setting */
    return prio <= logopt.verbosity;
}

/* ========================================================================= *
 * Logging Start/Stop
 * ========================================================================= */

/** Initialize logging ring buffer
 *
 * After this function has been called, messages can be queued to
 * ring buffer.
 *
 * Worker thread is started from dsme_log_open() i.e. when also the
 * logging target has been defined.
 *
 * @return true on success, or false on failure
 */
bool
dsme_log_init(void)
{
    bool ack = false;

    if( (ring_buffer_event_fd = eventfd(0, EFD_CLOEXEC)) == -1 ) {
        fprintf(stderr, "eventfd: %s\n", strerror(errno));
        goto EXIT;
    }

    /* Using dsme_log() & co is ok once ring_buffer_event_fd is set, but
     * note that default verbosity and logging target is effective until
     * also dsme_log_open() is called.
     */

    ack = true;

EXIT:
    return ack;
}

/** Initialize logging
 *
 * @param method    logging method
 * @param usetime   if nonzero, each message will pe prepended with a timestamp
 * @param prefix    the text that will be printed before each message
 * @param facility  the facility for openlog() (only for syslog method)
 * @param option    option for openlog() (only for syslog method)
 * @param filename  log file name (only for file method)
 *
 * @return true upon successfull initialization, falseotherwise.
 */
bool
dsme_log_open(log_method  method,
              int         verbosity,
              int         usetime,
              const char* prefix,
              int         facility,
              int         option,
              const char* filename)
{
    logopt.method    = method;
    logopt.verbosity = log_prio_cap(verbosity);
    logopt.usetime   = usetime;
    logopt.prefix    = prefix;

    switch( method ) {
    case LOG_METHOD_NONE:
        dsme_log_routine = log_to_null;
        break;

    case LOG_METHOD_STDERR:
        dsme_log_routine = log_to_stderr;
        break;

    case LOG_METHOD_SYSLOG:
        openlog(prefix, option, facility);
        dsme_log_routine = log_to_syslog;
        break;

    case LOG_METHOD_FILE:
        if( !(logopt.filep = fopen(filename, "a")) ) {
            fprintf(stderr,
                    "Can't create log file %s (%s)\n",
                    filename,
                    strerror(errno));
            return false;
        }
        dsme_log_routine = log_to_file;
        break;

    default:
        return false;
    }

    /* create the logging thread */
    pthread_attr_t     tattr;
    pthread_t          tid;
    struct sched_param param;

    if (pthread_attr_init(&tattr) != 0) {
        fprintf(stderr, "Error getting thread attributes\n");
        return false;
    }

    if (pthread_attr_getschedparam(&tattr, &param) != 0) {
        fprintf(stderr, "Error getting scheduling parameters\n");
        return false;
    }

    if (pthread_create(&tid, &tattr, dsme_log_thread, 0) != 0) {
        fprintf(stderr, "Error creating the logging thread\n");
        return false;
    }

    worker_tid = tid;

    /* Report whether true or merely assumed to be atomic
     * operations are used for bookkeeping. */
# ifdef G_ATOMIC_LOCK_FREE
    dsme_log(LOG_DEBUG, "using glib atomic helper functions");
# else
    dsme_log(LOG_WARNING, "not using glib atomic helper functions");
# endif
    return true;
}

/** Cleanup logging
 *
 * This routine should be called before program termination. It will close
 * log streams and do other cleanup work.
 */
void
dsme_log_close(void)
{
    // Free rule data
    dsme_log_clear_rules();

    // Deny queue processing from logging thread
    dsme_log_stop();

    // Flush remaining messages from main thread
    for( guint at = read_count_get(); at != write_count_get(); ++at ) {
        log_entry_t *entry = &ring_buffer[at % DSME_LOG_ENTRY_COUNT];
        dsme_log_routine(entry);
    }

    // Cleanup
    switch (logopt.method) {
    case LOG_METHOD_STDERR:
        fflush(stderr);
        break;

    case LOG_METHOD_SYSLOG:
        closelog();
        break;

    case LOG_METHOD_FILE:
        if( logopt.filep )
            fclose(logopt.filep), logopt.filep = 0;
        break;

    default:
        break;
    }
}

/** Stop logging worker thread
 *
 * Should be used when dsme is about to exit.
 *
 * Does not wait for the worker thread to exit under assumption that
 * the worst thing that can happen is that one of the queued messages
 * gets output from both worker thread and the main thread when the
 * remaining queue is flushed.
 */
void
dsme_log_stop(void)
{
    /* Disable ring buffering and tell logger to exit */
    thread_enabled = 0;

    /* Closing eventfd denies logger from going to sleep */
    int fd = ring_buffer_event_fd;
    if( fd != -1 ) {
        ring_buffer_event_fd = -1;
        close(fd);
    }

    /* Wait upto 3 seconds for logger to exit */
    pthread_t tid = worker_tid;
    if( tid != 0 ) {
        worker_tid = 0;
        int err = pthread_cancel(tid);
        if( err ) {
            fprintf(stderr, "*** FAILED TO STOP DSME LOGGER, err=%s\n", strerror(err));
            fflush(stderr);
        }
        else {
            void *ret = 0;
            struct timespec tmo = { };
            clock_gettime(CLOCK_REALTIME, &tmo);
            tmo.tv_sec += 3;
            err = pthread_timedjoin_np(tid, &ret, &tmo);
            if( err ) {
                fprintf(stderr, "*** FAILED TO JOIN DSME LOGGER, err=%s\n", strerror(err));
                fflush(stderr);
            }
        }
    }
}
