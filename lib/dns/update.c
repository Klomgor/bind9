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

#include <inttypes.h>
#include <stdbool.h>
#include <time.h>

#include <isc/log.h>
#include <isc/magic.h>
#include <isc/mem.h>
#include <isc/netaddr.h>
#include <isc/random.h>
#include <isc/result.h>
#include <isc/serial.h>
#include <isc/stats.h>
#include <isc/stdtime.h>
#include <isc/string.h>
#include <isc/time.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/diff.h>
#include <dns/dnssec.h>
#include <dns/fixedname.h>
#include <dns/journal.h>
#include <dns/kasp.h>
#include <dns/keyvalues.h>
#include <dns/message.h>
#include <dns/nsec.h>
#include <dns/nsec3.h>
#include <dns/private.h>
#include <dns/rdataclass.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatastruct.h>
#include <dns/rdatatype.h>
#include <dns/skr.h>
#include <dns/soa.h>
#include <dns/ssu.h>
#include <dns/stats.h>
#include <dns/tsig.h>
#include <dns/update.h>
#include <dns/view.h>
#include <dns/zone.h>
#include <dns/zt.h>

/**************************************************************************/

#define STATE_MAGIC	       ISC_MAGIC('S', 'T', 'T', 'E')
#define DNS_STATE_VALID(state) ISC_MAGIC_VALID(state, STATE_MAGIC)

/*%
 * Log level for tracing dynamic update protocol requests.
 */
#define LOGLEVEL_PROTOCOL ISC_LOG_INFO

/*%
 * Log level for low-level debug tracing.
 */
#define LOGLEVEL_DEBUG ISC_LOG_DEBUG(8)

/*%
 * Check an operation for failure.  These macros all assume that
 * the function using them has a 'result' variable and a 'failure'
 * label.
 */
#define CHECK(op)                            \
	do {                                 \
		result = (op);               \
		if (result != ISC_R_SUCCESS) \
			goto failure;        \
	} while (0)

/*%
 * Fail unconditionally with result 'code', which must not
 * be ISC_R_SUCCESS.  The reason for failure presumably has
 * been logged already.
 *
 * The test against ISC_R_SUCCESS is there to keep the Solaris compiler
 * from complaining about "end-of-loop code not reached".
 */

#define FAIL(code)                           \
	do {                                 \
		result = (code);             \
		if (result != ISC_R_SUCCESS) \
			goto failure;        \
	} while (0)

/*%
 * Fail unconditionally and log as a client error.
 * The test against ISC_R_SUCCESS is there to keep the Solaris compiler
 * from complaining about "end-of-loop code not reached".
 */
#define FAILC(code, msg)                                                       \
	do {                                                                   \
		const char *_what = "failed";                                  \
		result = (code);                                               \
		switch (result) {                                              \
		case DNS_R_NXDOMAIN:                                           \
		case DNS_R_YXDOMAIN:                                           \
		case DNS_R_YXRRSET:                                            \
		case DNS_R_NXRRSET:                                            \
			_what = "unsuccessful";                                \
		}                                                              \
		update_log(log, zone, LOGLEVEL_PROTOCOL, "update %s: %s (%s)", \
			   _what, msg, isc_result_totext(result));             \
		if (result != ISC_R_SUCCESS)                                   \
			goto failure;                                          \
	} while (0)

#define FAILN(code, name, msg)                                             \
	do {                                                               \
		const char *_what = "failed";                              \
		result = (code);                                           \
		switch (result) {                                          \
		case DNS_R_NXDOMAIN:                                       \
		case DNS_R_YXDOMAIN:                                       \
		case DNS_R_YXRRSET:                                        \
		case DNS_R_NXRRSET:                                        \
			_what = "unsuccessful";                            \
		}                                                          \
		if (isc_log_wouldlog(LOGLEVEL_PROTOCOL)) {                 \
			char _nbuf[DNS_NAME_FORMATSIZE];                   \
			dns_name_format(name, _nbuf, sizeof(_nbuf));       \
			update_log(log, zone, LOGLEVEL_PROTOCOL,           \
				   "update %s: %s: %s (%s)", _what, _nbuf, \
				   msg, isc_result_totext(result));        \
		}                                                          \
		if (result != ISC_R_SUCCESS)                               \
			goto failure;                                      \
	} while (0)

#define FAILNT(code, name, type, msg)                                         \
	do {                                                                  \
		const char *_what = "failed";                                 \
		result = (code);                                              \
		switch (result) {                                             \
		case DNS_R_NXDOMAIN:                                          \
		case DNS_R_YXDOMAIN:                                          \
		case DNS_R_YXRRSET:                                           \
		case DNS_R_NXRRSET:                                           \
			_what = "unsuccessful";                               \
		}                                                             \
		if (isc_log_wouldlog(LOGLEVEL_PROTOCOL)) {                    \
			char _nbuf[DNS_NAME_FORMATSIZE];                      \
			char _tbuf[DNS_RDATATYPE_FORMATSIZE];                 \
			dns_name_format(name, _nbuf, sizeof(_nbuf));          \
			dns_rdatatype_format(type, _tbuf, sizeof(_tbuf));     \
			update_log(log, zone, LOGLEVEL_PROTOCOL,              \
				   "update %s: %s/%s: %s (%s)", _what, _nbuf, \
				   _tbuf, msg, isc_result_totext(result));    \
		}                                                             \
		if (result != ISC_R_SUCCESS)                                  \
			goto failure;                                         \
	} while (0)

/*%
 * Fail unconditionally and log as a server error.
 * The test against ISC_R_SUCCESS is there to keep the Solaris compiler
 * from complaining about "end-of-loop code not reached".
 */
#define FAILS(code, msg)                                                       \
	do {                                                                   \
		result = (code);                                               \
		update_log(log, zone, LOGLEVEL_PROTOCOL, "error: %s: %s", msg, \
			   isc_result_totext(result));                         \
		if (result != ISC_R_SUCCESS)                                   \
			goto failure;                                          \
	} while (0)

/**************************************************************************/

typedef struct rr rr_t;

struct rr {
	/* dns_name_t name; */
	uint32_t ttl;
	dns_rdata_t rdata;
};

typedef struct update_event update_event_t;

/**************************************************************************/

static void
update_log(dns_update_log_t *callback, dns_zone_t *zone, int level,
	   const char *fmt, ...) ISC_FORMAT_PRINTF(4, 5);

static void
update_log(dns_update_log_t *callback, dns_zone_t *zone, int level,
	   const char *fmt, ...) {
	va_list ap;
	char message[4096];

	if (callback == NULL) {
		return;
	}

	if (!isc_log_wouldlog(level)) {
		return;
	}

	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	(callback->func)(callback->arg, zone, level, message);
}

/*%
 * Update a single RR in version 'ver' of 'db' and log the
 * update in 'diff'.
 *
 * Ensures:
 * \li	'*tuple' == NULL.  Either the tuple is freed, or its
 *	ownership has been transferred to the diff.
 */
static isc_result_t
do_one_tuple(dns_difftuple_t **tuple, dns_db_t *db, dns_dbversion_t *ver,
	     dns_diff_t *diff) {
	dns_diff_t temp_diff;
	isc_result_t result;

	/*
	 * Create a singleton diff.
	 */
	dns_diff_init(diff->mctx, &temp_diff);
	ISC_LIST_APPEND(temp_diff.tuples, *tuple, link);

	/*
	 * Apply it to the database.
	 */
	result = dns_diff_apply(&temp_diff, db, ver);
	ISC_LIST_UNLINK(temp_diff.tuples, *tuple, link);
	if (result != ISC_R_SUCCESS) {
		dns_difftuple_free(tuple);
		return result;
	}

	/*
	 * Merge it into the current pending journal entry.
	 */
	dns_diff_appendminimal(diff, tuple);

	/*
	 * Do not clear temp_diff.
	 */
	return ISC_R_SUCCESS;
}

static isc_result_t
update_one_rr(dns_db_t *db, dns_dbversion_t *ver, dns_diff_t *diff,
	      dns_diffop_t op, dns_name_t *name, dns_ttl_t ttl,
	      dns_rdata_t *rdata) {
	dns_difftuple_t *tuple = NULL;
	dns_difftuple_create(diff->mctx, op, name, ttl, rdata, &tuple);
	return do_one_tuple(&tuple, db, ver, diff);
}

/**************************************************************************/
/*
 * Callback-style iteration over rdatasets and rdatas.
 *
 * foreach_rrset() can be used to iterate over the RRsets
 * of a name and call a callback function with each
 * one.  Similarly, foreach_rr() can be used to iterate
 * over the individual RRs at name, optionally restricted
 * to RRs of a given type.
 *
 * The callback functions are called "actions" and take
 * two arguments: a void pointer for passing arbitrary
 * context information, and a pointer to the current RRset
 * or RR.  By convention, their names end in "_action".
 */

/*
 * XXXRTH  We might want to make this public somewhere in libdns.
 */

/*%
 * Function type for foreach_rrset() iterator actions.
 */
