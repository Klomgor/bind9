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

/*! \file isccfg/check.h */

#include <isc/types.h>

#include <isccfg/cfg.h>

#ifndef MAX_MIN_CACHE_TTL
#define MAX_MIN_CACHE_TTL 90
#endif /* MAX_MIN_CACHE_TTL */

#ifndef MAX_MIN_NCACHE_TTL
#define MAX_MIN_NCACHE_TTL 90
#endif /* MAX_MIN_NCACHE_TTL */

#ifndef MAX_MAX_NCACHE_TTL
#define MAX_MAX_NCACHE_TTL 7 * 24 * 3600
#endif /* MAX_MAX_NCACHE_TTL */

#define BIND_CHECK_PLUGINS 0x00000001
/*%<
 * Check the plugin configuration.
 */
#define BIND_CHECK_ALGORITHMS 0x00000002
/*%<
 * Check the dnssec-policy DNSSEC algorithms against those
 * supported by the crypto provider.
 */

isc_result_t
isccfg_check_namedconf(const cfg_obj_t *config, unsigned int flags,
		       isc_mem_t *mctx);
/*%<
 * Check the syntactic validity of a configuration parse tree generated from
 * a named.conf file.
 *
 * If 'check_plugins' is true, load plugins and check the validity of their
 * parameters as well.
 *
 * Requires:
 *\li	config is a valid parse tree
 *
 *\li	logctx is a valid logging context.
 *
 * Returns:
 * \li	#ISC_R_SUCCESS
 * \li	#ISC_R_FAILURE
 */

isc_result_t
isccfg_check_key(const cfg_obj_t *config);
/*%<
 * Same as isccfg_check_namedconf(), but for a single 'key' statement.
 */
