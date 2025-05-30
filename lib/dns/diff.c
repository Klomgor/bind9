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

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include <isc/buffer.h>
#include <isc/file.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/result.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/callbacks.h>
#include <dns/db.h>
#include <dns/diff.h>
#include <dns/rdataclass.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatastruct.h>
#include <dns/rdatatype.h>
#include <dns/time.h>

#define CHECK(op)                            \
	do {                                 \
		result = (op);               \
		if (result != ISC_R_SUCCESS) \
			goto failure;        \
	} while (0)

static dns_rdatatype_t
rdata_covers(dns_rdata_t *rdata) {
	return rdata->type == dns_rdatatype_rrsig ? dns_rdata_covers(rdata) : 0;
}

void
dns_difftuple_create(isc_mem_t *mctx, dns_diffop_t op, const dns_name_t *name,
		     dns_ttl_t ttl, dns_rdata_t *rdata, dns_difftuple_t **tp) {
	dns_difftuple_t *t;
	unsigned int size;
	unsigned char *datap;

	REQUIRE(tp != NULL && *tp == NULL);

	/*
	 * Create a new tuple.  The variable-size wire-format name data and
	 * rdata immediately follow the dns_difftuple_t structure
	 * in memory.
	 */
	size = sizeof(*t) + name->length + rdata->length;
	t = isc_mem_allocate(mctx, size);
	t->mctx = NULL;
	isc_mem_attach(mctx, &t->mctx);
	t->op = op;

	datap = (unsigned char *)(t + 1);

	memmove(datap, name->ndata, name->length);
	dns_name_init(&t->name);
	dns_name_clone(name, &t->name);
	t->name.ndata = datap;
	datap += name->length;

	t->ttl = ttl;

	dns_rdata_init(&t->rdata);
	dns_rdata_clone(rdata, &t->rdata);
	if (rdata->data != NULL) {
		memmove(datap, rdata->data, rdata->length);
		t->rdata.data = datap;
		datap += rdata->length;
	} else {
		t->rdata.data = NULL;
		INSIST(rdata->length == 0);
	}

	ISC_LINK_INIT(&t->rdata, link);
	ISC_LINK_INIT(t, link);
	t->magic = DNS_DIFFTUPLE_MAGIC;

	INSIST(datap == (unsigned char *)t + size);

	*tp = t;
}

void
dns_difftuple_free(dns_difftuple_t **tp) {
	dns_difftuple_t *t = *tp;
	*tp = NULL;
	isc_mem_t *mctx;

	REQUIRE(DNS_DIFFTUPLE_VALID(t));

	dns_name_invalidate(&t->name);
	t->magic = 0;
	mctx = t->mctx;
	isc_mem_free(mctx, t);
	isc_mem_detach(&mctx);
}

void
dns_difftuple_copy(dns_difftuple_t *orig, dns_difftuple_t **copyp) {
	dns_difftuple_create(orig->mctx, orig->op, &orig->name, orig->ttl,
			     &orig->rdata, copyp);
}

void
dns_diff_init(isc_mem_t *mctx, dns_diff_t *diff) {
	diff->mctx = mctx;
	ISC_LIST_INIT(diff->tuples);
	diff->magic = DNS_DIFF_MAGIC;
	diff->size = 0;
}

void
dns_diff_clear(dns_diff_t *diff) {
	REQUIRE(DNS_DIFF_VALID(diff));
	ISC_LIST_FOREACH (diff->tuples, t, link) {
		ISC_LIST_UNLINK(diff->tuples, t, link);
		dns_difftuple_free(&t);
	}
	diff->size = 0;
	ENSURE(ISC_LIST_EMPTY(diff->tuples));
}

void
dns_diff_append(dns_diff_t *diff, dns_difftuple_t **tuplep) {
	REQUIRE(DNS_DIFF_VALID(diff));
	ISC_LIST_APPEND(diff->tuples, *tuplep, link);
	diff->size += 1;
	*tuplep = NULL;
}