typedef isc_result_t
rrset_func(void *data, dns_rdataset_t *rrset);

/*%
 * Function type for foreach_rr() iterator actions.
 */
typedef isc_result_t
rr_func(void *data, rr_t *rr);

/*%
 * Internal context struct for foreach_node_rr().
 */
typedef struct {
	rr_func *rr_action;
	void *rr_action_data;
} foreach_node_rr_ctx_t;

/*%
 * Internal helper function for foreach_node_rr().
 */
static isc_result_t
foreach_node_rr_action(void *data, dns_rdataset_t *rdataset) {
	isc_result_t result;
	foreach_node_rr_ctx_t *ctx = data;
	DNS_RDATASET_FOREACH (rdataset) {
		rr_t rr = { 0, DNS_RDATA_INIT };

		dns_rdataset_current(rdataset, &rr.rdata);
		rr.ttl = rdataset->ttl;
		result = (*ctx->rr_action)(ctx->rr_action_data, &rr);
		if (result != ISC_R_SUCCESS) {
			return result;
		}
	}

	return ISC_R_SUCCESS;
}

/*%
 * For each rdataset of 'name' in 'ver' of 'db', call 'action'
 * with the rdataset and 'action_data' as arguments.  If the name
 * does not exist, do nothing.
 *
 * If 'action' returns an error, abort iteration and return the error.
 */
static isc_result_t
foreach_rrset(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
	      rrset_func *action, void *action_data) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	dns_rdatasetiter_t *iter = NULL;

	result = dns_db_findnode(db, name, false, &node);
	if (result == ISC_R_NOTFOUND) {
		return ISC_R_SUCCESS;
	}
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	result = dns_db_allrdatasets(db, node, ver, 0, (isc_stdtime_t)0, &iter);
	if (result != ISC_R_SUCCESS) {
		goto cleanup_node;
	}

	DNS_RDATASETITER_FOREACH (iter) {
		dns_rdataset_t rdataset = DNS_RDATASET_INIT;

		dns_rdatasetiter_current(iter, &rdataset);

		result = (*action)(action_data, &rdataset);

		dns_rdataset_disassociate(&rdataset);
		if (result != ISC_R_SUCCESS) {
			break;
		}
	}
	dns_rdatasetiter_destroy(&iter);

cleanup_node:
	dns_db_detachnode(db, &node);

	return result;
}

/*%
 * For each RR of 'name' in 'ver' of 'db', call 'action'
 * with the RR and 'action_data' as arguments.  If the name
 * does not exist, do nothing.
 *
 * If 'action' returns an error, abort iteration
 * and return the error.
 */
static isc_result_t
foreach_node_rr(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
		rr_func *rr_action, void *rr_action_data) {
	foreach_node_rr_ctx_t ctx;
	ctx.rr_action = rr_action;
	ctx.rr_action_data = rr_action_data;
	return foreach_rrset(db, ver, name, foreach_node_rr_action, &ctx);
}

/*%
 * For each of the RRs specified by 'db', 'ver', 'name', 'type',
 * (which can be dns_rdatatype_any to match any type), and 'covers', call
 * 'action' with the RR and 'action_data' as arguments. If the name
 * does not exist, or if no RRset of the given type exists at the name,
 * do nothing.
 *
 * If 'action' returns an error, abort iteration and return the error.
 */
static isc_result_t
foreach_rr(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
	   dns_rdatatype_t type, dns_rdatatype_t covers, rr_func *rr_action,
	   void *rr_action_data) {
	isc_result_t result;
	dns_dbnode_t *node;
	dns_rdataset_t rdataset;

	if (type == dns_rdatatype_any) {
		return foreach_node_rr(db, ver, name, rr_action,
				       rr_action_data);
	}

	node = NULL;
	if (type == dns_rdatatype_nsec3 ||
	    (type == dns_rdatatype_rrsig && covers == dns_rdatatype_nsec3))
	{
		result = dns_db_findnsec3node(db, name, false, &node);
	} else {
		result = dns_db_findnode(db, name, false, &node);
	}
	if (result == ISC_R_NOTFOUND) {
		return ISC_R_SUCCESS;
	}
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	dns_rdataset_init(&rdataset);
	result = dns_db_findrdataset(db, node, ver, type, covers,
				     (isc_stdtime_t)0, &rdataset, NULL);
	if (result == ISC_R_NOTFOUND) {
		result = ISC_R_SUCCESS;
		goto cleanup_node;
	}
	if (result != ISC_R_SUCCESS) {
		goto cleanup_node;
	}

	DNS_RDATASET_FOREACH (&rdataset) {
		rr_t rr = { 0, DNS_RDATA_INIT };
		dns_rdataset_current(&rdataset, &rr.rdata);
		rr.ttl = rdataset.ttl;
		result = (*rr_action)(rr_action_data, &rr);
		if (result != ISC_R_SUCCESS) {
			goto cleanup_rdataset;
		}
	}

	result = ISC_R_SUCCESS;

cleanup_rdataset:
	dns_rdataset_disassociate(&rdataset);
cleanup_node:
	dns_db_detachnode(db, &node);

	return result;
}

/**************************************************************************/
/*
 * Various tests on the database contents (for prerequisites, etc).
 */

/*%
 * Function type for predicate functions that compare a database RR 'db_rr'
 * against an update RR 'update_rr'.
 */
typedef bool
rr_predicate(dns_rdata_t *update_rr, dns_rdata_t *db_rr);

/*%
 * Helper function for rrset_exists().
 */
static isc_result_t
rrset_exists_action(void *data, rr_t *rr) {
	UNUSED(data);
	UNUSED(rr);
	return ISC_R_EXISTS;
}

/*%
 * Utility macro for RR existence checking functions.
 *
 * If the variable 'result' has the value ISC_R_EXISTS or
 * ISC_R_SUCCESS, set *exists to true or false,
 * respectively, and return success.
 *
 * If 'result' has any other value, there was a failure.
 * Return the failure result code and do not set *exists.
 *
 * This would be more readable as "do { if ... } while(0)",
 * but that form generates tons of warnings on Solaris 2.6.
 */
#define RETURN_EXISTENCE_FLAG                                         \
	return ((result == ISC_R_EXISTS)                              \
			? (*exists = true, ISC_R_SUCCESS)             \
			: ((result == ISC_R_SUCCESS)                  \
				   ? (*exists = false, ISC_R_SUCCESS) \
				   : result))

/*%
 * Set '*exists' to true iff an rrset of the given type exists,
 * to false otherwise.
 */
static isc_result_t
rrset_exists(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
	     dns_rdatatype_t type, dns_rdatatype_t covers, bool *exists) {
	isc_result_t result;
	result = foreach_rr(db, ver, name, type, covers, rrset_exists_action,
			    NULL);
	RETURN_EXISTENCE_FLAG;
}

/*%
 * Set '*visible' to true if the RRset exists and is part of the
 * visible zone.  Otherwise '*visible' is set to false unless a
 * error occurs.
 */
static isc_result_t
rrset_visible(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
	      dns_rdatatype_t type, bool *visible) {
	isc_result_t result;
	dns_fixedname_t fixed;

	dns_fixedname_init(&fixed);
	result = dns_db_find(db, name, ver, type, DNS_DBFIND_NOWILD,
			     (isc_stdtime_t)0, NULL, dns_fixedname_name(&fixed),
			     NULL, NULL);
	switch (result) {
	case ISC_R_SUCCESS:
		*visible = true;
		break;
	/*
	 * Glue, obscured, deleted or replaced records.
	 */
	case DNS_R_DELEGATION:
	case DNS_R_DNAME:
	case DNS_R_CNAME:
	case DNS_R_NXDOMAIN:
	case DNS_R_NXRRSET:
	case DNS_R_EMPTYNAME:
	case DNS_R_COVERINGNSEC:
		*visible = false;
		result = ISC_R_SUCCESS;
		break;
	default:
		*visible = false; /* silence false compiler warning */
		break;
	}
	return result;
}

/*%
 * Context struct and helper function for name_exists().
 */

static isc_result_t
name_exists_action(void *data, dns_rdataset_t *rrset) {
	UNUSED(data);
	UNUSED(rrset);
	return ISC_R_EXISTS;
}

/*%
 * Set '*exists' to true iff the given name exists, to false otherwise.
 */
static isc_result_t
name_exists(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
	    bool *exists) {
	isc_result_t result;
	result = foreach_rrset(db, ver, name, name_exists_action, NULL);
	RETURN_EXISTENCE_FLAG;
}

/**************************************************************************/
/*
 * Checking of "RRset exists (value dependent)" prerequisites.
 *
 * In the RFC2136 section 3.2.5, this is the pseudocode involving
 * a variable called "temp", a mapping of <name, type> tuples to rrsets.
 *
 * Here, we represent the "temp" data structure as (non-minimal) "dns_diff_t"
 * where each tuple has op==DNS_DIFFOP_EXISTS.
 */

