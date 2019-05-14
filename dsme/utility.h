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

#ifndef  DSME_UTILITY_H_
# define DSME_UTILITY_H_

# include <stdbool.h>

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

bool dsme_home_is_encrypted(void);

#endif /* DSME_UTILITY_H_ */
