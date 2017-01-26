/**
   @file diskmonitor_backend.c

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

#define LOGPFIX "diskmonitor: "

#include "diskmonitor_backend.h"
#include "diskmonitor.h"

#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"

#include <string.h>
#include <mntent.h>
#include <sys/statfs.h>
#include <stdbool.h>

#define ArraySize(a) (sizeof(a)/sizeof*(a))

typedef struct
{
    const char       *mntpoint;
    int               max_usage_percent;
    diskspace_state_t signaled_state;
    unsigned          check_tag;
} disk_use_limit_t;

static disk_use_limit_t disk_space_use_limits[] =
{
   { "/",     90, DISKSPACE_STATE_UNDEF, 0 },
   { "/tmp",  70, DISKSPACE_STATE_UNDEF, 0 },
   { "/run",  70, DISKSPACE_STATE_UNDEF, 0 },
   { "/home", 90, DISKSPACE_STATE_UNDEF, 0 },
};

static disk_use_limit_t *
find_use_limit_for_mount(const char* mntpoint)
{
    disk_use_limit_t* use_limit = 0;

    for( size_t i = 0; i < ArraySize(disk_space_use_limits); ++i ) {
        if( !strcmp(disk_space_use_limits[i].mntpoint, mntpoint) ) {
            use_limit = &disk_space_use_limits[i];
            goto out;
        }
    }
out:
    return use_limit;
}

static void
check_mount_use_limit(const char* mntpoint, disk_use_limit_t* use_limit)
{
    struct statfs stfs;
    int used_percent;

    memset(&stfs, 0, sizeof(stfs));

    if( statfs(mntpoint, &stfs) == -1 ) {
        dsme_log(LOG_WARNING, LOGPFIX"failed to statfs the mount point %s: %m", mntpoint);
        goto EXIT;
    }

    if( stfs.f_blocks <= 0 ) {
        /* Ignore silently */
        goto EXIT;
    }

    used_percent = (int)((stfs.f_blocks - stfs.f_bfree) * 100.f /
                                stfs.f_blocks + 0.5f);

    /* Broadcasting of diskspace low events is repeated - There is
     * no query mechanism and ui processes that listen to resulting
     * D-Bus signals might miss the first one. Also, if the optional
     * temp reaper feature is enabled, we want it to get triggered
     * after each state evaluation.
     *
     * However, return to normal state is broadcast only if diskspace
     * low has been signaled earlier (or once after dsme startup).
     */
    if (used_percent >= use_limit->max_usage_percent) {
        dsme_log(LOG_WARNING, LOGPFIX"disk space usage (%d%%) for (%s) exceeds the limit (%d%%)",
                 used_percent, mntpoint, use_limit->max_usage_percent);

        use_limit->signaled_state = DISKSPACE_STATE_WARNING;
    }
    else if( use_limit->signaled_state != DISKSPACE_STATE_NORMAL ) {
        if( use_limit->signaled_state != DISKSPACE_STATE_UNDEF )
            dsme_log(LOG_WARNING, LOGPFIX"disk space usage (%d%%) for (%s) within the limit (%d%%)",
                     used_percent, mntpoint, use_limit->max_usage_percent);

        use_limit->signaled_state = DISKSPACE_STATE_NORMAL;
    }
    else {
        /* No change - skip broadcast */
        goto EXIT;
    }

    DSM_MSGTYPE_DISK_SPACE msg = DSME_MSG_INIT(DSM_MSGTYPE_DISK_SPACE);
    msg.blocks_percent_used = used_percent;
    msg.diskspace_state = use_limit->signaled_state;
    broadcast_internally_with_extra(&msg, strlen(mntpoint) + 1, mntpoint);

EXIT:
    return;
}

void
check_disk_space_usage(void)
{
    static unsigned check_tag = 0;

    FILE *fh = 0;

    dsme_log(LOG_DEBUG, LOGPFIX"check disk space usage");

    if( !(fh = setmntent(_PATH_MOUNTED, "r")) )
        goto EXIT;

    struct mntent mnt;
    char          buf[1024];

    ++check_tag;

    while( getmntent_r(fh, &mnt, buf, sizeof buf) ) {
        disk_use_limit_t *lim = find_use_limit_for_mount(mnt.mnt_dir);

        if( !lim ) {
            /* No limits configured for this mountpoint */
            continue;
        }

        if( lim->check_tag == check_tag ) {
            /* The same mountpoint was already checked */
            continue;
        }

        dsme_log(LOG_DEBUG, LOGPFIX"check mountpoint: %s", mnt.mnt_dir);

        lim->check_tag = check_tag;
        check_mount_use_limit(mnt.mnt_dir, lim);
    }

EXIT:
    if( fh )
        endmntent(fh);
}