/*%
 * A comparison function defining the sorting order for the entries
 * in the "temp" data structure.  The major sort key is the owner name,
 * followed by the type and rdata.
 */
static int
temp_order(const void *av, const void *bv) {
	dns_difftuple_t const *const *ap = av;
	dns_difftuple_t const *const *bp = bv;
	dns_difftuple_t const *a = *ap;
	dns_difftuple_t const *b = *bp;
	int r;
	r = dns_name_compare(&a->name, &b->name);
	if (r != 0) {
		return r;
	}
	r = (b->rdata.type - a->rdata.type);
	if (r != 0) {
		return r;
	}
	r = dns_rdata_casecompare(&a->rdata, &b->rdata);
	return r;
}

/**************************************************************************/
/*
 * Conditional deletion of RRs.
 */

/*%
 * Context structure for delete_if().
 */

typedef struct {
	rr_predicate *predicate;
	dns_db_t *db;
	dns_dbversion_t *ver;
	dns_diff_t *diff;
	dns_name_t *name;
	dns_rdata_t *update_rr;
} conditional_delete_ctx_t;

/*%
 * Predicate functions for delete_if().
 */

/*%
 * Return true always.
 */
static bool
true_p(dns_rdata_t *update_rr, dns_rdata_t *db_rr) {
	UNUSED(update_rr);
	UNUSED(db_rr);
	return true;
}

/*%
 * Return true if the record is a RRSIG.
 */
static bool
rrsig_p(dns_rdata_t *update_rr, dns_rdata_t *db_rr) {
	UNUSED(update_rr);
	return (db_rr->type == dns_rdatatype_rrsig) ? true : false;
}

/*%
 * Internal helper function for delete_if().
 */
static isc_result_t
delete_if_action(void *data, rr_t *rr) {
	conditional_delete_ctx_t *ctx = data;
	if ((*ctx->predicate)(ctx->update_rr, &rr->rdata)) {
		isc_result_t result;
		result = update_one_rr(ctx->db, ctx->ver, ctx->diff,
				       DNS_DIFFOP_DEL, ctx->name, rr->ttl,
				       &rr->rdata);
		return result;
	} else {
		return ISC_R_SUCCESS;
	}
}

/*%
 * Conditionally delete RRs.  Apply 'predicate' to the RRs
 * specified by 'db', 'ver', 'name', and 'type' (which can
 * be dns_rdatatype_any to match any type).  Delete those
 * RRs for which the predicate returns true, and log the
 * deletions in 'diff'.
 */
static isc_result_t
delete_if(rr_predicate *predicate, dns_db_t *db, dns_dbversion_t *ver,
	  dns_name_t *name, dns_rdatatype_t type, dns_rdatatype_t covers,
	  dns_rdata_t *update_rr, dns_diff_t *diff) {
	conditional_delete_ctx_t ctx;
	ctx.predicate = predicate;
	ctx.db = db;
	ctx.ver = ver;
	ctx.diff = diff;
	ctx.name = name;
	ctx.update_rr = update_rr;
	return foreach_rr(db, ver, name, type, covers, delete_if_action, &ctx);
}

/**************************************************************************/
/*
 * Incremental updating of NSECs and RRSIGs.
 */

/*%
 * We abuse the dns_diff_t type to represent a set of domain names
 * affected by the update.
 */
static void
namelist_append_name(dns_diff_t *list, dns_name_t *name) {
	dns_difftuple_t *tuple = NULL;
	static dns_rdata_t dummy_rdata = DNS_RDATA_INIT;

	dns_difftuple_create(list->mctx, DNS_DIFFOP_EXISTS, name, 0,
			     &dummy_rdata, &tuple);
	dns_diff_append(list, &tuple);
}

static isc_result_t
namelist_append_subdomain(dns_db_t *db, dns_name_t *name,
			  dns_diff_t *affected) {
	isc_result_t result;
	dns_fixedname_t fixedname;
	dns_name_t *child;
	dns_dbiterator_t *dbit = NULL;

	child = dns_fixedname_initname(&fixedname);

	CHECK(dns_db_createiterator(db, DNS_DB_NONSEC3, &dbit));

	for (result = dns_dbiterator_seek(dbit, name); result == ISC_R_SUCCESS;
	     result = dns_dbiterator_next(dbit))
	{
		dns_dbnode_t *node = NULL;
		CHECK(dns_dbiterator_current(dbit, &node, child));
		dns_db_detachnode(db, &node);
		if (!dns_name_issubdomain(child, name)) {
			break;
		}
		namelist_append_name(affected, child);
	}
	if (result == ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}
failure:
	if (dbit != NULL) {
		dns_dbiterator_destroy(&dbit);
	}
	return result;
}

/*%
 * Helper function for non_nsec_rrset_exists().
 */
static isc_result_t
is_non_nsec_action(void *data, dns_rdataset_t *rrset) {
	UNUSED(data);
	if (!(rrset->type == dns_rdatatype_nsec ||
	      rrset->type == dns_rdatatype_nsec3 ||
	      (rrset->type == dns_rdatatype_rrsig &&
	       (rrset->covers == dns_rdatatype_nsec ||
		rrset->covers == dns_rdatatype_nsec3))))
	{
		return ISC_R_EXISTS;
	}
	return ISC_R_SUCCESS;
}

/*%
 * Check whether there is an rrset other than a NSEC or RRSIG NSEC,
 * i.e., anything that justifies the continued existence of a name
 * after a secure update.
 *
 * If such an rrset exists, set '*exists' to true.
 * Otherwise, set it to false.
 */
static isc_result_t
non_nsec_rrset_exists(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
		      bool *exists) {
	isc_result_t result;
	result = foreach_rrset(db, ver, name, is_non_nsec_action, NULL);
	RETURN_EXISTENCE_FLAG;
}

/*%
 * A comparison function for sorting dns_diff_t:s by name.
 */
static int
name_order(const void *av, const void *bv) {
	dns_difftuple_t const *const *ap = av;
	dns_difftuple_t const *const *bp = bv;
	dns_difftuple_t const *a = *ap;
	dns_difftuple_t const *b = *bp;
	return dns_name_compare(&a->name, &b->name);
}

static isc_result_t
uniqify_name_list(dns_diff_t *list) {
	isc_result_t result;

	CHECK(dns_diff_sort(list, name_order));

	dns_name_t *curr_name = NULL;
	ISC_LIST_FOREACH (list->tuples, p, link) {
		if (curr_name == NULL || !dns_name_equal(curr_name, &p->name)) {
			curr_name = &(p->name);
		} else {
			ISC_LIST_UNLINK(list->tuples, p, link);
			dns_difftuple_free(&p);
		}
	}
failure:
	return result;
}

static isc_result_t
is_active(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name, bool *flag,
	  bool *cut, bool *unsecure) {
	isc_result_t result;
	dns_fixedname_t foundname;
	dns_fixedname_init(&foundname);
	result = dns_db_find(db, name, ver, dns_rdatatype_any,
			     DNS_DBFIND_GLUEOK | DNS_DBFIND_NOWILD,
			     (isc_stdtime_t)0, NULL,
			     dns_fixedname_name(&foundname), NULL, NULL);
	if (result == ISC_R_SUCCESS || result == DNS_R_EMPTYNAME) {
		*flag = true;
		*cut = false;
		SET_IF_NOT_NULL(unsecure, false);
		return ISC_R_SUCCESS;
	} else if (result == DNS_R_ZONECUT) {
		*flag = true;
		*cut = true;
		if (unsecure != NULL) {
			/*
			 * We are at the zonecut.  Check to see if there
			 * is a DS RRset.
			 */
			if (dns_db_find(db, name, ver, dns_rdatatype_ds, 0,
					(isc_stdtime_t)0, NULL,
					dns_fixedname_name(&foundname), NULL,
					NULL) == DNS_R_NXRRSET)
			{
				*unsecure = true;
			} else {
				*unsecure = false;
			}
		}
		return ISC_R_SUCCESS;
	} else if (result == DNS_R_GLUE || result == DNS_R_DNAME ||
		   result == DNS_R_DELEGATION || result == DNS_R_NXDOMAIN)
	{
		*flag = false;
		*cut = false;
		SET_IF_NOT_NULL(unsecure, false);
		return ISC_R_SUCCESS;
	} else {
		/*
		 * Silence compiler.
		 */
		*flag = false;
		*cut = false;
		SET_IF_NOT_NULL(unsecure, false);
		return result;
	}
}

/*%
 * Find the next/previous name that has a NSEC record.
 * In other words, skip empty database nodes and names that
 * have had their NSECs removed because they are obscured by
 * a zone cut.
 */
