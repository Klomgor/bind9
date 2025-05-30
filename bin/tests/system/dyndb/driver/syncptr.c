/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0 AND ISC
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*
 * Copyright (C) Red Hat
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND AUTHORS DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Automatic A/AAAA/PTR record synchronization.
 */

#include "syncptr.h"

#include <isc/async.h>
#include <isc/netaddr.h>
#include <isc/util.h>

#include <dns/byaddr.h>
#include <dns/db.h>
#include <dns/name.h>
#include <dns/view.h>
#include <dns/zone.h>

#include "instance.h"
#include "util.h"

/*
 * Event used for making changes to reverse zones.
 */
typedef struct syncptr syncptr_t;
struct syncptr {
	isc_mem_t *mctx;
	dns_zone_t *zone;
	dns_diff_t diff;
	dns_fixedname_t ptr_target_name; /* referenced by owner name in
					  * tuple */
	isc_buffer_t b; /* referenced by target name in tuple */
	unsigned char buf[DNS_NAME_MAXWIRE];
};

/*
 * Write diff generated in syncptr() to reverse zone.
 *
 * This function will be called asynchronously and syncptr() will not get
 * any result from it.
 *
 */
static void
syncptr_write(void *arg) {
	syncptr_t *syncptr = (syncptr_t *)arg;
	dns_dbversion_t *version = NULL;
	dns_db_t *db = NULL;
	isc_result_t result;

	log_write(ISC_LOG_INFO, "ENTER: syncptr_write");

	result = dns_zone_getdb(syncptr->zone, &db);
	if (result != ISC_R_SUCCESS) {
		log_write(ISC_LOG_ERROR,
			  "syncptr_write: dns_zone_getdb -> %s\n",
			  isc_result_totext(result));
		goto cleanup;
	}

	result = dns_db_newversion(db, &version);
	if (result != ISC_R_SUCCESS) {
		log_write(ISC_LOG_ERROR,
			  "syncptr_write: dns_db_newversion -> %s\n",
			  isc_result_totext(result));
		goto cleanup;
	}
	result = dns_diff_apply(&syncptr->diff, db, version);
	if (result != ISC_R_SUCCESS) {
		log_write(ISC_LOG_ERROR,
			  "syncptr_write: dns_diff_apply -> %s\n",
			  isc_result_totext(result));
		goto cleanup;
	}

cleanup:
	if (db != NULL) {
		if (version != NULL) {
			dns_db_closeversion(db, &version, true);
		}
		dns_db_detach(&db);
	}
	dns_zone_detach(&syncptr->zone);
	dns_diff_clear(&syncptr->diff);
	isc_mem_putanddetach(&syncptr->mctx, syncptr, sizeof(*syncptr));
}

/*
 * Find a reverse zone for given IP address.
 *
 * @param[in]  rdata      IP address as A/AAAA record
 * @param[out] name       Owner name for the PTR record
 * @param[out] zone       DNS zone for reverse record matching the IP address
 *
 * @retval ISC_R_SUCCESS  DNS name derived from given IP address belongs to an
 * 			  reverse zone managed by this driver instance.
 * 			  PTR record synchronization can continue.
 * @retval ISC_R_NOTFOUND Suitable reverse zone was not found because it
 * 			  does not exist or is not managed by this driver.
 */
static isc_result_t
syncptr_find_zone(sample_instance_t *inst, dns_rdata_t *rdata, dns_name_t *name,
		  dns_zone_t **zone) {
	isc_result_t result;
	isc_netaddr_t isc_ip; /* internal net address representation */
	dns_rdata_in_a_t ipv4;
	dns_rdata_in_aaaa_t ipv6;

	REQUIRE(inst != NULL);
	REQUIRE(zone != NULL && *zone == NULL);

	switch (rdata->type) {
	case dns_rdatatype_a:
		CHECK(dns_rdata_tostruct(rdata, &ipv4, inst->mctx));
		isc_netaddr_fromin(&isc_ip, &ipv4.in_addr);
		break;

	case dns_rdatatype_aaaa:
		CHECK(dns_rdata_tostruct(rdata, &ipv6, inst->mctx));
		isc_netaddr_fromin6(&isc_ip, &ipv6.in6_addr);
		break;

	default:
		FATAL_ERROR("unsupported address type 0x%x", rdata->type);
		break;
	}

	/*
	 * Convert IP address to PTR owner name.
	 *
	 * @example
	 * 192.168.0.1 -> 1.0.168.192.in-addr.arpa
	 */
	result = dns_byaddr_createptrname(&isc_ip, name);
	if (result != ISC_R_SUCCESS) {
		log_write(ISC_LOG_ERROR,
			  "syncptr_find_zone: dns_byaddr_createptrname -> %s\n",
			  isc_result_totext(result));
		goto cleanup;
	}

	/* Find a zone containing owner name of the PTR record. */
	result = dns_view_findzone(inst->view, name, 0, zone);
	if (result == DNS_R_PARTIALMATCH) {
		result = ISC_R_SUCCESS;
	} else if (result != ISC_R_SUCCESS) {
		log_write(ISC_LOG_ERROR,
			  "syncptr_find_zone: dns_zt_find -> %s\n",
			  isc_result_totext(result));
		goto cleanup;
	}

	/* Make sure that the zone is managed by this driver. */
	if (*zone != inst->zone1 && *zone != inst->zone2) {
		dns_zone_detach(zone);
		log_write(ISC_LOG_INFO, "syncptr_find_zone: zone not managed");
		result = ISC_R_NOTFOUND;
	}

cleanup:
	if (rdata->type == dns_rdatatype_a) {
		dns_rdata_freestruct(&ipv4);
	} else {
		dns_rdata_freestruct(&ipv6);
	}

	return result;
}