bool
dns_diff_is_boundary(const dns_diff_t *diff, dns_name_t *new_name) {
	REQUIRE(DNS_DIFF_VALID(diff));
	REQUIRE(DNS_NAME_VALID(new_name));

	if (ISC_LIST_EMPTY(diff->tuples)) {
		return false;
	}

	dns_difftuple_t *tail = ISC_LIST_TAIL(diff->tuples);
	return !dns_name_caseequal(&tail->name, new_name);
}

size_t
dns_diff_size(const dns_diff_t *diff) {
	REQUIRE(DNS_DIFF_VALID(diff));
	return diff->size;
}

/* XXX this is O(N) */

void
dns_diff_appendminimal(dns_diff_t *diff, dns_difftuple_t **tuplep) {
	REQUIRE(DNS_DIFF_VALID(diff));
	REQUIRE(DNS_DIFFTUPLE_VALID(*tuplep));

	/*
	 * Look for an existing tuple with the same owner name,
	 * rdata, and TTL.   If we are doing an addition and find a
	 * deletion or vice versa, remove both the old and the
	 * new tuple since they cancel each other out (assuming
	 * that we never delete nonexistent data or add existing
	 * data).
	 *
	 * If we find an old update of the same kind as
	 * the one we are doing, there must be a programming
	 * error.  We report it but try to continue anyway.
	 */
	ISC_LIST_FOREACH (diff->tuples, ot, link) {
		if (dns_name_caseequal(&ot->name, &(*tuplep)->name) &&
		    dns_rdata_compare(&ot->rdata, &(*tuplep)->rdata) == 0 &&
		    ot->ttl == (*tuplep)->ttl)
		{
			ISC_LIST_UNLINK(diff->tuples, ot, link);
			INSIST(diff->size > 0);
			diff->size -= 1;

			if ((*tuplep)->op == ot->op) {
				UNEXPECTED_ERROR("unexpected non-minimal diff");
			} else {
				dns_difftuple_free(tuplep);
			}
			dns_difftuple_free(&ot);
			break;
		}
	}

	if (*tuplep != NULL) {
		ISC_LIST_APPEND(diff->tuples, *tuplep, link);
		diff->size += 1;
		*tuplep = NULL;
	}
}

static isc_stdtime_t
setresign(dns_rdataset_t *modified) {
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_rrsig_t sig;
	int64_t when;
	isc_result_t result;

	result = dns_rdataset_first(modified);
	INSIST(result == ISC_R_SUCCESS);
	dns_rdataset_current(modified, &rdata);
	(void)dns_rdata_tostruct(&rdata, &sig, NULL);
	if ((rdata.flags & DNS_RDATA_OFFLINE) != 0) {
		when = 0;
	} else {
		when = dns_time64_from32(sig.timeexpire);
	}
	dns_rdata_reset(&rdata);

	result = dns_rdataset_next(modified);
	while (result == ISC_R_SUCCESS) {
		dns_rdataset_current(modified, &rdata);
		(void)dns_rdata_tostruct(&rdata, &sig, NULL);
		if ((rdata.flags & DNS_RDATA_OFFLINE) != 0) {
			goto next_rr;
		}
		if (when == 0 || dns_time64_from32(sig.timeexpire) < when) {
			when = dns_time64_from32(sig.timeexpire);
		}
	next_rr:
		dns_rdata_reset(&rdata);
		result = dns_rdataset_next(modified);
	}
	INSIST(result == ISC_R_NOMORE);
	return (isc_stdtime_t)when;
}

static void
getownercase(dns_rdataset_t *rdataset, dns_name_t *name) {
	if (dns_rdataset_isassociated(rdataset)) {
		dns_rdataset_getownercase(rdataset, name);
	}
}

static void
setownercase(dns_rdataset_t *rdataset, const dns_name_t *name) {
	if (dns_rdataset_isassociated(rdataset)) {
		dns_rdataset_setownercase(rdataset, name);
	}
}