static isc_result_t
next_active(dns_update_log_t *log, dns_zone_t *zone, dns_db_t *db,
	    dns_dbversion_t *ver, dns_name_t *oldname, dns_name_t *newname,
	    bool forward) {
	isc_result_t result;
	dns_dbiterator_t *dbit = NULL;
	bool has_nsec = false;
	unsigned int wraps = 0;
	bool secure = dns_db_issecure(db);

	CHECK(dns_db_createiterator(db, 0, &dbit));

	CHECK(dns_dbiterator_seek(dbit, oldname));
	do {
		dns_dbnode_t *node = NULL;

		if (forward) {
			result = dns_dbiterator_next(dbit);
		} else {
			result = dns_dbiterator_prev(dbit);
		}
		if (result == ISC_R_NOMORE) {
			/*
			 * Wrap around.
			 */
			if (forward) {
				CHECK(dns_dbiterator_first(dbit));
			} else {
				CHECK(dns_dbiterator_last(dbit));
			}
			wraps++;
			if (wraps == 2) {
				update_log(log, zone, ISC_LOG_ERROR,
					   "secure zone with no NSECs");
				result = DNS_R_BADZONE;
				goto failure;
			}
		}
		CHECK(dns_dbiterator_current(dbit, &node, newname));
		dns_db_detachnode(db, &node);

		/*
		 * The iterator may hold the tree lock, and
		 * rrset_exists() calls dns_db_findnode() which
		 * may try to reacquire it.  To avoid deadlock
		 * we must pause the iterator first.
		 */
		CHECK(dns_dbiterator_pause(dbit));
		if (secure) {
			CHECK(rrset_exists(db, ver, newname, dns_rdatatype_nsec,
					   0, &has_nsec));
		} else {
			dns_fixedname_t ffound;
			dns_name_t *found;
			found = dns_fixedname_initname(&ffound);
			result = dns_db_find(
				db, newname, ver, dns_rdatatype_soa,
				DNS_DBFIND_NOWILD, 0, NULL, found, NULL, NULL);
			if (result == ISC_R_SUCCESS ||
			    result == DNS_R_EMPTYNAME ||
			    result == DNS_R_NXRRSET || result == DNS_R_CNAME ||
			    (result == DNS_R_DELEGATION &&
			     dns_name_equal(newname, found)))
			{
				has_nsec = true;
				result = ISC_R_SUCCESS;
			} else if (result != DNS_R_NXDOMAIN) {
				break;
			}
		}
	} while (!has_nsec);
failure:
	if (dbit != NULL) {
		dns_dbiterator_destroy(&dbit);
	}

	return result;
}

/*%
 * Add a NSEC record for "name", recording the change in "diff".
 * The existing NSEC is removed.
 */
static isc_result_t
add_nsec(dns_update_log_t *log, dns_zone_t *zone, dns_db_t *db,
	 dns_dbversion_t *ver, dns_name_t *name, dns_ttl_t nsecttl,
	 dns_diff_t *diff) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	unsigned char buffer[DNS_NSEC_BUFFERSIZE];
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_difftuple_t *tuple = NULL;
	dns_fixedname_t fixedname;
	dns_name_t *target;

	target = dns_fixedname_initname(&fixedname);

	/*
	 * Find the successor name, aka NSEC target.
	 */
	CHECK(next_active(log, zone, db, ver, name, target, true));

	/*
	 * Create the NSEC RDATA.
	 */
	CHECK(dns_db_findnode(db, name, false, &node));
	dns_rdata_init(&rdata);
	CHECK(dns_nsec_buildrdata(db, ver, node, target, buffer, &rdata));
	dns_db_detachnode(db, &node);

	/*
	 * Delete the old NSEC and record the change.
	 */
	CHECK(delete_if(true_p, db, ver, name, dns_rdatatype_nsec, 0, NULL,
			diff));
	/*
	 * Add the new NSEC and record the change.
	 */
	dns_difftuple_create(diff->mctx, DNS_DIFFOP_ADD, name, nsecttl, &rdata,
			     &tuple);
	CHECK(do_one_tuple(&tuple, db, ver, diff));
	INSIST(tuple == NULL);

failure:
	if (node != NULL) {
		dns_db_detachnode(db, &node);
	}
	return result;
}

/*%
 * Add a placeholder NSEC record for "name", recording the change in "diff".
 */
static isc_result_t
add_placeholder_nsec(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
		     dns_diff_t *diff) {
	isc_result_t result;
	dns_difftuple_t *tuple = NULL;
	isc_region_t r;
	unsigned char data[1] = { 0 }; /* The root domain, no bits. */
	dns_rdata_t rdata = DNS_RDATA_INIT;

	r.base = data;
	r.length = sizeof(data);
	dns_rdata_fromregion(&rdata, dns_db_class(db), dns_rdatatype_nsec, &r);
	dns_difftuple_create(diff->mctx, DNS_DIFFOP_ADD, name, 0, &rdata,
			     &tuple);
	CHECK(do_one_tuple(&tuple, db, ver, diff));
failure:
	return result;
}

static isc_result_t
find_zone_keys(dns_zone_t *zone, isc_mem_t *mctx, unsigned int maxkeys,
	       dst_key_t **keys, unsigned int *nkeys) {
	dns_dnsseckeylist_t keylist;
	unsigned int count = 0;
	isc_result_t result;
	isc_stdtime_t now = isc_stdtime_now();
	dns_kasp_t *kasp;
	dns_keystorelist_t *keystores;
	const char *keydir;

	ISC_LIST_INIT(keylist);

	kasp = dns_zone_getkasp(zone);
	keydir = dns_zone_getkeydirectory(zone);
	keystores = dns_zone_getkeystores(zone);

	dns_zone_lock_keyfiles(zone);
	result = dns_dnssec_findmatchingkeys(dns_zone_getorigin(zone), kasp,
					     keydir, keystores, now, mctx,
					     &keylist);
	dns_zone_unlock_keyfiles(zone);

	if (result != ISC_R_SUCCESS) {
		*nkeys = 0;
		return result;
	}

	/* Add new 'dnskeys' to 'keys' */
	ISC_LIST_FOREACH (keylist, k, link) {
		if (count >= maxkeys) {
			ISC_LIST_UNLINK(keylist, k, link);
			dns_dnsseckey_destroy(mctx, &k);
			result = ISC_R_NOSPACE;
			break;
		}

		/* Detect inactive keys */
		if (!dns_dnssec_keyactive(k->key, now)) {
			dst_key_setinactive(k->key, true);
		}

		keys[count] = k->key;
		k->key = NULL;
		count++;

		ISC_LIST_UNLINK(keylist, k, link);
		dns_dnsseckey_destroy(mctx, &k);
	}

	*nkeys = count;
	return result;
}

/*%
 * Add RRSIG records for an RRset, recording the change in "diff".
 */
static isc_result_t
add_sigs(dns_update_log_t *log, dns_zone_t *zone, dns_db_t *db,
	 dns_dbversion_t *ver, dns_name_t *name, dns_rdatatype_t type,
	 dns_diff_t *diff, dst_key_t **keys, unsigned int nkeys,
	 isc_stdtime_t now, isc_stdtime_t inception, isc_stdtime_t expire) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	dns_kasp_t *kasp = dns_zone_getkasp(zone);
	dns_rdataset_t rdataset;
	dns_rdata_t sig_rdata = DNS_RDATA_INIT;
	dns_stats_t *dnssecsignstats = dns_zone_getdnssecsignstats(zone);
	isc_buffer_t buffer;
	unsigned char data[1024]; /* XXX */
	unsigned int i;
	bool added_sig = false;
	bool use_kasp = false;
	bool offlineksk = false;
	isc_mem_t *mctx = diff->mctx;

	if (kasp != NULL) {
		use_kasp = true;
		offlineksk = dns_kasp_offlineksk(kasp);
	}

	dns_rdataset_init(&rdataset);
	isc_buffer_init(&buffer, data, sizeof(data));

	/* Get the rdataset to sign. */
	if (type == dns_rdatatype_nsec3) {
		CHECK(dns_db_findnsec3node(db, name, false, &node));
	} else {
		CHECK(dns_db_findnode(db, name, false, &node));
	}
	CHECK(dns_db_findrdataset(db, node, ver, type, 0, (isc_stdtime_t)0,
				  &rdataset, NULL));
	dns_db_detachnode(db, &node);

