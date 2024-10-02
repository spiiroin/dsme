/**
   @file state-internal.h

   This file has defines for state module internal to DSME.
   <p>
   Copyright (C) 2010 Nokia Corporation.

   @author Semi Malinen <semi.malinen@nokia.com>

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

#ifndef DSME_STATE_INTERNAL_H
#define DSME_STATE_INTERNAL_H

#include <dsme/messages.h>
#include <stdbool.h>

typedef struct {
    DSMEMSG_PRIVATE_FIELDS
    bool mounted_to_pc;
} DSM_MSGTYPE_SET_USB_STATE;

typedef dsmemsg_generic_t DSM_MSGTYPE_TELINIT;
typedef dsmemsg_generic_t DSM_MSGTYPE_BLOCK_SHUTDOWN;
typedef dsmemsg_generic_t DSM_MSGTYPE_ALLOW_SHUTDOWN;

enum {
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

    DSME_MSG_ENUM(DSM_MSGTYPE_SET_USB_STATE,  0x00000317),
    DSME_MSG_ENUM(DSM_MSGTYPE_TELINIT,        0x00000318),
    DSME_MSG_ENUM(DSM_MSGTYPE_BLOCK_SHUTDOWN, 0x0000031b),
    DSME_MSG_ENUM(DSM_MSGTYPE_ALLOW_SHUTDOWN, 0x0000031c),
};

#endif