static const char *
optotext(dns_diffop_t op) {
	switch (op) {
	case DNS_DIFFOP_ADD:
		return "add";
	case DNS_DIFFOP_ADDRESIGN:
		return "add-resign";
	case DNS_DIFFOP_DEL:
		return "del";
	case DNS_DIFFOP_DELRESIGN:
		return "del-resign";
	default:
		return "unknown";
	}
}

static isc_result_t
diff_apply(const dns_diff_t *diff, dns_db_t *db, dns_dbversion_t *ver,
	   bool warn) {
	dns_difftuple_t *t;
	dns_dbnode_t *node = NULL;
	isc_result_t result;
	char namebuf[DNS_NAME_FORMATSIZE];
	char typebuf[DNS_RDATATYPE_FORMATSIZE];
	char classbuf[DNS_RDATACLASS_FORMATSIZE];

	REQUIRE(DNS_DIFF_VALID(diff));
	REQUIRE(DNS_DB_VALID(db));

	t = ISC_LIST_HEAD(diff->tuples);
	while (t != NULL) {
		dns_name_t *name;

		INSIST(node == NULL);
		name = &t->name;
		/*
		 * Find the node.
		 * We create the node if it does not exist.
		 * This will cause an empty node to be created if the diff
		 * contains a deletion of an RR at a nonexistent name,
		 * but such diffs should never be created in the first
		 * place.
		 */

		while (t != NULL && dns_name_equal(&t->name, name)) {
			dns_rdatatype_t type, covers;
			dns_rdataclass_t rdclass;
			dns_diffop_t op;
			dns_rdatalist_t rdl;
			dns_rdataset_t rds;
			dns_rdataset_t ardataset;
			unsigned int options;

			op = t->op;
			type = t->rdata.type;
			rdclass = t->rdata.rdclass;
			covers = rdata_covers(&t->rdata);

			/*
			 * Collect a contiguous set of updates with
			 * the same operation (add/delete) and RR type
			 * into a single rdatalist so that the
			 * database rrset merging/subtraction code
			 * can work more efficiently than if each
			 * RR were merged into / subtracted from
			 * the database separately.
			 *
			 * This is done by linking rdata structures from the
			 * diff into "rdatalist".  This uses the rdata link
			 * field, not the diff link field, so the structure
			 * of the diff itself is not affected.
			 */

			dns_rdatalist_init(&rdl);
			rdl.type = type;
			rdl.covers = covers;
			rdl.rdclass = t->rdata.rdclass;
			rdl.ttl = t->ttl;

			node = NULL;
			if (type != dns_rdatatype_nsec3 &&
			    covers != dns_rdatatype_nsec3)
			{
				CHECK(dns_db_findnode(db, name, true, &node));
			} else {
				CHECK(dns_db_findnsec3node(db, name, true,
							   &node));
			}

			while (t != NULL && dns_name_equal(&t->name, name) &&
			       t->op == op && t->rdata.type == type &&
			       rdata_covers(&t->rdata) == covers)
			{
				/*
				 * Remember the add name for
				 * dns_rdataset_setownercase.
				 */
				name = &t->name;
				if (t->ttl != rdl.ttl && warn) {
					dns_name_format(name, namebuf,
							sizeof(namebuf));
					dns_rdatatype_format(t->rdata.type,
							     typebuf,
							     sizeof(typebuf));
					dns_rdataclass_format(t->rdata.rdclass,
							      classbuf,
							      sizeof(classbuf));
					isc_log_write(DNS_LOGCATEGORY_GENERAL,
						      DNS_LOGMODULE_DIFF,
						      ISC_LOG_WARNING,
						      "'%s/%s/%s': TTL differs "
						      "in "
						      "rdataset, adjusting "
						      "%lu -> %lu",
						      namebuf, typebuf,
						      classbuf,
						      (unsigned long)t->ttl,
						      (unsigned long)rdl.ttl);
				}
				ISC_LIST_APPEND(rdl.rdata, &t->rdata, link);
				t = ISC_LIST_NEXT(t, link);
			}

			/*
			 * Convert the rdatalist into a rdataset.
			 */
			dns_rdataset_init(&rds);
			dns_rdataset_init(&ardataset);
			dns_rdatalist_tordataset(&rdl, &rds);
			rds.trust = dns_trust_ultimate;

			/*
			 * Merge the rdataset into the database.
			 */
			switch (op) {
			case DNS_DIFFOP_ADD:
			case DNS_DIFFOP_ADDRESIGN:
				options = DNS_DBADD_MERGE | DNS_DBADD_EXACT |
					  DNS_DBADD_EXACTTTL;
				result = dns_db_addrdataset(db, node, ver, 0,
							    &rds, options,
							    &ardataset);
				break;
			case DNS_DIFFOP_DEL:
			case DNS_DIFFOP_DELRESIGN:
				options = DNS_DBSUB_EXACT | DNS_DBSUB_WANTOLD;
				result = dns_db_subtractrdataset(db, node, ver,
								 &rds, options,
								 &ardataset);
				break;
			default:
				UNREACHABLE();
			}

			if (result == ISC_R_SUCCESS) {
				if (rds.type == dns_rdatatype_rrsig &&
				    (op == DNS_DIFFOP_DELRESIGN ||
				     op == DNS_DIFFOP_ADDRESIGN))
				{
					isc_stdtime_t resign;
					resign = setresign(&ardataset);
					dns_db_setsigningtime(db, &ardataset,
							      resign);
				}
				if (op == DNS_DIFFOP_ADD ||
				    op == DNS_DIFFOP_ADDRESIGN)
				{
					setownercase(&ardataset, name);
				}
				if (op == DNS_DIFFOP_DEL ||
				    op == DNS_DIFFOP_DELRESIGN)
				{
					getownercase(&ardataset, name);
				}
			} else if (result == DNS_R_UNCHANGED) {
				/*
				 * This will not happen when executing a
				 * dynamic update, because that code will
				 * generate strictly minimal diffs.
				 * It may happen when receiving an IXFR
				 * from a server that is not as careful.
				 * Issue a warning and continue.
				 */
				if (warn) {
					dns_name_format(dns_db_origin(db),
							namebuf,
							sizeof(namebuf));
					dns_rdataclass_format(dns_db_class(db),
							      classbuf,
							      sizeof(classbuf));
					isc_log_write(DNS_LOGCATEGORY_GENERAL,
						      DNS_LOGMODULE_DIFF,
						      ISC_LOG_WARNING,
						      "%s/%s: dns_diff_apply: "
						      "update with no effect",
						      namebuf, classbuf);
				}
				if (op == DNS_DIFFOP_ADD ||
				    op == DNS_DIFFOP_ADDRESIGN)
				{
					setownercase(&ardataset, name);
				}
				if (op == DNS_DIFFOP_DEL ||
				    op == DNS_DIFFOP_DELRESIGN)
				{
					getownercase(&ardataset, name);
				}
			} else if (result == DNS_R_NXRRSET) {
				/*
				 * OK.
				 */
				if (op == DNS_DIFFOP_DEL ||
				    op == DNS_DIFFOP_DELRESIGN)
				{
					getownercase(&ardataset, name);
				}
				if (dns_rdataset_isassociated(&ardataset)) {
					dns_rdataset_disassociate(&ardataset);
				}
			} else {
				if (result == DNS_R_NOTEXACT) {
					dns_name_format(name, namebuf,
							sizeof(namebuf));
					dns_rdatatype_format(type, typebuf,
							     sizeof(typebuf));
					dns_rdataclass_format(rdclass, classbuf,
							      sizeof(classbuf));
					isc_log_write(
						DNS_LOGCATEGORY_GENERAL,
						DNS_LOGMODULE_DIFF,
						ISC_LOG_ERROR,
						"dns_diff_apply: %s/%s/%s: %s "
						"%s",
						namebuf, typebuf, classbuf,
						optotext(op),
						isc_result_totext(result));
				}
				if (dns_rdataset_isassociated(&ardataset)) {
					dns_rdataset_disassociate(&ardataset);
				}
				CHECK(result);
			}
			dns_db_detachnode(db, &node);
			if (dns_rdataset_isassociated(&ardataset)) {
				dns_rdataset_disassociate(&ardataset);
			}
		}
	}
	return ISC_R_SUCCESS;

failure:
	if (node != NULL) {
		dns_db_detachnode(db, &node);
	}
	return result;
}