#define REVOKE(x) ((dst_key_flags(x) & DNS_KEYFLAG_REVOKE) != 0)
#define KSK(x)	  ((dst_key_flags(x) & DNS_KEYFLAG_KSK) != 0)
#define ID(x)	  dst_key_id(x)
#define ALG(x)	  dst_key_alg(x)

	/*
	 * If we are honoring KSK flags then we need to check that we
	 * have both KSK and non-KSK keys that are not revoked per
	 * algorithm.
	 */
	for (i = 0; i < nkeys; i++) {
		bool both = false;

		/* Don't add signatures for offline or inactive keys */
		if (!dst_key_isprivate(keys[i]) && !offlineksk) {
			continue;
		}
		if (dst_key_inactive(keys[i]) && !offlineksk) {
			continue;
		}

		if (use_kasp) {
			/*
			 * A dnssec-policy is found. Check what RRsets this
			 * key should sign.
			 */
			isc_stdtime_t when;
			isc_result_t kresult;
			bool ksk = false;
			bool zsk = false;

			kresult = dst_key_getbool(keys[i], DST_BOOL_KSK, &ksk);
			if (kresult != ISC_R_SUCCESS) {
				if (KSK(keys[i])) {
					ksk = true;
				}
			}
			kresult = dst_key_getbool(keys[i], DST_BOOL_ZSK, &zsk);
			if (kresult != ISC_R_SUCCESS) {
				if (!KSK(keys[i])) {
					zsk = true;
				}
			}

			if (!dst_key_isprivate(keys[i]) && offlineksk && zsk) {
				continue;
			}
			if (dst_key_inactive(keys[i]) && offlineksk && zsk) {
				continue;
			}

			if (dns_rdatatype_iskeymaterial(type)) {
				/*
				 * DNSKEY RRset is signed with KSK.
				 * CDS and CDNSKEY RRsets too (RFC 7344, 4.1).
				 */
				if (!ksk) {
					continue;
				}
			} else if (!zsk) {
				/*
				 * Other RRsets are signed with ZSK.
				 */
				continue;
			} else if (zsk &&
				   !dst_key_is_signing(keys[i], DST_BOOL_ZSK,
						       now, &when))
			{
				/*
				 * This key is not active for zone-signing.
				 */
				continue;
			}
		} else if (!REVOKE(keys[i])) {
			/*
			 * Don't consider inactive keys, however the KSK may be
			 * temporary offline, so do consider KSKs which private
			 * key files are unavailable.
			 */
			both = dst_key_have_ksk_and_zsk(
				keys, nkeys, i, false, KSK(keys[i]),
				!KSK(keys[i]), NULL, NULL);
			if (both) {
				/*
				 * CDS and CDNSKEY are signed with KSK (RFC
				 * 7344, 4.1).
				 */
				if (dns_rdatatype_iskeymaterial(type)) {
					if (!KSK(keys[i])) {
						continue;
					}
				} else if (KSK(keys[i])) {
					continue;
				}
			}
		}

		/*
		 * If this key is revoked, it may only sign the DNSKEY RRset.
		 */
		if (REVOKE(keys[i]) && type != dns_rdatatype_dnskey) {
			continue;
		}

		/* Calculate the signature, creating a RRSIG RDATA. */
		if (offlineksk && dns_rdatatype_iskeymaterial(type)) {
			/* Look up the signature in the SKR bundle */
			dns_skrbundle_t *bundle = dns_zone_getskrbundle(zone);
			if (bundle == NULL) {
				CHECK(DNS_R_NOSKRBUNDLE);
			}
			CHECK(dns_skrbundle_getsig(bundle, keys[i], type,
						   &sig_rdata));
		} else {
			CHECK(dns_dnssec_sign(name, &rdataset, keys[i],
					      &inception, &expire, mctx,
					      &buffer, &sig_rdata));
		}

		/* Update the database and journal with the RRSIG. */
		/* XXX inefficient - will cause dataset merging */
		CHECK(update_one_rr(db, ver, diff, DNS_DIFFOP_ADDRESIGN, name,
				    rdataset.ttl, &sig_rdata));
		dns_rdata_reset(&sig_rdata);
		isc_buffer_init(&buffer, data, sizeof(data));
		added_sig = true;
		/* Update DNSSEC sign statistics. */
		if (dnssecsignstats != NULL) {
			dns_dnssecsignstats_increment(dnssecsignstats,
						      ID(keys[i]),
						      (uint8_t)ALG(keys[i]),
						      dns_dnssecsignstats_sign);
		}
	}
	if (!added_sig) {
		update_log(log, zone, ISC_LOG_ERROR,
			   "found no active private keys, "
			   "unable to generate any signatures");
		result = ISC_R_NOTFOUND;
	}

failure:
	if (dns_rdataset_isassociated(&rdataset)) {
		dns_rdataset_disassociate(&rdataset);
	}
	if (node != NULL) {
		dns_db_detachnode(db, &node);
	}
	return result;
}

/*
 * Delete expired RRsigs and any RRsigs we are about to re-sign.
 * See also zone.c:del_sigs().
 */
static isc_result_t
del_keysigs(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *name,
	    dns_diff_t *diff, dst_key_t **keys, unsigned int nkeys) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	dns_rdataset_t rdataset;
	unsigned int i;
	dns_rdata_rrsig_t rrsig;
	bool found;

	dns_rdataset_init(&rdataset);

	result = dns_db_findnode(db, name, false, &node);
	if (result == ISC_R_NOTFOUND) {
		return ISC_R_SUCCESS;
	}
	if (result != ISC_R_SUCCESS) {
		goto failure;
	}
	result = dns_db_findrdataset(db, node, ver, dns_rdatatype_rrsig,
				     dns_rdatatype_dnskey, (isc_stdtime_t)0,
				     &rdataset, NULL);
	dns_db_detachnode(db, &node);

	if (result == ISC_R_NOTFOUND) {
		return ISC_R_SUCCESS;
	}
	if (result != ISC_R_SUCCESS) {
		goto failure;
	}

	DNS_RDATASET_FOREACH (&rdataset) {
		dns_rdata_t rdata = DNS_RDATA_INIT;
		dns_rdataset_current(&rdataset, &rdata);
		result = dns_rdata_tostruct(&rdata, &rrsig, NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);
		found = false;
		for (i = 0; i < nkeys; i++) {
			if (rrsig.keyid == dst_key_id(keys[i])) {
				found = true;
				if (!dst_key_isprivate(keys[i]) &&
				    !dst_key_inactive(keys[i]))
				{
					/*
					 * The re-signing code in zone.c
					 * will mark this as offline.
					 * Just skip the record for now.
					 */
					break;
				}
				result = update_one_rr(db, ver, diff,
						       DNS_DIFFOP_DEL, name,
						       rdataset.ttl, &rdata);
				break;
			}
		}
		/*
		 * If there is not a matching DNSKEY then delete the RRSIG.
		 */
		if (!found) {
			result = update_one_rr(db, ver, diff, DNS_DIFFOP_DEL,
					       name, rdataset.ttl, &rdata);
		}
		dns_rdata_reset(&rdata);
		if (result != ISC_R_SUCCESS) {
			break;
		}
	}
	dns_rdataset_disassociate(&rdataset);

failure:
	if (node != NULL) {
		dns_db_detachnode(db, &node);
	}
	return result;
}

static isc_result_t
add_exposed_sigs(dns_update_log_t *log, dns_zone_t *zone, dns_db_t *db,
		 dns_dbversion_t *ver, dns_name_t *name, bool cut,
		 dns_diff_t *diff, dst_key_t **keys, unsigned int nkeys,
		 isc_stdtime_t now, isc_stdtime_t inception,
		 isc_stdtime_t expire, unsigned int *sigs) {
	isc_result_t result;
	dns_dbnode_t *node;
	dns_rdatasetiter_t *iter;

	node = NULL;
	result = dns_db_findnode(db, name, false, &node);
	if (result == ISC_R_NOTFOUND) {
		return ISC_R_SUCCESS;
	}
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	iter = NULL;
	result = dns_db_allrdatasets(db, node, ver, 0, (isc_stdtime_t)0, &iter);
	if (result != ISC_R_SUCCESS) {
		goto cleanup_node;
	}

	DNS_RDATASETITER_FOREACH (iter) {
		dns_rdataset_t rdataset = DNS_RDATASET_INIT;
		dns_rdatatype_t type;
		bool flag;

		dns_rdatasetiter_current(iter, &rdataset);
		type = rdataset.type;
		dns_rdataset_disassociate(&rdataset);

		/*
		 * We don't need to sign unsigned NSEC records at the cut
		 * as they are handled elsewhere.
		 */
		if ((type == dns_rdatatype_rrsig) ||
		    (cut && type != dns_rdatatype_ds))
		{
			continue;
		}
		result = rrset_exists(db, ver, name, dns_rdatatype_rrsig, type,
				      &flag);
		if (result != ISC_R_SUCCESS) {
			break;
		}
		if (flag) {
			continue;
		}
		result = add_sigs(log, zone, db, ver, name, type, diff, keys,
				  nkeys, now, inception, expire);
		if (result != ISC_R_SUCCESS) {
			break;
		}
		(*sigs)++;
	}
	dns_rdatasetiter_destroy(&iter);

cleanup_node:
	dns_db_detachnode(db, &node);

	return result;
}

/*%
 * Update RRSIG, NSEC and NSEC3 records affected by an update.  The original
 * update, including the SOA serial update but excluding the RRSIG & NSEC
 * changes, is in "diff" and has already been applied to "newver" of "db".
 * The database version prior to the update is "oldver".
 *
 * The necessary RRSIG, NSEC and NSEC3 changes will be applied to "newver"
 * and added (as a minimal diff) to "diff".
 *
 * The RRSIGs generated will be valid for 'sigvalidityinterval' seconds.
 */
