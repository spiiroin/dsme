/**
 * @file utility.h
 *
 * Generic functions needed by dsme core and/or multiple plugings.
 *
 * <p>
 * Copyright (C) 2019 Jolla Ltd.
 *
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

#include "utility.h"

#include "../include/dsme/logging.h"

#include <string.h>

#include <libcryptsetup.h>

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

static void                 dsme_free_crypt_device        (struct crypt_device *cdev);
static struct crypt_device *dsme_get_crypt_device_for_home(void);
bool                        dsme_home_is_encrypted        (void);

/* ========================================================================= *
 * Probing for encrypted home partition
 * ========================================================================= */

static const char homeLuksContainer[] = "/dev/sailfish/home";

static void
dsme_free_crypt_device(struct crypt_device *cdev)
{
    if( cdev )
        crypt_free(cdev);
}

static struct crypt_device *
dsme_get_crypt_device_for_home(void)
{
    struct crypt_device *cdev = 0;
    struct crypt_device *work = 0;

    int rc;

    if( (rc = crypt_init(&work, homeLuksContainer)) < 0 ) {
        dsme_log(LOG_WARNING, "%s: could not initialize crypt device: %s",
                 homeLuksContainer, strerror(-rc));
        goto EXIT;
    }

    if( (rc = crypt_load(work, 0, 0)) < 0 ) {
        dsme_log(LOG_WARNING, "%s: could not load crypt device info: %s",
                 homeLuksContainer, strerror(-rc));
        goto EXIT;
    }

    cdev = work, work = 0;

EXIT:

    dsme_free_crypt_device(work);

    return cdev;
}

bool
dsme_home_is_encrypted(void)
{
    static bool is_encrypted = false;
    static bool was_probed = false;

    if( !was_probed ) {
        was_probed = true;

        struct crypt_device *cdev = dsme_get_crypt_device_for_home();
        is_encrypted = (cdev != 0);
        dsme_free_crypt_device(cdev);

        dsme_log(LOG_WARNING, "HOME is encrypted: %s",
                 is_encrypted ? "True" : "False");
    }

    return is_encrypted;
}
