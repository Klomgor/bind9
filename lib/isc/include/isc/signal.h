/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#pragma once

#include <stdlib.h>

#include <isc/loop.h>

typedef void (*isc_signal_cb)(void *, int);

isc_signal_t *
isc_signal_new(isc_signal_cb cb, void *cbarg, int signum);
/*%<
 * Create a new signal handler for loop manager 'loopmgr', handling
 * the signal value 'signum'.
 *
 * After isc_signal_start() is called on the returned signal handler,
 * and until isc_signal_stop() is called, if the running process receives
 * signal 'signum', 'cb' will be run with argument 'cbarg'.
 */

void
isc_signal_destroy(isc_signal_t **signalp);
/*%<
 * Free the memory allocated by isc_signal_new().
 */

void
isc_signal_start(isc_signal_t *signal);
/*%<
 * Start using the signal handler 'signal'.
 */

void
isc_signal_stop(isc_signal_t *signal);
/*%<
 * Stop using the signal handler 'signal'. (It can be restarted with
 * isc_signal_start().)
 */
