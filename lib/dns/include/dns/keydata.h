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

/*****
***** Module Info
*****/

/*! \file dns/keydata.h
 * \brief
 * KEYDATA utilities.
 */

/***
 *** Imports
 ***/

#include <inttypes.h>

#include <isc/types.h>

#include <dns/rdatastruct.h>
#include <dns/types.h>

isc_result_t
dns_keydata_todnskey(dns_rdata_keydata_t *keydata, dns_rdata_dnskey_t *dnskey,
		     isc_mem_t *mctx);

isc_result_t
dns_keydata_fromdnskey(dns_rdata_keydata_t *keydata, dns_rdata_dnskey_t *dnskey,
		       uint32_t refresh, uint32_t addhd, uint32_t removehd,
		       isc_mem_t *mctx);