/*
 * Generate update event for PTR record to reflect change in A/AAAA record.
 *
 * @pre Reverse zone is managed by this driver.
 *
 * @param[in]  a_name  DNS domain of modified A/AAAA record
 * @param[in]  af      Address family
 * @param[in]  ip_str  IP address as a string (IPv4 or IPv6)
 * @param[in]  mod_op  LDAP_MOD_DELETE if A/AAAA record is being deleted
 *                     or LDAP_MOD_ADD if A/AAAA record is being added.
 *
 * @retval ISC_R_SUCCESS Event for PTR record update was generated and send.
 *                       Change to reverse zone will be done asynchronously.
 * @retval other	 Synchronization failed - reverse doesn't exist,
 * 			 is not managed by this driver instance,
 * 			 memory allocation error, etc.
 */
static isc_result_t
syncptr(sample_instance_t *inst, dns_name_t *name, dns_rdata_t *addr_rdata,
	dns_ttl_t ttl, dns_diffop_t op) {
	isc_result_t result;
	isc_mem_t *mctx = inst->mctx;
	dns_fixedname_t ptr_name;
	dns_zone_t *ptr_zone = NULL;
	dns_rdata_ptr_t ptr_struct;
	dns_rdata_t ptr_rdata = DNS_RDATA_INIT;
	dns_difftuple_t *tp = NULL;
	syncptr_t *syncptr = NULL;

	dns_fixedname_init(&ptr_name);
	DNS_RDATACOMMON_INIT(&ptr_struct, dns_rdatatype_ptr, dns_rdataclass_in);
	dns_name_init(&ptr_struct.ptr);

	syncptr = isc_mem_get(mctx, sizeof(*syncptr));
	*syncptr = (syncptr_t){ 0 };
	isc_mem_attach(mctx, &syncptr->mctx);
	isc_buffer_init(&syncptr->b, syncptr->buf, sizeof(syncptr->buf));
	dns_fixedname_init(&syncptr->ptr_target_name);

	/* Check if reverse zone is managed by this driver */
	result = syncptr_find_zone(inst, addr_rdata,
				   dns_fixedname_name(&ptr_name), &ptr_zone);
	if (result != ISC_R_SUCCESS) {
		log_error_r("PTR record synchronization skipped: reverse zone "
			    "is not managed by driver instance '%s'",
			    inst->db_name);
		goto cleanup;
	}

	/* Reverse zone is managed by this driver, prepare PTR record */
	dns_zone_attach(ptr_zone, &syncptr->zone);
	dns_name_copy(name, dns_fixedname_name(&syncptr->ptr_target_name));
	dns_name_clone(dns_fixedname_name(&syncptr->ptr_target_name),
		       &ptr_struct.ptr);
	dns_diff_init(inst->mctx, &syncptr->diff);
	result = dns_rdata_fromstruct(&ptr_rdata, dns_rdataclass_in,
				      dns_rdatatype_ptr, &ptr_struct,
				      &syncptr->b);
	if (result != ISC_R_SUCCESS) {
		log_write(ISC_LOG_ERROR,
			  "syncptr: dns_rdata_fromstruct -> %s\n",
			  isc_result_totext(result));
		goto cleanup;
	}

	/* Create diff */
	dns_difftuple_create(mctx, op, dns_fixedname_name(&ptr_name), ttl,
			     &ptr_rdata, &tp);
	dns_diff_append(&syncptr->diff, &tp);

	/*
	 * Send update event to the reverse zone.
	 * It will be processed asynchronously.
	 */
	isc_async_run(dns_zone_getloop(ptr_zone), syncptr_write, syncptr);
	syncptr = NULL;

cleanup:
	if (ptr_zone != NULL) {
		dns_zone_detach(&ptr_zone);
	}
	if (tp != NULL) {
		dns_difftuple_free(&tp);
	}
	if (syncptr != NULL) {
		isc_mem_put(mctx, syncptr, sizeof(*syncptr));
	}

	return result;
}

/*
 * Generate update event for every rdata in rdataset.
 *
 * @param[in]  name      Owner name for A/AAAA records in rdataset.
 * @param[in]  rdataset  A/AAAA records.
 * @param[in]  op	 DNS_DIFFOP_ADD / DNS_DIFFOP_DEL for adding / deleting
 * 			 the rdata
 */
isc_result_t
syncptrs(sample_instance_t *inst, dns_name_t *name, dns_rdataset_t *rdataset,
	 dns_diffop_t op) {
	isc_result_t result = ISC_R_SUCCESS;
	DNS_RDATASET_FOREACH (rdataset) {
		dns_rdata_t rdata = DNS_RDATA_INIT;
		dns_rdataset_current(rdataset, &rdata);
		result = syncptr(inst, name, &rdata, rdataset->ttl, op);
		if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND) {
			break;
		}
	}

	return result;
}