isc_result_t
dns_update_signatures(dns_update_log_t *log, dns_zone_t *zone, dns_db_t *db,
		      dns_dbversion_t *oldver, dns_dbversion_t *newver,
		      dns_diff_t *diff, uint32_t sigvalidityinterval) {
	return dns_update_signaturesinc(log, zone, db, oldver, newver, diff,
					sigvalidityinterval, NULL);
}

struct dns_update_state {
	unsigned int magic;
	dns_diff_t diffnames;
	dns_diff_t affected;
	dns_diff_t sig_diff;
	dns_diff_t nsec_diff;
	dns_diff_t nsec_mindiff;
	dns_diff_t work;
	dst_key_t *zone_keys[DNS_MAXZONEKEYS];
	unsigned int nkeys;
	isc_stdtime_t now, inception, expire, soaexpire, keyexpire;
	dns_ttl_t nsecttl;
	bool build_nsec3;
	enum {
		sign_updates,
		remove_orphaned,
		build_chain,
		process_nsec,
		sign_nsec,
		update_nsec3,
		process_nsec3,
		sign_nsec3
	} state;
};

static uint32_t
dns__jitter_expire(dns_zone_t *zone) {
	/* Spread out signatures over time */
	isc_stdtime_t jitter = DEFAULT_JITTER;
	isc_stdtime_t sigvalidity = dns_zone_getsigvalidityinterval(zone);
	dns_kasp_t *kasp = dns_zone_getkasp(zone);

	if (kasp != NULL) {
		jitter = dns_kasp_sigjitter(kasp);
		sigvalidity = dns_kasp_sigvalidity(kasp);
		INSIST(jitter <= sigvalidity);
	}

	if (jitter > sigvalidity) {
		jitter = sigvalidity;
	}

	if (sigvalidity >= 3600U) {
		if (sigvalidity > 7200U) {
			sigvalidity -= isc_random_uniform(jitter);
		} else {
			sigvalidity -= isc_random_uniform(1200);
		}
	}
	return sigvalidity;
}

