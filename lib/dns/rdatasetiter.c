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

/*! \file */

#include <stddef.h>

#include <isc/util.h>

#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>

void
dns__rdatasetiter_destroy(dns_rdatasetiter_t **iteratorp DNS__DB_FLARG) {
	REQUIRE(iteratorp != NULL);
	REQUIRE(DNS_RDATASETITER_VALID(*iteratorp));

	(*iteratorp)->methods->destroy(iteratorp DNS__DB_FLARG_PASS);

	ENSURE(*iteratorp == NULL);
}

isc_result_t
dns__rdatasetiter_first(dns_rdatasetiter_t *iterator DNS__DB_FLARG) {
	REQUIRE(DNS_RDATASETITER_VALID(iterator));

	isc_result_t result =
		iterator->methods->first(iterator DNS__DB_FLARG_PASS);
	ENSURE(result == ISC_R_SUCCESS || result == ISC_R_NOMORE);
	return result;
}

isc_result_t
dns__rdatasetiter_next(dns_rdatasetiter_t *iterator DNS__DB_FLARG) {
	REQUIRE(DNS_RDATASETITER_VALID(iterator));

	isc_result_t result =
		iterator->methods->next(iterator DNS__DB_FLARG_PASS);
	ENSURE(result == ISC_R_SUCCESS || result == ISC_R_NOMORE);
	return result;
}

void
dns__rdatasetiter_current(dns_rdatasetiter_t *iterator,
			  dns_rdataset_t *rdataset DNS__DB_FLARG) {
	REQUIRE(DNS_RDATASETITER_VALID(iterator));
	REQUIRE(DNS_RDATASET_VALID(rdataset));
	REQUIRE(!dns_rdataset_isassociated(rdataset));

	iterator->methods->current(iterator, rdataset DNS__DB_FLARG_PASS);
}