isc_result_t
dns_diff_apply(const dns_diff_t *diff, dns_db_t *db, dns_dbversion_t *ver) {
	return diff_apply(diff, db, ver, true);
}

isc_result_t
dns_diff_applysilently(const dns_diff_t *diff, dns_db_t *db,
		       dns_dbversion_t *ver) {
	return diff_apply(diff, db, ver, false);
}

/* XXX this duplicates lots of code in diff_apply(). */

isc_result_t
dns_diff_load(const dns_diff_t *diff, dns_rdatacallbacks_t *callbacks) {
	dns_difftuple_t *t;
	isc_result_t result;

	REQUIRE(DNS_DIFF_VALID(diff));

	if (callbacks->setup != NULL) {
		callbacks->setup(callbacks->add_private);
	}

	t = ISC_LIST_HEAD(diff->tuples);
	while (t != NULL) {
		dns_name_t *name;

		name = &t->name;
		while (t != NULL && dns_name_caseequal(&t->name, name)) {
			dns_rdatatype_t type, covers;
			dns_diffop_t op;
			dns_rdatalist_t rdl;
			dns_rdataset_t rds;

			op = t->op;
			type = t->rdata.type;
			covers = rdata_covers(&t->rdata);

			dns_rdatalist_init(&rdl);
			rdl.type = type;
			rdl.covers = covers;
			rdl.rdclass = t->rdata.rdclass;
			rdl.ttl = t->ttl;

			while (t != NULL &&
			       dns_name_caseequal(&t->name, name) &&
			       t->op == op && t->rdata.type == type &&
			       rdata_covers(&t->rdata) == covers)
			{
				ISC_LIST_APPEND(rdl.rdata, &t->rdata, link);
				t = ISC_LIST_NEXT(t, link);
			}

			/*
			 * Convert the rdatalist into a rdataset.
			 */
			dns_rdataset_init(&rds);
			dns_rdatalist_tordataset(&rdl, &rds);
			rds.trust = dns_trust_ultimate;

			INSIST(op == DNS_DIFFOP_ADD);
			result = callbacks->add(callbacks->add_private, name,
						&rds DNS__DB_FILELINE);
			if (result == DNS_R_UNCHANGED) {
				isc_log_write(DNS_LOGCATEGORY_GENERAL,
					      DNS_LOGMODULE_DIFF,
					      ISC_LOG_WARNING,
					      "dns_diff_load: "
					      "update with no effect");
			} else if (result == ISC_R_SUCCESS ||
				   result == DNS_R_NXRRSET)
			{
				/*
				 * OK.
				 */
			} else {
				CHECK(result);
			}
		}
	}
	result = ISC_R_SUCCESS;

failure:
	if (callbacks->commit != NULL) {
		callbacks->commit(callbacks->add_private);
	}
	return result;
}