isc_result_t
dns_update_signaturesinc(dns_update_log_t *log, dns_zone_t *zone, dns_db_t *db,
			 dns_dbversion_t *oldver, dns_dbversion_t *newver,
			 dns_diff_t *diff, uint32_t sigvalidityinterval,
			 dns_update_state_t **statep) {
	isc_result_t result = ISC_R_SUCCESS;
	dns_update_state_t mystate, *state = NULL;
	dns_difftuple_t *tuple = NULL;
	bool flag, build_nsec;
	unsigned int i;
	dns_rdata_soa_t soa;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdataset_t rdataset;
	dns_dbnode_t *node = NULL;
	bool unsecure;
	bool cut;
	dns_rdatatype_t privatetype = dns_zone_getprivatetype(zone);
	unsigned int sigs = 0;
	unsigned int maxsigs = dns_zone_getsignatures(zone);

	if (statep == NULL || *statep == NULL) {
		if (statep == NULL) {
			state = &mystate;
		} else {
			state = isc_mem_get(diff->mctx, sizeof(*state));
		}

		dns_diff_init(diff->mctx, &state->diffnames);
		dns_diff_init(diff->mctx, &state->affected);
		dns_diff_init(diff->mctx, &state->sig_diff);
		dns_diff_init(diff->mctx, &state->nsec_diff);
		dns_diff_init(diff->mctx, &state->nsec_mindiff);
		dns_diff_init(diff->mctx, &state->work);
		state->nkeys = 0;
		state->build_nsec3 = false;

		result = find_zone_keys(zone, diff->mctx, DNS_MAXZONEKEYS,
					state->zone_keys, &state->nkeys);
		if (result == ISC_R_NOSPACE) {
			update_log(log, zone, ISC_LOG_ERROR,
				   "too many zone keys for secure "
				   "dynamic update");
		} else if (result != ISC_R_SUCCESS) {
			update_log(log, zone, ISC_LOG_ERROR,
				   "could not get zone keys for secure "
				   "dynamic update");
			goto failure;
		}

		state->now = isc_stdtime_now();
		state->inception = state->now - 3600; /* Allow for some clock
							 skew. */
		state->expire = state->now + dns__jitter_expire(zone);
		state->soaexpire = state->now + sigvalidityinterval;
		state->keyexpire = dns_zone_getkeyvalidityinterval(zone);
		if (state->keyexpire == 0) {
			state->keyexpire = state->expire;
		} else {
			state->keyexpire += state->now;
		}

		/*
		 * Calculate the NSEC/NSEC3 TTL as a minimum of the SOA TTL and
		 * MINIMUM field.
		 */
		CHECK(dns_db_findnode(db, dns_db_origin(db), false, &node));
		dns_rdataset_init(&rdataset);
		CHECK(dns_db_findrdataset(db, node, newver, dns_rdatatype_soa,
					  0, (isc_stdtime_t)0, &rdataset,
					  NULL));
		CHECK(dns_rdataset_first(&rdataset));
		dns_rdataset_current(&rdataset, &rdata);
		CHECK(dns_rdata_tostruct(&rdata, &soa, NULL));
		state->nsecttl = ISC_MIN(rdataset.ttl, soa.minimum);
		dns_rdataset_disassociate(&rdataset);
		dns_db_detachnode(db, &node);

		/*
		 * Find all RRsets directly affected by the update, and
		 * update their RRSIGs.  Also build a list of names affected
		 * by the update in "diffnames".
		 */
		CHECK(dns_diff_sort(diff, temp_order));
		state->state = sign_updates;
		state->magic = STATE_MAGIC;
		if (statep != NULL) {
			*statep = state;
		}
	} else {
		REQUIRE(DNS_STATE_VALID(*statep));
		state = *statep;
	}

next_state:
	switch (state->state) {
	case sign_updates:
		tuple = ISC_LIST_HEAD(diff->tuples);
		while (tuple != NULL) {
			dns_name_t *name = &tuple->name;
			dns_difftuple_t *next = NULL;

			/*
			 * Now "name" is a new, unique name affected by the
			 * update.
			 */
			namelist_append_name(&state->diffnames, name);

			while (tuple != NULL &&
			       dns_name_equal(&tuple->name, name))
			{
				dns_rdatatype_t type;
				type = tuple->rdata.type;

				/*
				 * Now "name" and "type" denote a new unique
				 * RRset affected by the update.
				 */

				/* Don't sign RRSIGs. */
				if (type == dns_rdatatype_rrsig) {
					goto skip;
				}

				/*
				 * Delete all old RRSIGs covering this type,
				 * since they are all invalid when the signed
				 * RRset has changed.  We may not be able to
				 * recreate all of them - tough.
				 * Special case changes to the zone's DNSKEY
				 * records to support offline KSKs.
				 */
				if (type == dns_rdatatype_dnskey) {
					del_keysigs(db, newver, name,
						    &state->sig_diff,
						    state->zone_keys,
						    state->nkeys);
				} else {
					CHECK(delete_if(
						true_p, db, newver, name,
						dns_rdatatype_rrsig, type, NULL,
						&state->sig_diff));
				}

				/*
				 * If this RRset is still visible after the
				 * update, add a new signature for it.
				 */
				CHECK(rrset_visible(db, newver, name, type,
						    &flag));
				if (flag) {
					isc_stdtime_t exp;
					if (dns_rdatatype_iskeymaterial(type)) {
						exp = state->keyexpire;
					} else if (type == dns_rdatatype_soa) {
						exp = state->soaexpire;
					} else {
						exp = state->expire;
					}

					CHECK(add_sigs(log, zone, db, newver,
						       name, type,
						       &state->sig_diff,
						       state->zone_keys,
						       state->nkeys, state->now,
						       state->inception, exp));
					sigs++;
				}
			skip:
				/* Skip any other updates to the same RRset. */
				while (tuple != NULL &&
				       dns_name_equal(&tuple->name, name) &&
				       tuple->rdata.type == type)
				{
					next = ISC_LIST_NEXT(tuple, link);
					ISC_LIST_UNLINK(diff->tuples, tuple,
							link);
					ISC_LIST_APPEND(state->work.tuples,
							tuple, link);
					tuple = next;
				}
			}
			if (state != &mystate && sigs > maxsigs) {
				return DNS_R_CONTINUE;
			}
		}
		ISC_LIST_APPENDLIST(diff->tuples, state->work.tuples, link);

		update_log(log, zone, ISC_LOG_DEBUG(3),
			   "updated data signatures");
		FALLTHROUGH;
	case remove_orphaned:
		state->state = remove_orphaned;

		/* Remove orphaned NSECs and RRSIG NSECs. */
		ISC_LIST_FOREACH (state->diffnames.tuples, t, link) {
			CHECK(non_nsec_rrset_exists(db, newver, &t->name,
						    &flag));
			if (!flag) {
				CHECK(delete_if(true_p, db, newver, &t->name,
						dns_rdatatype_any, 0, NULL,
						&state->sig_diff));
			}
		}
		update_log(log, zone, ISC_LOG_DEBUG(3),
			   "removed any orphaned NSEC records");

		/*
		 * See if we need to build NSEC or NSEC3 chains.
		 */
		CHECK(dns_private_chains(db, newver, privatetype, &build_nsec,
					 &state->build_nsec3));
		if (!build_nsec) {
			state->state = update_nsec3;
			goto next_state;
		}

		update_log(log, zone, ISC_LOG_DEBUG(3),
			   "rebuilding NSEC chain");

		FALLTHROUGH;
	case build_chain:
		state->state = build_chain;
		/*
		 * When a name is created or deleted, its predecessor needs to
		 * have its NSEC updated.
		 */
		ISC_LIST_FOREACH (state->diffnames.tuples, t, link) {
			bool existed, exists;
			dns_fixedname_t fixedname;
			dns_name_t *prevname;

			prevname = dns_fixedname_initname(&fixedname);

			if (oldver != NULL) {
				CHECK(name_exists(db, oldver, &t->name,
						  &existed));
			} else {
				existed = false;
			}
			CHECK(name_exists(db, newver, &t->name, &exists));
			if (exists == existed) {
				continue;
			}

			/*
			 * Find the predecessor.
			 * When names become obscured or unobscured in this
			 * update transaction, we may find the wrong
			 * predecessor because the NSECs have not yet been
			 * updated to reflect the delegation change.  This
			 * should not matter because in this case, the correct
			 * predecessor is either the delegation node or a
			 * newly unobscured node, and those nodes are on the
			 * "affected" list in any case.
			 */
			CHECK(next_active(log, zone, db, newver, &t->name,
					  prevname, false));
			namelist_append_name(&state->affected, prevname);
		}

		/*
		 * Find names potentially affected by delegation changes
		 * (obscured by adding an NS or DNAME, or unobscured by
		 * removing one).
		 */
		ISC_LIST_FOREACH (state->diffnames.tuples, t, link) {
			bool ns_existed, dname_existed;
			bool ns_exists, dname_exists;

			if (oldver != NULL) {
				CHECK(rrset_exists(db, oldver, &t->name,
						   dns_rdatatype_ns, 0,
						   &ns_existed));
			} else {
				ns_existed = false;
			}
			if (oldver != NULL) {
				CHECK(rrset_exists(db, oldver, &t->name,
						   dns_rdatatype_dname, 0,
						   &dname_existed));
			} else {
				dname_existed = false;
			}
			CHECK(rrset_exists(db, newver, &t->name,
					   dns_rdatatype_ns, 0, &ns_exists));
			CHECK(rrset_exists(db, newver, &t->name,
					   dns_rdatatype_dname, 0,
					   &dname_exists));
			if ((ns_exists || dname_exists) ==
			    (ns_existed || dname_existed))
			{
				continue;
			}
			/*
			 * There was a delegation change.  Mark all subdomains
			 * of t->name as potentially needing a NSEC update.
			 */
			CHECK(namelist_append_subdomain(db, &t->name,
							&state->affected));
		}
		ISC_LIST_APPENDLIST(state->affected.tuples,
				    state->diffnames.tuples, link);
		INSIST(ISC_LIST_EMPTY(state->diffnames.tuples));

		CHECK(uniqify_name_list(&state->affected));

		FALLTHROUGH;
	case process_nsec:
		state->state = process_nsec;

		/*
		 * Determine which names should have NSECs, and delete/create
		 * NSECs to make it so.  We don't know the final NSEC targets
		 * yet, so we just create placeholder NSECs with arbitrary
		 * contents to indicate that their respective owner names
		 * should be part of the NSEC chain.
		 */
		ISC_LIST_FOREACH (state->affected.tuples, t, link) {
			bool exists;
			dns_name_t *name = &t->name;

			CHECK(name_exists(db, newver, name, &exists));
			if (!exists) {
				goto unlink;
			}
			CHECK(is_active(db, newver, name, &flag, &cut, NULL));
			if (!flag) {
				/*
				 * This name is obscured.  Delete any
				 * existing NSEC record.
				 */
				CHECK(delete_if(true_p, db, newver, name,
						dns_rdatatype_nsec, 0, NULL,
						&state->nsec_diff));
				CHECK(delete_if(rrsig_p, db, newver, name,
						dns_rdatatype_any, 0, NULL,
						diff));
			} else {
				/*
				 * This name is not obscured.  It needs to have
				 * a NSEC unless it is the at the origin, in
				 * which case it should already exist if there
				 * is a complete NSEC chain and if there isn't
				 * a complete NSEC chain we don't want to add
				 * one as that would signal that there is a
				 * complete NSEC chain.
				 */
				if (!dns_name_equal(name, dns_db_origin(db))) {
					CHECK(rrset_exists(db, newver, name,
							   dns_rdatatype_nsec,
							   0, &flag));
					if (!flag) {
						CHECK(add_placeholder_nsec(
							db, newver, name,
							diff));
					}
				}
				CHECK(add_exposed_sigs(
					log, zone, db, newver, name, cut,
					&state->sig_diff, state->zone_keys,
					state->nkeys, state->now,
					state->inception, state->expire,
					&sigs));
			}
		unlink:
			ISC_LIST_UNLINK(state->affected.tuples, t, link);
			ISC_LIST_APPEND(state->work.tuples, t, link);
			if (state != &mystate && sigs > maxsigs) {
				return DNS_R_CONTINUE;
			}
		}
		ISC_LIST_APPENDLIST(state->affected.tuples, state->work.tuples,
				    link);

		/*
		 * Now we know which names are part of the NSEC chain.
		 * Make them all point at their correct targets.
		 */
		ISC_LIST_FOREACH (state->affected.tuples, t, link) {
			CHECK(rrset_exists(db, newver, &t->name,
					   dns_rdatatype_nsec, 0, &flag));
			if (flag) {
				/*
				 * There is a NSEC, but we don't know if it
				 * is correct. Delete it and create a correct
				 * one to be sure. If the update was
				 * unnecessary, the diff minimization
				 * will take care of eliminating it from the
				 * journal, IXFRs, etc.
				 *
				 * The RRSIG bit should always be set in the
				 * NSECs we generate, because they will all
				 * get RRSIG NSECs.
				 * (XXX what if the zone keys are missing?).
				 * Because the RRSIG NSECs have not necessarily
				 * been created yet, the correctness of the
				 * bit mask relies on the assumption that NSECs
				 * are only created if there is other data, and
				 * if there is other data, there are other
				 * RRSIGs.
				 */
				CHECK(add_nsec(log, zone, db, newver, &t->name,
					       state->nsecttl,
					       &state->nsec_diff));
			}
		}

		/*
		 * Minimize the set of NSEC updates so that we don't
		 * have to regenerate the RRSIG NSECs for NSECs that were
		 * replaced with identical ones.
		 */
		ISC_LIST_FOREACH (state->nsec_diff.tuples, t, link) {
			ISC_LIST_UNLINK(state->nsec_diff.tuples, t, link);
			dns_diff_appendminimal(&state->nsec_mindiff, &t);
		}

		update_log(log, zone, ISC_LOG_DEBUG(3),
			   "signing rebuilt NSEC chain");

		FALLTHROUGH;
	case sign_nsec:
		state->state = sign_nsec;
		/* Update RRSIG NSECs. */
		ISC_LIST_FOREACH (state->nsec_mindiff.tuples, t, link) {
			if (t->op == DNS_DIFFOP_DEL) {
				CHECK(delete_if(true_p, db, newver, &t->name,
						dns_rdatatype_rrsig,
						dns_rdatatype_nsec, NULL,
						&state->sig_diff));
			} else if (t->op == DNS_DIFFOP_ADD) {
				CHECK(add_sigs(log, zone, db, newver, &t->name,
					       dns_rdatatype_nsec,
					       &state->sig_diff,
					       state->zone_keys, state->nkeys,
					       state->now, state->inception,
					       state->expire));
				sigs++;
			} else {
				UNREACHABLE();
			}
			ISC_LIST_UNLINK(state->nsec_mindiff.tuples, t, link);
			ISC_LIST_APPEND(state->work.tuples, t, link);
			if (state != &mystate && sigs > maxsigs) {
				return DNS_R_CONTINUE;
			}
		}
		ISC_LIST_APPENDLIST(state->nsec_mindiff.tuples,
				    state->work.tuples, link);
		FALLTHROUGH;
	case update_nsec3:
		state->state = update_nsec3;

		/* Record our changes for the journal. */
		ISC_LIST_FOREACH (state->sig_diff.tuples, t, link) {
			ISC_LIST_UNLINK(state->sig_diff.tuples, t, link);
			dns_diff_appendminimal(diff, &t);
		}
		ISC_LIST_FOREACH (state->nsec_mindiff.tuples, t, link) {
			ISC_LIST_UNLINK(state->nsec_mindiff.tuples, t, link);
			dns_diff_appendminimal(diff, &t);
		}

		INSIST(ISC_LIST_EMPTY(state->sig_diff.tuples));
		INSIST(ISC_LIST_EMPTY(state->nsec_diff.tuples));
		INSIST(ISC_LIST_EMPTY(state->nsec_mindiff.tuples));

		if (!state->build_nsec3) {
			update_log(log, zone, ISC_LOG_DEBUG(3),
				   "no NSEC3 chains to rebuild");
			goto failure;
		}

		update_log(log, zone, ISC_LOG_DEBUG(3),
			   "rebuilding NSEC3 chains");

		dns_diff_clear(&state->diffnames);
		dns_diff_clear(&state->affected);

		CHECK(dns_diff_sort(diff, temp_order));

		/*
		 * Find names potentially affected by delegation changes
		 * (obscured by adding an NS or DNAME, or unobscured by
		 * removing one).
		 */
		tuple = ISC_LIST_HEAD(diff->tuples);
		while (tuple != NULL) {
			dns_name_t *name = &tuple->name;

			bool ns_existed, dname_existed;
			bool ns_exists, dname_exists;
			bool exists, existed;

			if (tuple->rdata.type == dns_rdatatype_nsec ||
			    tuple->rdata.type == dns_rdatatype_rrsig)
			{
				tuple = ISC_LIST_NEXT(tuple, link);
				continue;
			}

			namelist_append_name(&state->affected, name);

			if (oldver != NULL) {
				CHECK(rrset_exists(db, oldver, name,
						   dns_rdatatype_ns, 0,
						   &ns_existed));
			} else {
				ns_existed = false;
			}
			if (oldver != NULL) {
				CHECK(rrset_exists(db, oldver, name,
						   dns_rdatatype_dname, 0,
						   &dname_existed));
			} else {
				dname_existed = false;
			}
			CHECK(rrset_exists(db, newver, name, dns_rdatatype_ns,
					   0, &ns_exists));
			CHECK(rrset_exists(db, newver, name,
					   dns_rdatatype_dname, 0,
					   &dname_exists));

			exists = ns_exists || dname_exists;
			existed = ns_existed || dname_existed;
			if (exists == existed) {
				goto nextname;
			}
			/*
			 * There was a delegation change.  Mark all subdomains
			 * of tuple->name as potentially needing a NSEC3 update.
			 */
			CHECK(namelist_append_subdomain(db, name,
							&state->affected));

		nextname:
			while (tuple != NULL &&
			       dns_name_equal(&tuple->name, name))
			{
				tuple = ISC_LIST_NEXT(tuple, link);
			}
		}

		FALLTHROUGH;
	case process_nsec3:
		state->state = process_nsec3;
		ISC_LIST_FOREACH (state->affected.tuples, t, link) {
			dns_name_t *name = &t->name;

			unsecure = false; /* Silence compiler warning. */
			CHECK(is_active(db, newver, name, &flag, &cut,
					&unsecure));

			if (!flag) {
				CHECK(delete_if(rrsig_p, db, newver, name,
						dns_rdatatype_any, 0, NULL,
						diff));
				CHECK(dns_nsec3_delnsec3sx(db, newver, name,
							   privatetype,
							   &state->nsec_diff));
			} else {
				CHECK(add_exposed_sigs(
					log, zone, db, newver, name, cut,
					&state->sig_diff, state->zone_keys,
					state->nkeys, state->now,
					state->inception, state->expire,
					&sigs));
				CHECK(dns_nsec3_addnsec3sx(
					db, newver, name, state->nsecttl,
					unsecure, privatetype,
					&state->nsec_diff));
			}
			ISC_LIST_UNLINK(state->affected.tuples, t, link);
			ISC_LIST_APPEND(state->work.tuples, t, link);
			if (state != &mystate && sigs > maxsigs) {
				return DNS_R_CONTINUE;
			}
		}
		ISC_LIST_APPENDLIST(state->affected.tuples, state->work.tuples,
				    link);

		/*
		 * Minimize the set of NSEC3 updates so that we don't
		 * have to regenerate the RRSIG NSEC3s for NSEC3s that were
		 * replaced with identical ones.
		 */
		ISC_LIST_FOREACH (state->nsec_diff.tuples, t, link) {
			ISC_LIST_UNLINK(state->nsec_diff.tuples, t, link);
			dns_diff_appendminimal(&state->nsec_mindiff, &t);
		}

		update_log(log, zone, ISC_LOG_DEBUG(3),
			   "signing rebuilt NSEC3 chain");

		FALLTHROUGH;
	case sign_nsec3:
		state->state = sign_nsec3;
		/* Update RRSIG NSEC3s. */
		ISC_LIST_FOREACH (state->nsec_mindiff.tuples, t, link) {
			if (t->op == DNS_DIFFOP_DEL) {
				CHECK(delete_if(true_p, db, newver, &t->name,
						dns_rdatatype_rrsig,
						dns_rdatatype_nsec3, NULL,
						&state->sig_diff));
			} else if (t->op == DNS_DIFFOP_ADD) {
				CHECK(add_sigs(log, zone, db, newver, &t->name,
					       dns_rdatatype_nsec3,
					       &state->sig_diff,
					       state->zone_keys, state->nkeys,
					       state->now, state->inception,
					       state->expire));
				sigs++;
			} else {
				UNREACHABLE();
			}
			ISC_LIST_UNLINK(state->nsec_mindiff.tuples, t, link);
			ISC_LIST_APPEND(state->work.tuples, t, link);
			if (state != &mystate && sigs > maxsigs) {
				return DNS_R_CONTINUE;
			}
		}
		ISC_LIST_APPENDLIST(state->nsec_mindiff.tuples,
				    state->work.tuples, link);

		/* Record our changes for the journal. */
		ISC_LIST_FOREACH (state->sig_diff.tuples, t, link) {
			ISC_LIST_UNLINK(state->sig_diff.tuples, t, link);
			dns_diff_appendminimal(diff, &t);
		}
		ISC_LIST_FOREACH (state->nsec_mindiff.tuples, t, link) {
			ISC_LIST_UNLINK(state->nsec_mindiff.tuples, t, link);
			dns_diff_appendminimal(diff, &t);
		}

		INSIST(ISC_LIST_EMPTY(state->sig_diff.tuples));
		INSIST(ISC_LIST_EMPTY(state->nsec_diff.tuples));
		INSIST(ISC_LIST_EMPTY(state->nsec_mindiff.tuples));
		break;
	default:
		UNREACHABLE();
	}

failure:
	if (node != NULL) {
		dns_db_detachnode(db, &node);
	}

	dns_diff_clear(&state->sig_diff);
	dns_diff_clear(&state->nsec_diff);
	dns_diff_clear(&state->nsec_mindiff);

	dns_diff_clear(&state->affected);
	dns_diff_clear(&state->diffnames);
	dns_diff_clear(&state->work);

	for (i = 0; i < state->nkeys; i++) {
		dst_key_free(&state->zone_keys[i]);
	}

	if (state != &mystate) {
		*statep = NULL;
		state->magic = 0;
		isc_mem_put(diff->mctx, state, sizeof(*state));
	}

	return result;
}

