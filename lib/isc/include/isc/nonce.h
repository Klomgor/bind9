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

/*! \file isc/nonce.h
 * \brief Provides a function for generating an arbitrarily long nonce.
 */

void
isc_nonce_buf(void *buf, size_t buflen);
/*!<
 * Fill 'buf', up to 'buflen' bytes, with random data from the
 * crypto provider's random function.
 */