/*
 * XXX uses qsort(); a merge sort would be more natural for lists,
 * and perhaps safer wrt thread stack overflow.
 */
isc_result_t
dns_diff_sort(dns_diff_t *diff, dns_diff_compare_func *compare) {
	unsigned int length = 0;
	unsigned int i;
	dns_difftuple_t **v = NULL;
	REQUIRE(DNS_DIFF_VALID(diff));

	ISC_LIST_FOREACH (diff->tuples, p, link) {
		length++;
	}
	if (length == 0) {
		return ISC_R_SUCCESS;
	}
	v = isc_mem_cget(diff->mctx, length, sizeof(dns_difftuple_t *));
	for (i = 0; i < length; i++) {
		dns_difftuple_t *p = ISC_LIST_HEAD(diff->tuples);
		v[i] = p;
		ISC_LIST_UNLINK(diff->tuples, p, link);
	}
	INSIST(ISC_LIST_HEAD(diff->tuples) == NULL);
	qsort(v, length, sizeof(v[0]), compare);
	for (i = 0; i < length; i++) {
		ISC_LIST_APPEND(diff->tuples, v[i], link);
	}
	isc_mem_cput(diff->mctx, v, length, sizeof(dns_difftuple_t *));
	return ISC_R_SUCCESS;
}

