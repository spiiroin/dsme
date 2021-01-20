/**
 * @file utility.h
 *
 * Generic functions needed by dsme core and/or multiple plugings.
 *
 * <p>
 * Copyright (c) 2019 - 2020 Jolla Ltd.
 * Copyright (c) 2020 Open Mobile Platform LLC.
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

#ifndef  DSME_UTILITY_H_
# define DSME_UTILITY_H_

# include <dsme/state.h>

# include <stdbool.h>

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

bool        dsme_user_is_privileged(uid_t uid, gid_t gid);
bool        dsme_process_is_privileged(pid_t pid);
bool        dsme_home_is_encrypted(void);
const char *dsme_state_repr       (dsme_state_t state);

char* dsme_pid2text(pid_t pid);

#endif /* DSME_UTILITY_H_ */