static isc_stdtime_t
epoch_to_yyyymmdd(time_t when) {
	struct tm t, *tm = localtime_r(&when, &t);
	if (tm == NULL) {
		return 0;
	}
	return ((tm->tm_year + 1900) * 10000) + ((tm->tm_mon + 1) * 100) +
	       tm->tm_mday;
}

static uint32_t
dns__update_soaserial(uint32_t serial, dns_updatemethod_t method) {
	isc_stdtime_t now;

	switch (method) {
	case dns_updatemethod_none:
		return serial;
	case dns_updatemethod_unixtime:
		now = isc_stdtime_now();
		return now;
	case dns_updatemethod_date:
		now = isc_stdtime_now();
		return epoch_to_yyyymmdd((time_t)now) * 100;
	case dns_updatemethod_increment:
		/* RFC1982 */
		serial = (serial + 1) & 0xFFFFFFFF;
		if (serial == 0) {
			return 1;
		}
		return serial;
	default:
		UNREACHABLE();
	}
}

uint32_t
dns_update_soaserial(uint32_t serial, dns_updatemethod_t method,
		     dns_updatemethod_t *used) {
	uint32_t new_serial = dns__update_soaserial(serial, method);
	switch (method) {
	case dns_updatemethod_none:
	case dns_updatemethod_increment:
		break;
	case dns_updatemethod_unixtime:
	case dns_updatemethod_date:
		if (!(new_serial != 0 && isc_serial_gt(new_serial, serial))) {
			/*
			 * If the new date serial following YYYYMMDD00 is equal
			 * to or smaller than the current serial, but YYYYMMDD99
			 * would be larger, pretend we have used the
			 * "dns_updatemethod_date" method.
			 */
			if (method == dns_updatemethod_unixtime ||
			    !isc_serial_gt(new_serial + 99, serial))
			{
				method = dns_updatemethod_increment;
			}
			new_serial = dns__update_soaserial(
				serial, dns_updatemethod_increment);
		}
		break;
	default:
		UNREACHABLE();
	}

	SET_IF_NOT_NULL(used, method);

	return new_serial;
}