/*
 * Create an rdataset containing the single RR of the given
 * tuple.  The caller must allocate the rdata, rdataset and
 * an rdatalist structure for it to refer to.
 */

static void
diff_tuple_tordataset(dns_difftuple_t *t, dns_rdata_t *rdata,
		      dns_rdatalist_t *rdl, dns_rdataset_t *rds) {
	REQUIRE(DNS_DIFFTUPLE_VALID(t));
	REQUIRE(rdl != NULL);
	REQUIRE(rds != NULL);

	dns_rdatalist_init(rdl);
	rdl->type = t->rdata.type;
	rdl->rdclass = t->rdata.rdclass;
	rdl->ttl = t->ttl;
	dns_rdataset_init(rds);
	ISC_LINK_INIT(rdata, link);
	dns_rdata_clone(&t->rdata, rdata);
	ISC_LIST_APPEND(rdl->rdata, rdata, link);
	dns_rdatalist_tordataset(rdl, rds);
}

isc_result_t
dns_diff_print(dns_diff_t *diff, FILE *file) {
	isc_result_t result;
	char *mem = NULL;
	unsigned int size = 2048;
	const char *op = NULL;

	REQUIRE(DNS_DIFF_VALID(diff));

	mem = isc_mem_get(diff->mctx, size);

	ISC_LIST_FOREACH (diff->tuples, t, link) {
		isc_buffer_t buf;
		isc_region_t r;

		dns_rdatalist_t rdl;
		dns_rdataset_t rds;
		dns_rdata_t rd = DNS_RDATA_INIT;

		diff_tuple_tordataset(t, &rd, &rdl, &rds);
	again:
		isc_buffer_init(&buf, mem, size);
		result = dns_rdataset_totext(&rds, &t->name, false, false,
					     &buf);

		if (result == ISC_R_NOSPACE) {
			isc_mem_put(diff->mctx, mem, size);
			size += 1024;
			mem = isc_mem_get(diff->mctx, size);
			goto again;
		}

		if (result != ISC_R_SUCCESS) {
			goto cleanup;
		}
		/*
		 * Get rid of final newline.
		 */
		INSIST(buf.used >= 1 &&
		       ((char *)buf.base)[buf.used - 1] == '\n');
		buf.used--;

		isc_buffer_usedregion(&buf, &r);
		switch (t->op) {
		case DNS_DIFFOP_EXISTS:
			op = "exists";
			break;
		case DNS_DIFFOP_ADD:
			op = "add";
			break;
		case DNS_DIFFOP_DEL:
			op = "del";
			break;
		case DNS_DIFFOP_ADDRESIGN:
			op = "add re-sign";
			break;
		case DNS_DIFFOP_DELRESIGN:
			op = "del re-sign";
			break;
		}
		if (file != NULL) {
			fprintf(file, "%s %.*s\n", op, (int)r.length,
				(char *)r.base);
		} else {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_DIFF, ISC_LOG_DEBUG(7),
				      "%s %.*s", op, (int)r.length,
				      (char *)r.base);
		}
	}
	result = ISC_R_SUCCESS;
cleanup:
	if (mem != NULL) {
		isc_mem_put(diff->mctx, mem, size);
	}
	return result;
}
