/*
 * Portions Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 *
 * Portions Copyright (C) Network Associates, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC AND NETWORK ASSOCIATES DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE
 * FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*! \file */

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <openssl/opensslv.h>

#include <isc/async.h>
#include <isc/atomic.h>
#include <isc/attributes.h>
#include <isc/base32.h>
#include <isc/commandline.h>
#include <isc/dir.h>
#include <isc/file.h>
#include <isc/hash.h>
#include <isc/hex.h>
#include <isc/lib.h>
#include <isc/log.h>
#include <isc/loop.h>
#include <isc/managers.h>
#include <isc/md.h>
#include <isc/mem.h>
#include <isc/mutex.h>
#include <isc/os.h>
#include <isc/random.h>
#include <isc/result.h>
#include <isc/rwlock.h>
#include <isc/safe.h>
#include <isc/serial.h>
#include <isc/stdio.h>
#include <isc/string.h>
#include <isc/tid.h>
#include <isc/time.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/diff.h>
#include <dns/dnssec.h>
#include <dns/ds.h>
#include <dns/fixedname.h>
#include <dns/kasp.h>
#include <dns/keyvalues.h>
#include <dns/lib.h>
#include <dns/master.h>
#include <dns/masterdump.h>
#include <dns/nsec.h>
#include <dns/nsec3.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatastruct.h>
#include <dns/rdatatype.h>
#include <dns/soa.h>
#include <dns/time.h>
#include <dns/update.h>
#include <dns/zoneverify.h>

#include <dst/dst.h>

#include "dnssectool.h"

typedef struct hashlist hashlist_t;

static int nsec_datatype = dns_rdatatype_nsec;

#define check_dns_dbiterator_current(result)                               \
	check_result((result == DNS_R_NEWORIGIN) ? ISC_R_SUCCESS : result, \
		     "dns_dbiterator_current()")

#define IS_NSEC3  (nsec_datatype == dns_rdatatype_nsec3)
#define OPTOUT(x) (((x) & DNS_NSEC3FLAG_OPTOUT) != 0)

#define REVOKE(x) ((dst_key_flags(x) & DNS_KEYFLAG_REVOKE) != 0)

#define BUFSIZE	  2048
#define MAXDSKEYS 8

#define SIGNER_EVENTCLASS  ISC_EVENTCLASS(0x4453)
#define SIGNER_EVENT_WRITE (SIGNER_EVENTCLASS + 0)
#define SIGNER_EVENT_WORK  (SIGNER_EVENTCLASS + 1)

#define SOA_SERIAL_KEEP	     0
#define SOA_SERIAL_INCREMENT 1
#define SOA_SERIAL_UNIXTIME  2
#define SOA_SERIAL_DATE	     3

static dns_dnsseckeylist_t keylist;
static unsigned int keycount = 0;
static isc_rwlock_t keylist_lock;
static isc_stdtime_t starttime = 0, endtime = 0, dnskey_endtime = 0, now;
static int cycle = -1;
static int jitter = 0;
static bool tryverify = false;
static bool printstats = false;
static isc_mem_t *mctx = NULL;
static dns_ttl_t zone_soa_min_ttl;
static dns_ttl_t soa_ttl;
static FILE *outfp = NULL;
static char *tempfile = NULL;
static const dns_master_style_t *masterstyle;
static dns_masterformat_t inputformat = dns_masterformat_text;
static dns_masterformat_t outputformat = dns_masterformat_text;
static uint32_t rawversion = 1, serialnum = 0;
static bool snset = false;
static atomic_uint_fast32_t nsigned = 0, nretained = 0, ndropped = 0;
static atomic_uint_fast32_t nverified = 0, nverifyfailed = 0;
static const char *directory = NULL, *dsdir = NULL;
static isc_mutex_t namelock;
static dns_db_t *gdb;		  /* The database */
static dns_dbversion_t *gversion; /* The database version */
static dns_dbiterator_t *gdbiter; /* The database iterator */
static dns_rdataclass_t gclass;	  /* The class */
static dns_name_t *gorigin;	  /* The database origin */
static int nsec3flags = 0;
static dns_iterations_t nsec3iter = 0U;
static unsigned char saltbuf[255];
static unsigned char *gsalt = saltbuf;
static size_t salt_length = 0;
static unsigned int nloops = 0;
static atomic_bool shuttingdown;
static atomic_bool finished;
static bool nokeys = false;
static bool removefile = false;
static bool generateds = false;
static bool ignore_kskflag = false;
static bool keyset_kskonly = false;
static dns_master_style_t *dsstyle = NULL;
static unsigned int serialformat = SOA_SERIAL_KEEP;
static unsigned int hash_length = 0;
static bool unknownalg = false;
static bool disable_zone_check = false;
static bool update_chain = false;
static bool set_keyttl = false;
static dns_ttl_t keyttl;
static bool smartsign = false;
static bool remove_orphansigs = false;
static bool remove_inactkeysigs = false;
static bool output_dnssec_only = false;
static bool output_stdout = false;
static bool set_maxttl = false;
static dns_ttl_t maxttl = 0;
static bool no_max_check = false;
static const char *sync_records = "cdnskey,cds:sha-256";

#define INCSTAT(counter)                               \
	if (printstats) {                              \
		atomic_fetch_add_relaxed(&counter, 1); \
	}

/*%
 * Store a copy of 'name' in 'fzonecut' and return a pointer to that copy.
 */
static dns_name_t *
savezonecut(dns_fixedname_t *fzonecut, dns_name_t *name) {
	dns_name_t *result;

	result = dns_fixedname_initname(fzonecut);
	dns_name_copy(name, result);

	return result;
}

static void
dumpnode(dns_name_t *name, dns_dbnode_t *node) {
	dns_rdatasetiter_t *iter = NULL;
	isc_buffer_t *buffer = NULL;
	isc_region_t r;
	isc_result_t result;
	unsigned int bufsize = 4096;

	if (!output_dnssec_only) {
		return;
	}

	result = dns_db_allrdatasets(gdb, node, gversion, 0, 0, &iter);
	check_result(result, "dns_db_allrdatasets");

	isc_buffer_allocate(mctx, &buffer, bufsize);

	DNS_RDATASETITER_FOREACH (iter) {
		dns_rdataset_t rds = DNS_RDATASET_INIT;
		dns_rdatasetiter_current(iter, &rds);

		if (rds.type != dns_rdatatype_rrsig &&
		    rds.type != dns_rdatatype_nsec &&
		    rds.type != dns_rdatatype_nsec3 &&
		    rds.type != dns_rdatatype_nsec3param &&
		    (!smartsign || rds.type != dns_rdatatype_dnskey))
		{
			dns_rdataset_disassociate(&rds);
			continue;
		}

		for (;;) {
			result = dns_master_rdatasettotext(
				name, &rds, masterstyle, NULL, buffer);
			if (result != ISC_R_NOSPACE) {
				break;
			}

			bufsize <<= 1;
			isc_buffer_free(&buffer);
			isc_buffer_allocate(mctx, &buffer, bufsize);
		}
		check_result(result, "dns_master_rdatasettotext");

		isc_buffer_usedregion(buffer, &r);
		result = isc_stdio_write(r.base, 1, r.length, outfp, NULL);
		check_result(result, "isc_stdio_write");
		isc_buffer_clear(buffer);

		dns_rdataset_disassociate(&rds);
	}

	isc_buffer_free(&buffer);
	dns_rdatasetiter_destroy(&iter);
}

static void
lock_and_dumpnode(dns_name_t *name, dns_dbnode_t *node) {
	if (!output_dnssec_only) {
		return;
	}

	LOCK(&namelock);
	dumpnode(name, node);
	UNLOCK(&namelock);
}

/*%
 * Sign the given RRset with given key, and add the signature record to the
 * given tuple.
 */
static void
signwithkey(dns_name_t *name, dns_rdataset_t *rdataset, dst_key_t *key,
	    dns_ttl_t ttl, dns_diff_t *add, const char *logmsg) {
	isc_result_t result;
	isc_stdtime_t jendtime, expiry;
	char keystr[DST_KEY_FORMATSIZE];
	dns_rdata_t trdata = DNS_RDATA_INIT;
	unsigned char array[BUFSIZE];
	isc_buffer_t b;
	dns_difftuple_t *tuple;

	dst_key_format(key, keystr, sizeof(keystr));
	vbprintf(1, "\t%s %s\n", logmsg, keystr);

	if (rdataset->type == dns_rdatatype_dnskey) {
		expiry = dnskey_endtime;
	} else {
		expiry = endtime;
	}

	jendtime = (jitter != 0) ? expiry - isc_random_uniform(jitter) : expiry;
	isc_buffer_init(&b, array, sizeof(array));
	result = dns_dnssec_sign(name, rdataset, key, &starttime, &jendtime,
				 mctx, &b, &trdata);
	if (result != ISC_R_SUCCESS) {
		fatal("dnskey '%s' failed to sign data: %s", keystr,
		      isc_result_totext(result));
	}
	INCSTAT(nsigned);

	if (tryverify) {
		result = dns_dnssec_verify(name, rdataset, key, true, mctx,
					   &trdata, NULL);
		if (result == ISC_R_SUCCESS || result == DNS_R_FROMWILDCARD) {
			vbprintf(3, "\tsignature verified\n");
			INCSTAT(nverified);
		} else {
			vbprintf(3, "\tsignature failed to verify\n");
			INCSTAT(nverifyfailed);
		}
	}

	tuple = NULL;
	dns_difftuple_create(mctx, DNS_DIFFOP_ADDRESIGN, name, ttl, &trdata,
			     &tuple);
	dns_diff_append(add, &tuple);
}

static bool
issigningkey(dns_dnsseckey_t *key) {
	return key->force_sign || key->hint_sign;
}

static bool
ispublishedkey(dns_dnsseckey_t *key) {
	return (key->force_publish || key->hint_publish) && !key->hint_remove;
}

static bool
iszonekey(dns_dnsseckey_t *key) {
	return dns_name_equal(dst_key_name(key->key), gorigin) &&
	       dst_key_iszonekey(key->key);
}

static bool
isksk(dns_dnsseckey_t *key) {
	return key->ksk;
}

static bool
iszsk(dns_dnsseckey_t *key) {
	return ignore_kskflag || !key->ksk;
}

/*%
 * Find the key that generated an RRSIG, if it is in the key list.  If
 * so, return a pointer to it, otherwise return NULL.
 *
 * No locking is performed here, this must be done by the caller.
 */
static dns_dnsseckey_t *
keythatsigned_unlocked(dns_rdata_rrsig_t *rrsig) {
	dst_algorithm_t algorithm = dst_algorithm_fromdata(
		rrsig->algorithm, rrsig->signature, rrsig->siglen);

	ISC_LIST_FOREACH (keylist, key, link) {
		if (rrsig->keyid == dst_key_id(key->key) &&
		    algorithm == dst_key_alg(key->key) &&
		    dns_name_equal(&rrsig->signer, dst_key_name(key->key)))
		{
			return key;
		}
	}
	return NULL;
}

/*%
 * Finds the key that generated a RRSIG, if possible.  First look at the keys
 * that we've loaded already, and then see if there's a key on disk.
 */
static dns_dnsseckey_t *
keythatsigned(dns_rdata_rrsig_t *rrsig) {
	isc_result_t result;
	dst_key_t *pubkey = NULL, *privkey = NULL;
	dns_dnsseckey_t *key = NULL;

	RWLOCK(&keylist_lock, isc_rwlocktype_read);
	key = keythatsigned_unlocked(rrsig);
	RWUNLOCK(&keylist_lock, isc_rwlocktype_read);
	if (key != NULL) {
		return key;
	}

	/*
	 * We did not find the key in our list.  Get a write lock now, since
	 * we may be modifying the bits.  We could do the tryupgrade() dance,
	 * but instead just get a write lock and check once again to see if
	 * it is on our list.  It's possible someone else may have added it
	 * after all.
	 */
	isc_rwlock_lock(&keylist_lock, isc_rwlocktype_write);
	key = keythatsigned_unlocked(rrsig);
	if (key != NULL) {
		isc_rwlock_unlock(&keylist_lock, isc_rwlocktype_write);
		return key;
	}

	result = dst_key_fromfile(&rrsig->signer, rrsig->keyid,
				  rrsig->algorithm, DST_TYPE_PUBLIC, directory,
				  mctx, &pubkey);
	if (result != ISC_R_SUCCESS) {
		isc_rwlock_unlock(&keylist_lock, isc_rwlocktype_write);
		return NULL;
	}

	result = dst_key_fromfile(
		&rrsig->signer, rrsig->keyid, rrsig->algorithm,
		DST_TYPE_PUBLIC | DST_TYPE_PRIVATE, directory, mctx, &privkey);
	if (result == ISC_R_SUCCESS) {
		dst_key_free(&pubkey);
		dns_dnsseckey_create(mctx, &privkey, &key);
	} else {
		dns_dnsseckey_create(mctx, &pubkey, &key);
		key->pubkey = true;
	}

	key->index = keycount++;
	ISC_LIST_APPEND(keylist, key, link);

	isc_rwlock_unlock(&keylist_lock, isc_rwlocktype_write);
	return key;
}

/*%
 * Check to see if we expect to find a key at this name.  If we see a RRSIG
 * and can't find the signing key that we expect to find, we drop the rrsig.
 * I'm not sure if this is completely correct, but it seems to work.
 */
static bool
expecttofindkey(dns_name_t *name) {
	unsigned int options = DNS_DBFIND_NOWILD;
	dns_fixedname_t fname;
	isc_result_t result;
	char namestr[DNS_NAME_FORMATSIZE];

	dns_fixedname_init(&fname);
	result = dns_db_find(gdb, name, gversion, dns_rdatatype_dnskey, options,
			     0, NULL, dns_fixedname_name(&fname), NULL, NULL);
	switch (result) {
	case ISC_R_SUCCESS:
	case DNS_R_NXDOMAIN:
	case DNS_R_NXRRSET:
		return true;
	case DNS_R_DELEGATION:
	case DNS_R_CNAME:
	case DNS_R_DNAME:
		return false;
	default:
		break;
	}
	dns_name_format(name, namestr, sizeof(namestr));
	fatal("failure looking for '%s DNSKEY' in database: %s", namestr,
	      isc_result_totext(result));
	UNREACHABLE();
	return false; /* removes a warning */
}

static bool
setverifies(dns_name_t *name, dns_rdataset_t *set, dst_key_t *key,
	    dns_rdata_t *rrsig) {
	isc_result_t result;
	result = dns_dnssec_verify(name, set, key, false, mctx, rrsig, NULL);
	if (result == ISC_R_SUCCESS || result == DNS_R_FROMWILDCARD) {
		INCSTAT(nverified);
		return true;
	} else {
		INCSTAT(nverifyfailed);
		return false;
	}
}

/*%
 * Signs a set.  Goes through contortions to decide if each RRSIG should
 * be dropped or retained, and then determines if any new SIGs need to
 * be generated.
 */
static void
signset(dns_diff_t *del, dns_diff_t *add, dns_dbnode_t *node, dns_name_t *name,
	dns_rdataset_t *set) {
	dns_rdataset_t sigset;
	dns_rdata_rrsig_t rrsig;
	isc_result_t result;
	bool nosigs = false;
	bool *wassignedby, *nowsignedby;
	int arraysize;
	dns_difftuple_t *tuple;
	dns_ttl_t ttl;
	int i;
	char namestr[DNS_NAME_FORMATSIZE];
	char typestr[DNS_RDATATYPE_FORMATSIZE];
	char sigstr[SIG_FORMATSIZE];

	dns_name_format(name, namestr, sizeof(namestr));
	dns_rdatatype_format(set->type, typestr, sizeof(typestr));

	ttl = ISC_MIN(set->ttl, endtime - starttime);

	dns_rdataset_init(&sigset);
	result = dns_db_findrdataset(gdb, node, gversion, dns_rdatatype_rrsig,
				     set->type, 0, &sigset, NULL);
	if (result == ISC_R_NOTFOUND) {
		vbprintf(2, "no existing signatures for %s/%s\n", namestr,
			 typestr);
		result = ISC_R_SUCCESS;
		nosigs = true;
	}
	if (result != ISC_R_SUCCESS) {
		fatal("failed while looking for '%s RRSIG %s': %s", namestr,
		      typestr, isc_result_totext(result));
	}

	vbprintf(1, "%s/%s:\n", namestr, typestr);

	arraysize = keycount;
	if (!nosigs) {
		arraysize += dns_rdataset_count(&sigset);
	}
	wassignedby = isc_mem_cget(mctx, arraysize, sizeof(bool));
	nowsignedby = isc_mem_cget(mctx, arraysize, sizeof(bool));

	for (i = 0; i < arraysize; i++) {
		wassignedby[i] = nowsignedby[i] = false;
	}

	if (!nosigs) {
		DNS_RDATASET_FOREACH (&sigset) {
			dns_rdata_t sigrdata = DNS_RDATA_INIT;
			dns_dnsseckey_t *key = NULL;
			bool expired, refresh, future, offline;
			bool keep = false, resign = false;

			dns_rdataset_current(&sigset, &sigrdata);

			result = dns_rdata_tostruct(&sigrdata, &rrsig, NULL);
			check_result(result, "dns_rdata_tostruct");

			future = isc_serial_lt(now, rrsig.timesigned);

			key = keythatsigned(&rrsig);
			offline = (key != NULL) ? key->pubkey : false;
			sig_format(&rrsig, sigstr, sizeof(sigstr));
			expired = isc_serial_gt(now, rrsig.timeexpire);
			refresh = isc_serial_gt(now + cycle, rrsig.timeexpire);

			if (isc_serial_gt(rrsig.timesigned, rrsig.timeexpire)) {
				/* rrsig is dropped and not replaced */
				vbprintf(2,
					 "\trrsig by %s dropped - "
					 "invalid validity period\n",
					 sigstr);
			} else if (key == NULL && !future &&
				   expecttofindkey(&rrsig.signer))
			{
				/* rrsig is dropped and not replaced */
				vbprintf(2,
					 "\trrsig by %s dropped - "
					 "private dnskey not found\n",
					 sigstr);
			} else if (key == NULL || future) {
				keep = (!expired && !remove_orphansigs);
				vbprintf(
					2,
					"\trrsig by %s %s - dnskey not found\n",
					keep ? "retained" : "dropped", sigstr);
			} else if (!dns_dnssec_keyactive(key->key, now) &&
				   remove_inactkeysigs)
			{
				keep = false;
				vbprintf(2,
					 "\trrsig by %s dropped - key "
					 "inactive\n",
					 sigstr);
			} else if (issigningkey(key)) {
				wassignedby[key->index] = true;

				if (!refresh && rrsig.originalttl == set->ttl &&
				    setverifies(name, set, key->key, &sigrdata))
				{
					vbprintf(2, "\trrsig by %s retained\n",
						 sigstr);
					keep = true;
				} else if (offline) {
					vbprintf(2,
						 "\trrsig by %s retained - "
						 "private key "
						 "missing\n",
						 sigstr);
					keep = true;
				} else {
					vbprintf(2,
						 "\trrsig by %s dropped - %s\n",
						 sigstr,
						 refresh ? "refresh"
						 : rrsig.originalttl != set->ttl
							 ? "ttl change"
							 : "failed to "
							   "verify");
					resign = true;
				}
			} else if (!ispublishedkey(key) && remove_orphansigs) {
				vbprintf(2,
					 "\trrsig by %s dropped - dnskey "
					 "removed\n",
					 sigstr);
			} else if (iszonekey(key)) {
				wassignedby[key->index] = true;

				if (!refresh && rrsig.originalttl == set->ttl &&
				    setverifies(name, set, key->key, &sigrdata))
				{
					vbprintf(2, "\trrsig by %s retained\n",
						 sigstr);
					keep = true;
				} else if (offline) {
					vbprintf(2,
						 "\trrsig by %s retained - "
						 "private key "
						 "missing\n",
						 sigstr);
					keep = true;
				} else {
					vbprintf(2,
						 "\trrsig by %s dropped - %s\n",
						 sigstr,
						 refresh ? "refresh"
						 : rrsig.originalttl != set->ttl
							 ? "ttl change"
							 : "failed to "
							   "verify");
				}
			} else if (!refresh) {
				vbprintf(2, "\trrsig by %s retained\n", sigstr);
				keep = true;
			} else {
				vbprintf(2, "\trrsig by %s %s\n", sigstr,
					 expired ? "expired" : "needs refresh");
			}

			if (keep) {
				if (key != NULL) {
					nowsignedby[key->index] = true;
				}
				INCSTAT(nretained);
				if (sigset.ttl != ttl) {
					vbprintf(2, "\tfixing ttl %s\n",
						 sigstr);
					tuple = NULL;
					dns_difftuple_create(
						mctx, DNS_DIFFOP_DELRESIGN,
						name, sigset.ttl, &sigrdata,
						&tuple);
					dns_diff_append(del, &tuple);
					dns_difftuple_create(
						mctx, DNS_DIFFOP_ADDRESIGN,
						name, ttl, &sigrdata, &tuple);
					dns_diff_append(add, &tuple);
				}
			} else {
				tuple = NULL;
				vbprintf(2, "\tremoving signature by %s\n",
					 sigstr);
				dns_difftuple_create(mctx, DNS_DIFFOP_DELRESIGN,
						     name, sigset.ttl,
						     &sigrdata, &tuple);
				dns_diff_append(del, &tuple);
				INCSTAT(ndropped);
			}

			if (resign) {
				INSIST(!keep);

				signwithkey(name, set, key->key, ttl, add,
					    "resigning with dnskey");
				nowsignedby[key->index] = true;
			}

			dns_rdata_reset(&sigrdata);
			dns_rdata_freestruct(&rrsig);
		}
	}

	check_result(result, "dns_rdataset_first/next");
	if (dns_rdataset_isassociated(&sigset)) {
		dns_rdataset_disassociate(&sigset);
	}

	ISC_LIST_FOREACH (keylist, key, link) {
		if (REVOKE(key->key) && set->type != dns_rdatatype_dnskey) {
			continue;
		}

		if (nowsignedby[key->index]) {
			continue;
		}

		if (!issigningkey(key)) {
			continue;
		}

		if ((set->type == dns_rdatatype_cds ||
		     set->type == dns_rdatatype_cdnskey ||
		     set->type == dns_rdatatype_dnskey) &&
		    dns_name_equal(name, gorigin))
		{
			bool have_ksk = isksk(key);

			ISC_LIST_FOREACH (keylist, curr, link) {
				if (dst_key_alg(key->key) !=
				    dst_key_alg(curr->key))
				{
					continue;
				}
				if (REVOKE(curr->key)) {
					continue;
				}
				if (isksk(curr)) {
					have_ksk = true;
				}
			}
			if (isksk(key) || !have_ksk ||
			    (iszsk(key) && !keyset_kskonly))
			{
				signwithkey(name, set, key->key, ttl, add,
					    "signing with dnskey");
			}
		} else if (iszsk(key)) {
			/*
			 * Sign with the ZSK unless there is a predecessor
			 * key that already signs this RRset.
			 */
			bool have_pre_sig = false;
			uint32_t pre;
			isc_result_t ret = dst_key_getnum(
				key->key, DST_NUM_PREDECESSOR, &pre);
			if (ret == ISC_R_SUCCESS) {
				/*
				 * This key has a predecessor, look for the
				 * corresponding key in the keylist. The
				 * key we are looking for must be:
				 * - From the same cryptographic algorithm.
				 * - Have the ZSK type (iszsk).
				 * - Have key ID equal to the predecessor id.
				 * - Have a successor that matches 'key' id.
				 */
				ISC_LIST_FOREACH (keylist, curr, link) {
					uint32_t suc;

					if (dst_key_alg(key->key) !=
						    dst_key_alg(curr->key) ||
					    !iszsk(curr) ||
					    dst_key_id(curr->key) != pre)
					{
						continue;
					}
					ret = dst_key_getnum(curr->key,
							     DST_NUM_SUCCESSOR,
							     &suc);
					if (ret != ISC_R_SUCCESS ||
					    dst_key_id(key->key) != suc)
					{
						continue;
					}

					/*
					 * curr is the predecessor we were
					 * looking for. Check if this key
					 * signs this RRset.
					 */
					if (nowsignedby[curr->index]) {
						have_pre_sig = true;
					}
				}
			}

			/*
			 * If we have a signature of a predecessor key,
			 * skip signing with this key.
			 */
			if (!have_pre_sig) {
				signwithkey(name, set, key->key, ttl, add,
					    "signing with dnskey");
			}
		}
	}

	isc_mem_cput(mctx, wassignedby, arraysize, sizeof(bool));
	isc_mem_cput(mctx, nowsignedby, arraysize, sizeof(bool));
}

struct hashlist {
	unsigned char *hashbuf;
	size_t entries;
	size_t size;
	size_t length;
};

static void
hashlist_init(hashlist_t *l, unsigned int nodes, unsigned int length) {
	l->entries = 0;
	l->length = length + 1;

	if (nodes != 0) {
		l->size = nodes;
		l->hashbuf = malloc(l->size * l->length);
		if (l->hashbuf == NULL) {
			l->size = 0;
		}
	} else {
		l->size = 0;
		l->hashbuf = NULL;
	}
}

static void
hashlist_free(hashlist_t *l) {
	if (l->hashbuf) {
		free(l->hashbuf);
		l->hashbuf = NULL;
		l->entries = 0;
		l->length = 0;
		l->size = 0;
	}
}

static void
hashlist_add(hashlist_t *l, const unsigned char *hash, size_t len) {
	REQUIRE(len <= l->length);

	if (l->entries == l->size) {
		l->size = l->size * 2 + 100;
		l->hashbuf = realloc(l->hashbuf, l->size * l->length);
		if (l->hashbuf == NULL) {
			fatal("unable to grow hashlist: out of memory");
		}
	}
	memset(l->hashbuf + l->entries * l->length, 0, l->length);
	memmove(l->hashbuf + l->entries * l->length, hash, len);
	l->entries++;
}

static void
hashlist_add_dns_name(hashlist_t *l,
		      /*const*/ dns_name_t *name, unsigned int hashalg,
		      unsigned int iterations, const unsigned char *salt,
		      size_t salt_len, bool speculative) {
	char nametext[DNS_NAME_FORMATSIZE];
	unsigned char hash[NSEC3_MAX_HASH_LENGTH + 1];
	unsigned int len;
	size_t i;

	len = isc_iterated_hash(hash, hashalg, iterations, salt, (int)salt_len,
				name->ndata, name->length);
	if (verbose) {
		dns_name_format(name, nametext, sizeof nametext);
		for (i = 0; i < len; i++) {
			fprintf(stderr, "%02x", hash[i]);
		}
		fprintf(stderr, " %s\n", nametext);
	}
	hash[len++] = speculative ? 1 : 0;
	hashlist_add(l, hash, len);
}

static int
hashlist_comp(const void *a, const void *b) {
	return memcmp(a, b, hash_length + 1);
}

static void
hashlist_sort(hashlist_t *l) {
	INSIST(l->hashbuf != NULL || l->length == 0);
	if (l->length > 0) {
		qsort(l->hashbuf, l->entries, l->length, hashlist_comp);
	}
}

static bool
hashlist_hasdup(hashlist_t *l) {
	unsigned char *current;
	unsigned char *next = l->hashbuf;
	size_t entries = l->entries;

	/*
	 * Skip initial speculative wild card hashes.
	 */
	while (entries > 0U && next[l->length - 1] != 0U) {
		next += l->length;
		entries--;
	}

	current = next;
	while (entries-- > 1U) {
		next += l->length;
		if (next[l->length - 1] != 0) {
			continue;
		}
		if (isc_safe_memequal(current, next, l->length - 1)) {
			return true;
		}
		current = next;
	}
	return false;
}

static const unsigned char *
hashlist_findnext(const hashlist_t *l,
		  const unsigned char hash[NSEC3_MAX_HASH_LENGTH]) {
	size_t entries = l->entries;
	const unsigned char *next = bsearch(hash, l->hashbuf, l->entries,
					    l->length, hashlist_comp);
	INSIST(next != NULL);

	do {
		if (next < l->hashbuf + (l->entries - 1) * l->length) {
			next += l->length;
		} else {
			next = l->hashbuf;
		}
		if (next[l->length - 1] == 0) {
			break;
		}
	} while (entries-- > 1U);
	INSIST(entries != 0U);
	return next;
}

static bool
hashlist_exists(const hashlist_t *l,
		const unsigned char hash[NSEC3_MAX_HASH_LENGTH]) {
	if (bsearch(hash, l->hashbuf, l->entries, l->length, hashlist_comp)) {
		return true;
	} else {
		return false;
	}
}

static void
addnowildcardhash(hashlist_t *l,
		  /*const*/ dns_name_t *name, unsigned int hashalg,
		  unsigned int iterations, const unsigned char *salt,
		  size_t salt_len) {
	dns_fixedname_t fixed;
	dns_name_t *wild;
	dns_dbnode_t *node = NULL;
	isc_result_t result;
	char namestr[DNS_NAME_FORMATSIZE];

	wild = dns_fixedname_initname(&fixed);

	result = dns_name_concatenate(dns_wildcardname, name, wild);
	if (result == ISC_R_NOSPACE) {
		return;
	}
	check_result(result, "addnowildcardhash: dns_name_concatenate()");

	result = dns_db_findnode(gdb, wild, false, &node);
	if (result == ISC_R_SUCCESS) {
		dns_db_detachnode(gdb, &node);
		return;
	}

	if (verbose) {
		dns_name_format(wild, namestr, sizeof(namestr));
		fprintf(stderr, "adding no-wildcardhash for %s\n", namestr);
	}

	hashlist_add_dns_name(l, wild, hashalg, iterations, salt, salt_len,
			      true);
}

static void
opendb(const char *prefix, dns_name_t *name, dns_rdataclass_t rdclass,
       dns_db_t **dbp) {
	char filename[PATH_MAX];
	isc_buffer_t b;
	isc_result_t result;

	isc_buffer_init(&b, filename, sizeof(filename));
	if (dsdir != NULL) {
		/* allow room for a trailing slash */
		if (strlen(dsdir) >= isc_buffer_availablelength(&b)) {
			fatal("path '%s' is too long", dsdir);
		}
		isc_buffer_putstr(&b, dsdir);
		if (dsdir[strlen(dsdir) - 1] != '/') {
			isc_buffer_putstr(&b, "/");
		}
	}
	if (strlen(prefix) > isc_buffer_availablelength(&b)) {
		fatal("path '%s' is too long", dsdir);
	}
	isc_buffer_putstr(&b, prefix);
	result = dns_name_tofilenametext(name, false, &b);
	check_result(result, "dns_name_tofilenametext()");
	if (isc_buffer_availablelength(&b) == 0) {
		char namestr[DNS_NAME_FORMATSIZE];
		dns_name_format(name, namestr, sizeof(namestr));
		fatal("name '%s' is too long", namestr);
	}
	isc_buffer_putuint8(&b, 0);

	result = dns_db_create(mctx, ZONEDB_DEFAULT, dns_rootname,
			       dns_dbtype_zone, rdclass, 0, NULL, dbp);
	check_result(result, "dns_db_create()");

	result = dns_db_load(*dbp, filename, inputformat, DNS_MASTER_HINT);
	if (result != ISC_R_SUCCESS && result != DNS_R_SEENINCLUDE) {
		dns_db_detach(dbp);
	}
}

/*%
 * Load the DS set for a child zone, if a dsset-* file can be found.
 * If not, try to find a keyset-* file from an earlier version of
 * dnssec-signzone, and build DS records from that.
 */
static isc_result_t
loadds(dns_name_t *name, uint32_t ttl, dns_rdataset_t *dsset) {
	dns_db_t *db = NULL;
	dns_dbversion_t *ver = NULL;
	dns_dbnode_t *node = NULL;
	isc_result_t result;
	dns_rdataset_t keyset;
	unsigned char dsbuf[DNS_DS_BUFFERSIZE];
	dns_diff_t diff;
	dns_difftuple_t *tuple = NULL;

	opendb("dsset-", name, gclass, &db);
	if (db != NULL) {
		result = dns_db_findnode(db, name, false, &node);
		if (result == ISC_R_SUCCESS) {
			dns_rdataset_init(dsset);
			result = dns_db_findrdataset(db, node, NULL,
						     dns_rdatatype_ds, 0, 0,
						     dsset, NULL);
			dns_db_detachnode(db, &node);
			if (result == ISC_R_SUCCESS) {
				vbprintf(2, "found DS records\n");
				dsset->ttl = ttl;
				dns_db_detach(&db);
				return result;
			}
		}
		dns_db_detach(&db);
	}

	/* No DS records found; try again, looking for DNSKEY records */
	opendb("keyset-", name, gclass, &db);
	if (db == NULL) {
		return ISC_R_NOTFOUND;
	}

	result = dns_db_findnode(db, name, false, &node);
	if (result != ISC_R_SUCCESS) {
		dns_db_detach(&db);
		return result;
	}

	dns_rdataset_init(&keyset);
	result = dns_db_findrdataset(db, node, NULL, dns_rdatatype_dnskey, 0, 0,
				     &keyset, NULL);
	if (result != ISC_R_SUCCESS) {
		dns_db_detachnode(db, &node);
		dns_db_detach(&db);
		return result;
	}
	vbprintf(2, "found DNSKEY records\n");

	result = dns_db_newversion(db, &ver);
	check_result(result, "dns_db_newversion");
	dns_diff_init(mctx, &diff);

	DNS_RDATASET_FOREACH (&keyset) {
		dns_rdata_t key = DNS_RDATA_INIT;
		dns_rdata_t ds = DNS_RDATA_INIT;
		dns_rdataset_current(&keyset, &key);
		result = dns_ds_buildrdata(name, &key, DNS_DSDIGEST_SHA256,
					   dsbuf, sizeof(dsbuf), &ds);
		check_result(result, "dns_ds_buildrdata");

		dns_difftuple_create(mctx, DNS_DIFFOP_ADDRESIGN, name, ttl, &ds,
				     &tuple);
		dns_diff_append(&diff, &tuple);
	}

	result = dns_diff_apply(&diff, db, ver);
	check_result(result, "dns_diff_apply");
	dns_diff_clear(&diff);

	dns_db_closeversion(db, &ver, true);

	result = dns_db_findrdataset(db, node, NULL, dns_rdatatype_ds, 0, 0,
				     dsset, NULL);
	check_result(result, "dns_db_findrdataset");

	dns_rdataset_disassociate(&keyset);
	dns_db_detachnode(db, &node);
	dns_db_detach(&db);
	return result;
}

static bool
secure(dns_name_t *name, dns_dbnode_t *node) {
	dns_rdataset_t dsset;
	isc_result_t result;

	if (dns_name_equal(name, gorigin)) {
		return false;
	}

	dns_rdataset_init(&dsset);
	result = dns_db_findrdataset(gdb, node, gversion, dns_rdatatype_ds, 0,
				     0, &dsset, NULL);
	if (dns_rdataset_isassociated(&dsset)) {
		dns_rdataset_disassociate(&dsset);
	}

	return result == ISC_R_SUCCESS;
}

static bool
is_delegation(dns_db_t *db, dns_dbversion_t *ver, dns_name_t *origin,
	      dns_name_t *name, dns_dbnode_t *node, uint32_t *ttlp) {
	dns_rdataset_t nsset;
	isc_result_t result;

	if (dns_name_equal(name, origin)) {
		return false;
	}

	dns_rdataset_init(&nsset);
	result = dns_db_findrdataset(db, node, ver, dns_rdatatype_ns, 0, 0,
				     &nsset, NULL);
	if (dns_rdataset_isassociated(&nsset)) {
		if (ttlp != NULL) {
			*ttlp = nsset.ttl;
		}
		dns_rdataset_disassociate(&nsset);
	}

	return result == ISC_R_SUCCESS;
}

/*%
 * Return true if version 'ver' of database 'db' contains a DNAME RRset at
 * 'node'; return false otherwise.
 */
static bool
has_dname(dns_db_t *db, dns_dbversion_t *ver, dns_dbnode_t *node) {
	dns_rdataset_t dnameset;
	isc_result_t result;

	dns_rdataset_init(&dnameset);
	result = dns_db_findrdataset(db, node, ver, dns_rdatatype_dname, 0, 0,
				     &dnameset, NULL);
	if (dns_rdataset_isassociated(&dnameset)) {
		dns_rdataset_disassociate(&dnameset);
	}

	return result == ISC_R_SUCCESS;
}

/*%
 * Signs all records at a name.
 */
static void
signname(dns_dbnode_t *node, bool apex, dns_name_t *name) {
	isc_result_t result;
	dns_rdataset_t rdataset;
	dns_rdatasetiter_t *rdsiter;
	bool isdelegation = false;
	dns_diff_t del, add;
	char namestr[DNS_NAME_FORMATSIZE];

	dns_rdataset_init(&rdataset);
	dns_name_format(name, namestr, sizeof(namestr));

	/*
	 * Determine if this is a delegation point.
	 */
	if (is_delegation(gdb, gversion, gorigin, name, node, NULL)) {
		isdelegation = true;
	}

	/*
	 * Now iterate through the rdatasets.
	 */
	dns_diff_init(mctx, &del);
	dns_diff_init(mctx, &add);
	rdsiter = NULL;
	result = dns_db_allrdatasets(gdb, node, gversion, 0, 0, &rdsiter);
	check_result(result, "dns_db_allrdatasets()");
	DNS_RDATASETITER_FOREACH (rdsiter) {
		dns_rdatasetiter_current(rdsiter, &rdataset);

		/* If this is a RRSIG set, skip it. */
		if (rdataset.type == dns_rdatatype_rrsig) {
			goto skip;
		}

		/*
		 * If this name is a delegation point, skip all records
		 * except NSEC and DS sets.  Otherwise check that there
		 * isn't a DS record.
		 */
		if (isdelegation) {
			if (rdataset.type != nsec_datatype &&
			    rdataset.type != dns_rdatatype_ds)
			{
				goto skip;
			}
		} else if (rdataset.type == dns_rdatatype_ds) {
			char namebuf[DNS_NAME_FORMATSIZE];
			dns_name_format(name, namebuf, sizeof(namebuf));
			fatal("'%s': found DS RRset without NS RRset\n",
			      namebuf);
		} else if (rdataset.type == dns_rdatatype_dnskey && !apex) {
			char namebuf[DNS_NAME_FORMATSIZE];
			dns_name_format(name, namebuf, sizeof(namebuf));
			fatal("'%s': Non-apex DNSKEY RRset\n", namebuf);
		}

		signset(&del, &add, node, name, &rdataset);

	skip:
		dns_rdataset_disassociate(&rdataset);
	}

	dns_rdatasetiter_destroy(&rdsiter);

	result = dns_diff_applysilently(&del, gdb, gversion);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to delete SIGs at node '%s': %s", namestr,
		      isc_result_totext(result));
	}

	result = dns_diff_applysilently(&add, gdb, gversion);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to add SIGs at node '%s': %s", namestr,
		      isc_result_totext(result));
	}

	dns_diff_clear(&del);
	dns_diff_clear(&add);
}

/*
 * See if the node contains any non RRSIG/NSEC records and report to
 * caller.  Clean out extraneous RRSIG records for node.
 */
static bool
active_node(dns_dbnode_t *node) {
	dns_rdatasetiter_t *rdsiter = NULL;
	dns_rdatasetiter_t *rdsiter2 = NULL;
	bool active = false;
	isc_result_t result;
	dns_rdataset_t rdataset = DNS_RDATASET_INIT;
	dns_rdatatype_t type;
	dns_rdatatype_t covers;
	bool found;

	result = dns_db_allrdatasets(gdb, node, gversion, 0, 0, &rdsiter);
	check_result(result, "dns_db_allrdatasets()");
	DNS_RDATASETITER_FOREACH (rdsiter) {
		dns_rdatasetiter_current(rdsiter, &rdataset);
		dns_rdatatype_t t = rdataset.type;
		dns_rdataset_disassociate(&rdataset);

		if (t != dns_rdatatype_nsec && t != dns_rdatatype_nsec3 &&
		    t != dns_rdatatype_rrsig)
		{
			active = true;
			break;
		}
	}

	if (!active && nsec_datatype == dns_rdatatype_nsec) {
		/*%
		 * The node is empty of everything but NSEC / RRSIG records.
		 */
		DNS_RDATASETITER_FOREACH (rdsiter) {
			dns_rdatasetiter_current(rdsiter, &rdataset);
			result = dns_db_deleterdataset(gdb, node, gversion,
						       rdataset.type,
						       rdataset.covers);
			check_result(result, "dns_db_deleterdataset()");
			dns_rdataset_disassociate(&rdataset);
		}
	} else {
		/*
		 * Delete RRSIGs for types that no longer exist.
		 */
		result = dns_db_allrdatasets(gdb, node, gversion, 0, 0,
					     &rdsiter2);
		check_result(result, "dns_db_allrdatasets()");
		DNS_RDATASETITER_FOREACH (rdsiter) {
			dns_rdatasetiter_current(rdsiter, &rdataset);
			type = rdataset.type;
			covers = rdataset.covers;
			dns_rdataset_disassociate(&rdataset);
			/*
			 * Delete the NSEC chain if we are signing with
			 * NSEC3.
			 */
			if (nsec_datatype == dns_rdatatype_nsec3 &&
			    (type == dns_rdatatype_nsec ||
			     covers == dns_rdatatype_nsec))
			{
				result = dns_db_deleterdataset(
					gdb, node, gversion, type, covers);
				check_result(result, "dns_db_deleterdataset("
						     "nsec/rrsig)");
				continue;
			}
			if (type != dns_rdatatype_rrsig) {
				continue;
			}
			found = false;
			DNS_RDATASETITER_FOREACH (rdsiter2) {
				dns_rdatasetiter_current(rdsiter2, &rdataset);
				if (rdataset.type == covers) {
					found = true;
				}
				dns_rdataset_disassociate(&rdataset);
			}
			if (!found) {
				result = dns_db_deleterdataset(
					gdb, node, gversion, type, covers);
				check_result(result, "dns_db_deleterdataset("
						     "rrsig)");
			} else if (result != ISC_R_SUCCESS) {
				fatal("rdataset iteration failed: %s",
				      isc_result_totext(result));
			}
		}
		dns_rdatasetiter_destroy(&rdsiter2);
	}
	dns_rdatasetiter_destroy(&rdsiter);

	return active;
}

/*%
 * Extracts the minimum TTL from the SOA record, and the SOA record's TTL.
 */
static void
get_soa_ttls(void) {
	dns_rdataset_t soaset;
	dns_fixedname_t fname;
	dns_name_t *name;
	isc_result_t result;
	dns_rdata_t rdata = DNS_RDATA_INIT;

	name = dns_fixedname_initname(&fname);
	dns_rdataset_init(&soaset);
	result = dns_db_find(gdb, gorigin, gversion, dns_rdatatype_soa, 0, 0,
			     NULL, name, &soaset, NULL);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to find an SOA at the zone apex: %s",
		      isc_result_totext(result));
	}

	result = dns_rdataset_first(&soaset);
	check_result(result, "dns_rdataset_first");
	dns_rdataset_current(&soaset, &rdata);
	soa_ttl = soaset.ttl;
	zone_soa_min_ttl = ISC_MIN(dns_soa_getminimum(&rdata), soa_ttl);
	if (set_maxttl) {
		zone_soa_min_ttl = ISC_MIN(zone_soa_min_ttl, maxttl);
		soa_ttl = ISC_MIN(soa_ttl, maxttl);
	}
	dns_rdataset_disassociate(&soaset);
}

/*%
 * Increment (or set if nonzero) the SOA serial
 */
static isc_result_t
setsoaserial(uint32_t serial, dns_updatemethod_t method) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	uint32_t old_serial, new_serial = 0;
	dns_updatemethod_t used = dns_updatemethod_none;

	result = dns_db_getoriginnode(gdb, &node);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	dns_rdataset_init(&rdataset);

	result = dns_db_findrdataset(gdb, node, gversion, dns_rdatatype_soa, 0,
				     0, &rdataset, NULL);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	result = dns_rdataset_first(&rdataset);
	RUNTIME_CHECK(result == ISC_R_SUCCESS);

	dns_rdataset_current(&rdataset, &rdata);

	old_serial = dns_soa_getserial(&rdata);

	if (method == dns_updatemethod_date ||
	    method == dns_updatemethod_unixtime)
	{
		new_serial = dns_update_soaserial(old_serial, method, &used);
	} else if (serial != 0 || method == dns_updatemethod_none) {
		/* Set SOA serial to the value provided. */
		new_serial = serial;
		used = method;
	} else {
		new_serial = dns_update_soaserial(old_serial, method, &used);
	}

	if (method != used) {
		fprintf(stderr,
			"%s: warning: Serial number would not advance, "
			"using increment method instead\n",
			isc_commandline_progname);
	}

	/* If the new serial is not likely to cause a zone transfer
	 * (a/ixfr) from servers having the old serial, warn the user.
	 *
	 * RFC1982 section 7 defines the maximum increment to be
	 * (2^(32-1))-1.  Using u_int32_t arithmetic, we can do a single
	 * comparison.  (5 - 6 == (2^32)-1, not negative-one)
	 */
	if (new_serial == old_serial || (new_serial - old_serial) > 0x7fffffffU)
	{
		fprintf(stderr,
			"%s: warning: Serial number not advanced, "
			"zone may not transfer\n",
			isc_commandline_progname);
	}

	dns_soa_setserial(new_serial, &rdata);

	result = dns_db_deleterdataset(gdb, node, gversion, dns_rdatatype_soa,
				       0);
	check_result(result, "dns_db_deleterdataset");
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	result = dns_db_addrdataset(gdb, node, gversion, 0, &rdataset, 0, NULL);
	check_result(result, "dns_db_addrdataset");
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

cleanup:
	dns_rdataset_disassociate(&rdataset);
	if (node != NULL) {
		dns_db_detachnode(gdb, &node);
	}
	dns_rdata_reset(&rdata);

	return result;
}

/*%
 * Set up the iterator and global state before starting the tasks.
 */
static void
presign(void) {
	isc_result_t result;

	gdbiter = NULL;
	result = dns_db_createiterator(gdb, 0, &gdbiter);
	check_result(result, "dns_db_createiterator()");
}

/*%
 * Clean up the iterator and global state after the tasks complete.
 */
static void
postsign(void) {
	dns_dbiterator_destroy(&gdbiter);
}

/*%
 * Sign the apex of the zone.
 * Note the origin may not be the first node if there are out of zone
 * records.
 */
static void
signapex(void) {
	dns_dbnode_t *node = NULL;
	dns_fixedname_t fixed;
	dns_name_t *name;
	isc_result_t result;

	name = dns_fixedname_initname(&fixed);
	result = dns_dbiterator_seek(gdbiter, gorigin);
	check_result(result, "dns_dbiterator_seek()");
	result = dns_dbiterator_current(gdbiter, &node, name);
	check_dns_dbiterator_current(result);
	signname(node, true, name);
	dumpnode(name, node);
	dns_db_detachnode(gdb, &node);
	result = dns_dbiterator_first(gdbiter);
	if (result == ISC_R_NOMORE) {
		atomic_store(&finished, true);
	} else if (result != ISC_R_SUCCESS) {
		fatal("failure iterating database: %s",
		      isc_result_totext(result));
	}
}

static void
abortwork(void *arg) {
	UNUSED(arg);

	atomic_store(&shuttingdown, true);
}

/*%
 * Assigns a node to a worker thread.  This is protected by the main task's
 * lock.
 */
static void
assignwork(void *arg) {
	dns_fixedname_t fname;
	dns_name_t *name = NULL;
	dns_dbnode_t *node = NULL;
	dns_rdataset_t nsec;
	bool found;
	isc_result_t result;
	static dns_name_t *zonecut = NULL; /* Protected by namelock. */
	static dns_fixedname_t fzonecut;   /* Protected by namelock. */
	static unsigned int ended = 0;	   /* Protected by namelock. */

	UNUSED(arg);

	if (atomic_load(&shuttingdown)) {
		return;
	}

	LOCK(&namelock);
	if (atomic_load(&finished)) {
		ended++;
		if (ended == nloops) {
			isc_loopmgr_shutdown();
		}
		UNLOCK(&namelock);
		return;
	}

	name = dns_fixedname_initname(&fname);
	node = NULL;
	found = false;
	while (!found) {
		result = dns_dbiterator_current(gdbiter, &node, name);
		check_dns_dbiterator_current(result);
		/*
		 * The origin was handled by signapex().
		 */
		if (dns_name_equal(name, gorigin)) {
			dns_db_detachnode(gdb, &node);
			goto next;
		}
		/*
		 * Sort the zone data from the glue and out-of-zone data.
		 * For NSEC zones nodes with zone data have NSEC records.
		 * For NSEC3 zones the NSEC3 nodes are zone data but
		 * outside of the zone name space.  For the rest we need
		 * to track the bottom of zone cuts.
		 * Nodes which don't need to be signed are dumped here.
		 */
		dns_rdataset_init(&nsec);
		result = dns_db_findrdataset(gdb, node, gversion, nsec_datatype,
					     0, 0, &nsec, NULL);
		if (dns_rdataset_isassociated(&nsec)) {
			dns_rdataset_disassociate(&nsec);
		}
		if (result == ISC_R_SUCCESS) {
			found = true;
		} else if (nsec_datatype == dns_rdatatype_nsec3) {
			if (dns_name_issubdomain(name, gorigin) &&
			    (zonecut == NULL ||
			     !dns_name_issubdomain(name, zonecut)))
			{
				if (is_delegation(gdb, gversion, gorigin, name,
						  node, NULL))
				{
					zonecut = savezonecut(&fzonecut, name);
					if (!OPTOUT(nsec3flags) ||
					    secure(name, node))
					{
						found = true;
					}
				} else if (has_dname(gdb, gversion, node)) {
					zonecut = savezonecut(&fzonecut, name);
					found = true;
				} else {
					found = true;
				}
			}
		}

		if (!found) {
			dumpnode(name, node);
			dns_db_detachnode(gdb, &node);
		}

	next:
		result = dns_dbiterator_next(gdbiter);
		if (result == ISC_R_NOMORE) {
			atomic_store(&finished, true);
			break;
		} else if (result != ISC_R_SUCCESS) {
			fatal("failure iterating database: %s",
			      isc_result_totext(result));
		}
	}
	if (!found) {
		ended++;
		if (ended == nloops) {
			isc_loopmgr_shutdown();
		}
		UNLOCK(&namelock);
		return;
	}

	UNLOCK(&namelock);

	signname(node, false, dns_fixedname_name(&fname));

	/*%
	 * Write a node to the output file, and restart the worker task.
	 */
	lock_and_dumpnode(dns_fixedname_name(&fname), node);
	dns_db_detachnode(gdb, &node);

	isc_async_current(assignwork, NULL);
}

/*%
 * Update / remove the DS RRset.  Preserve RRSIG(DS) if possible.
 */
static void
add_ds(dns_name_t *name, dns_dbnode_t *node, uint32_t nsttl) {
	dns_rdataset_t dsset;
	dns_rdataset_t sigdsset;
	isc_result_t result;

	dns_rdataset_init(&dsset);
	dns_rdataset_init(&sigdsset);
	result = dns_db_findrdataset(gdb, node, gversion, dns_rdatatype_ds, 0,
				     0, &dsset, &sigdsset);
	if (result == ISC_R_SUCCESS) {
		dns_rdataset_disassociate(&dsset);
		result = dns_db_deleterdataset(gdb, node, gversion,
					       dns_rdatatype_ds, 0);
		check_result(result, "dns_db_deleterdataset");
	}

	result = loadds(name, nsttl, &dsset);
	if (result == ISC_R_SUCCESS) {
		result = dns_db_addrdataset(gdb, node, gversion, 0, &dsset, 0,
					    NULL);
		check_result(result, "dns_db_addrdataset");
		dns_rdataset_disassociate(&dsset);
		if (dns_rdataset_isassociated(&sigdsset)) {
			dns_rdataset_disassociate(&sigdsset);
		}
	} else if (dns_rdataset_isassociated(&sigdsset)) {
		result = dns_db_deleterdataset(gdb, node, gversion,
					       dns_rdatatype_rrsig,
					       dns_rdatatype_ds);
		check_result(result, "dns_db_deleterdataset");
		dns_rdataset_disassociate(&sigdsset);
	}
}

/*
 * Remove records of the given type and their signatures.
 */
static void
remove_records(dns_dbnode_t *node, dns_rdatatype_t which, bool checknsec) {
	isc_result_t result;
	dns_rdatatype_t type, covers;
	dns_rdatasetiter_t *rdsiter = NULL;

	/*
	 * Delete any records of the given type at the apex.
	 */
	result = dns_db_allrdatasets(gdb, node, gversion, 0, 0, &rdsiter);
	check_result(result, "dns_db_allrdatasets()");
	DNS_RDATASETITER_FOREACH (rdsiter) {
		dns_rdataset_t rdataset = DNS_RDATASET_INIT;
		dns_rdatasetiter_current(rdsiter, &rdataset);
		type = rdataset.type;
		covers = rdataset.covers;
		dns_rdataset_disassociate(&rdataset);
		if (type == which || covers == which) {
			if (which == dns_rdatatype_nsec && checknsec &&
			    !update_chain)
			{
				fatal("Zone contains NSEC records.  Use -u "
				      "to update to NSEC3.");
			}
			if (which == dns_rdatatype_nsec3param && checknsec &&
			    !update_chain)
			{
				fatal("Zone contains NSEC3 chains.  Use -u "
				      "to update to NSEC.");
			}
			result = dns_db_deleterdataset(gdb, node, gversion,
						       type, covers);
			check_result(result, "dns_db_deleterdataset()");
		}
	}
	dns_rdatasetiter_destroy(&rdsiter);
}

/*
 * Remove signatures covering the given type.  If type == 0,
 * then remove all signatures, unless this is a delegation, in
 * which case remove all signatures except for DS or nsec_datatype
 */
static void
remove_sigs(dns_dbnode_t *node, bool delegation, dns_rdatatype_t which) {
	isc_result_t result;
	dns_rdatatype_t type, covers;
	dns_rdatasetiter_t *rdsiter = NULL;

	result = dns_db_allrdatasets(gdb, node, gversion, 0, 0, &rdsiter);
	check_result(result, "dns_db_allrdatasets()");
	DNS_RDATASETITER_FOREACH (rdsiter) {
		dns_rdataset_t rdataset = DNS_RDATASET_INIT;
		dns_rdatasetiter_current(rdsiter, &rdataset);
		type = rdataset.type;
		covers = rdataset.covers;
		dns_rdataset_disassociate(&rdataset);

		if (type != dns_rdatatype_rrsig) {
			continue;
		}

		if (which == 0 && delegation &&
		    (dns_rdatatype_atparent(covers) ||
		     (nsec_datatype == dns_rdatatype_nsec &&
		      covers == nsec_datatype)))
		{
			continue;
		}

		if (which != 0 && covers != which) {
			continue;
		}

		result = dns_db_deleterdataset(gdb, node, gversion, type,
					       covers);
		check_result(result, "dns_db_deleterdataset()");
	}
	dns_rdatasetiter_destroy(&rdsiter);
}

/*%
 * Generate NSEC records for the zone and remove NSEC3/NSEC3PARAM records.
 */
static void
nsecify(void) {
	dns_dbiterator_t *dbiter = NULL;
	dns_dbnode_t *node = NULL, *nextnode = NULL;
	dns_fixedname_t fname, fnextname, fzonecut;
	dns_name_t *name = dns_fixedname_initname(&fname);
	dns_name_t *nextname = dns_fixedname_initname(&fnextname);
	dns_name_t *zonecut = NULL;
	dns_rdatasetiter_t *rdsiter = NULL;
	dns_rdatatype_t type, covers;
	bool done = false;
	isc_result_t result;
	uint32_t nsttl = 0;

	/*
	 * Remove any NSEC3 chains.
	 */
	result = dns_db_createiterator(gdb, DNS_DB_NSEC3ONLY, &dbiter);
	check_result(result, "dns_db_createiterator()");
	DNS_DBITERATOR_FOREACH (dbiter) {
		dns_rdataset_t rdataset = DNS_RDATASET_INIT;
		result = dns_dbiterator_current(dbiter, &node, name);
		check_dns_dbiterator_current(result);
		result = dns_db_allrdatasets(gdb, node, gversion, 0, 0,
					     &rdsiter);
		check_result(result, "dns_db_allrdatasets()");
		DNS_RDATASETITER_FOREACH (rdsiter) {
			dns_rdatasetiter_current(rdsiter, &rdataset);
			type = rdataset.type;
			covers = rdataset.covers;
			dns_rdataset_disassociate(&rdataset);
			result = dns_db_deleterdataset(gdb, node, gversion,
						       type, covers);
			check_result(result, "dns_db_deleterdataset(nsec3param/"
					     "rrsig)");
		}
		dns_rdatasetiter_destroy(&rdsiter);
		dns_db_detachnode(gdb, &node);
	}
	dns_dbiterator_destroy(&dbiter);

	result = dns_db_createiterator(gdb, DNS_DB_NONSEC3, &dbiter);
	check_result(result, "dns_db_createiterator()");

	result = dns_dbiterator_first(dbiter);
	check_result(result, "dns_dbiterator_first()");

	while (!done) {
		result = dns_dbiterator_current(dbiter, &node, name);
		check_dns_dbiterator_current(result);
		/*
		 * Skip out-of-zone records.
		 */
		if (!dns_name_issubdomain(name, gorigin)) {
			result = dns_dbiterator_next(dbiter);
			if (result == ISC_R_NOMORE) {
				done = true;
			} else {
				check_result(result, "dns_dbiterator_next()");
			}
			dns_db_detachnode(gdb, &node);
			continue;
		}

		if (dns_name_equal(name, gorigin)) {
			remove_records(node, dns_rdatatype_nsec3param, true);
			/* Clean old rrsigs at apex. */
			(void)active_node(node);
		}

		if (is_delegation(gdb, gversion, gorigin, name, node, &nsttl)) {
			zonecut = savezonecut(&fzonecut, name);
			remove_sigs(node, true, 0);
			if (generateds) {
				add_ds(name, node, nsttl);
			}
		} else if (has_dname(gdb, gversion, node)) {
			zonecut = savezonecut(&fzonecut, name);
		}

		result = dns_dbiterator_next(dbiter);
		nextnode = NULL;
		while (result == ISC_R_SUCCESS) {
			bool active = false;
			result = dns_dbiterator_current(dbiter, &nextnode,
							nextname);
			check_dns_dbiterator_current(result);
			active = active_node(nextnode);
			if (!active) {
				dns_db_detachnode(gdb, &nextnode);
				result = dns_dbiterator_next(dbiter);
				continue;
			}
			if (!dns_name_issubdomain(nextname, gorigin) ||
			    (zonecut != NULL &&
			     dns_name_issubdomain(nextname, zonecut)))
			{
				remove_sigs(nextnode, false, 0);
				remove_records(nextnode, dns_rdatatype_nsec,
					       false);
				dns_db_detachnode(gdb, &nextnode);
				result = dns_dbiterator_next(dbiter);
				continue;
			}
			dns_db_detachnode(gdb, &nextnode);
			break;
		}
		if (result == ISC_R_NOMORE) {
			dns_name_clone(gorigin, nextname);
			done = true;
		} else if (result != ISC_R_SUCCESS) {
			fatal("iterating through the database failed: %s",
			      isc_result_totext(result));
		}
		dns_dbiterator_pause(dbiter);
		result = dns_nsec_build(gdb, gversion, node, nextname,
					zone_soa_min_ttl);
		check_result(result, "dns_nsec_build()");
		dns_db_detachnode(gdb, &node);
	}

	dns_dbiterator_destroy(&dbiter);
}

static void
addnsec3param(const unsigned char *salt, size_t salt_len,
	      dns_iterations_t iterations) {
	dns_dbnode_t *node = NULL;
	dns_rdata_nsec3param_t nsec3param;
	unsigned char nsec3parambuf[5 + 255];
	dns_rdatalist_t rdatalist;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	isc_buffer_t b;
	isc_result_t result;

	dns_rdataset_init(&rdataset);

	nsec3param.common.rdclass = gclass;
	nsec3param.common.rdtype = dns_rdatatype_nsec3param;
	nsec3param.mctx = NULL;
	nsec3param.flags = 0;
	nsec3param.hash = unknownalg ? DNS_NSEC3_UNKNOWNALG : dns_hash_sha1;
	nsec3param.iterations = iterations;
	nsec3param.salt_length = (unsigned char)salt_len;
	nsec3param.salt = UNCONST(salt);

	isc_buffer_init(&b, nsec3parambuf, sizeof(nsec3parambuf));
	result = dns_rdata_fromstruct(&rdata, gclass, dns_rdatatype_nsec3param,
				      &nsec3param, &b);
	check_result(result, "dns_rdata_fromstruct()");
	dns_rdatalist_init(&rdatalist);
	rdatalist.rdclass = rdata.rdclass;
	rdatalist.type = rdata.type;
	ISC_LIST_APPEND(rdatalist.rdata, &rdata, link);
	dns_rdatalist_tordataset(&rdatalist, &rdataset);

	result = dns_db_findnode(gdb, gorigin, true, &node);
	check_result(result, "dns_db_findnode(gorigin)");

	/*
	 * Delete any current NSEC3PARAM records.
	 */
	result = dns_db_deleterdataset(gdb, node, gversion,
				       dns_rdatatype_nsec3param, 0);
	if (result == DNS_R_UNCHANGED) {
		result = ISC_R_SUCCESS;
	}
	check_result(result, "dddnsec3param: dns_db_deleterdataset()");

	result = dns_db_addrdataset(gdb, node, gversion, 0, &rdataset,
				    DNS_DBADD_MERGE, NULL);
	if (result == DNS_R_UNCHANGED) {
		result = ISC_R_SUCCESS;
	}
	check_result(result, "addnsec3param: dns_db_addrdataset()");
	dns_db_detachnode(gdb, &node);
}

static void
addnsec3(dns_name_t *name, dns_dbnode_t *node, const unsigned char *salt,
	 size_t salt_len, unsigned int iterations, hashlist_t *hashlist,
	 dns_ttl_t ttl) {
	unsigned char hash[NSEC3_MAX_HASH_LENGTH];
	const unsigned char *nexthash;
	unsigned char nsec3buffer[DNS_NSEC3_BUFFERSIZE];
	dns_fixedname_t hashname;
	dns_rdatalist_t rdatalist;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	isc_result_t result;
	dns_dbnode_t *nsec3node = NULL;
	char namebuf[DNS_NAME_FORMATSIZE];
	size_t hash_len;

	dns_name_format(name, namebuf, sizeof(namebuf));

	dns_fixedname_init(&hashname);
	dns_rdataset_init(&rdataset);

	dns_name_downcase(name, name);
	result = dns_nsec3_hashname(&hashname, hash, &hash_len, name, gorigin,
				    dns_hash_sha1, iterations, salt, salt_len);
	check_result(result, "addnsec3: dns_nsec3_hashname()");
	nexthash = hashlist_findnext(hashlist, hash);
	result = dns_nsec3_buildrdata(
		gdb, gversion, node,
		unknownalg ? DNS_NSEC3_UNKNOWNALG : dns_hash_sha1, nsec3flags,
		iterations, salt, salt_len, nexthash, ISC_SHA1_DIGESTLENGTH,
		nsec3buffer, &rdata);
	check_result(result, "addnsec3: dns_nsec3_buildrdata()");
	dns_rdatalist_init(&rdatalist);
	rdatalist.rdclass = rdata.rdclass;
	rdatalist.type = rdata.type;
	rdatalist.ttl = ttl;
	ISC_LIST_APPEND(rdatalist.rdata, &rdata, link);
	dns_rdatalist_tordataset(&rdatalist, &rdataset);
	result = dns_db_findnsec3node(gdb, dns_fixedname_name(&hashname), true,
				      &nsec3node);
	check_result(result, "addnsec3: dns_db_findnode()");
	result = dns_db_addrdataset(gdb, nsec3node, gversion, 0, &rdataset, 0,
				    NULL);
	if (result == DNS_R_UNCHANGED) {
		result = ISC_R_SUCCESS;
	}
	check_result(result, "addnsec3: dns_db_addrdataset()");
	dns_db_detachnode(gdb, &nsec3node);
}

/*%
 * Clean out NSEC3 record and RRSIG(NSEC3) that are not in the hash list.
 *
 * Extract the hash from the first label of 'name' then see if it
 * is in hashlist.  If 'name' is not in the hashlist then delete the
 * any NSEC3 records which have the same parameters as the chain we
 * are building.
 *
 * XXXMPA Should we also check that it of the form &lt;hash&gt;.&lt;origin&gt;?
 */
static void
nsec3clean(dns_name_t *name, dns_dbnode_t *node, unsigned int hashalg,
	   unsigned int iterations, const unsigned char *salt, size_t salt_len,
	   hashlist_t *hashlist) {
	dns_label_t label;
	dns_rdata_nsec3_t nsec3;
	dns_rdatalist_t rdatalist;
	dns_rdataset_t rdataset, delrdataset;
	bool delete_rrsigs = false;
	isc_buffer_t target;
	isc_result_t result;
	unsigned char hash[NSEC3_MAX_HASH_LENGTH + 1];
	bool exists;

	/*
	 * Get the first label.
	 */
	dns_name_getlabel(name, 0, &label);

	/*
	 * We want just the label contents.
	 */
	isc_region_consume(&label, 1);

	/*
	 * Decode base32hex string.
	 */
	isc_buffer_init(&target, hash, sizeof(hash) - 1);
	result = isc_base32hex_decoderegion(&label, &target);
	if (result != ISC_R_SUCCESS) {
		return;
	}

	hash[isc_buffer_usedlength(&target)] = 0;

	exists = hashlist_exists(hashlist, hash);

	/*
	 * Verify that the NSEC3 parameters match the current ones
	 * otherwise we are dealing with a different NSEC3 chain.
	 */
	dns_rdataset_init(&rdataset);
	dns_rdataset_init(&delrdataset);

	result = dns_db_findrdataset(gdb, node, gversion, dns_rdatatype_nsec3,
				     0, 0, &rdataset, NULL);
	if (result != ISC_R_SUCCESS) {
		return;
	}

	/*
	 * Delete any NSEC3 records which are not part of the current
	 * NSEC3 chain.
	 */
	DNS_RDATASET_FOREACH (&rdataset) {
		dns_rdata_t rdata = DNS_RDATA_INIT;
		dns_rdata_t delrdata = DNS_RDATA_INIT;

		dns_rdataset_current(&rdataset, &rdata);
		result = dns_rdata_tostruct(&rdata, &nsec3, NULL);
		check_result(result, "dns_rdata_tostruct");
		if (exists && nsec3.hash == hashalg &&
		    nsec3.iterations == iterations &&
		    nsec3.salt_length == salt_len &&
		    isc_safe_memequal(nsec3.salt, salt, salt_len))
		{
			continue;
		}
		dns_rdatalist_init(&rdatalist);
		rdatalist.rdclass = rdata.rdclass;
		rdatalist.type = rdata.type;
		if (set_maxttl) {
			rdatalist.ttl = ISC_MIN(rdataset.ttl, maxttl);
		}

		dns_rdata_clone(&rdata, &delrdata);
		ISC_LIST_APPEND(rdatalist.rdata, &delrdata, link);
		dns_rdatalist_tordataset(&rdatalist, &delrdataset);
		result = dns_db_subtractrdataset(gdb, node, gversion,
						 &delrdataset, 0, NULL);
		dns_rdataset_disassociate(&delrdataset);
		if (result != ISC_R_SUCCESS && result != DNS_R_NXRRSET) {
			check_result(result, "dns_db_subtractrdataset(NSEC3)");
		}
		delete_rrsigs = true;
	}
	dns_rdataset_disassociate(&rdataset);

	if (!delete_rrsigs) {
		return;
	}
	/*
	 * Delete the NSEC3 RRSIGs
	 */
	result = dns_db_deleterdataset(gdb, node, gversion, dns_rdatatype_rrsig,
				       dns_rdatatype_nsec3);
	if (result != ISC_R_SUCCESS && result != DNS_R_UNCHANGED) {
		check_result(result, "dns_db_deleterdataset(RRSIG(NSEC3))");
	}
}

static void
rrset_cleanup(dns_name_t *name, dns_rdataset_t *rdataset, dns_diff_t *add,
	      dns_diff_t *del) {
	unsigned int count1 = 0;
	dns_rdataset_t tmprdataset;
	char namestr[DNS_NAME_FORMATSIZE];
	char typestr[DNS_RDATATYPE_FORMATSIZE];

	dns_name_format(name, namestr, sizeof(namestr));
	dns_rdatatype_format(rdataset->type, typestr, sizeof(typestr));

	dns_rdataset_init(&tmprdataset);
	DNS_RDATASET_FOREACH (rdataset) {
		dns_rdata_t rdata1 = DNS_RDATA_INIT;
		unsigned int count2 = 0;

		count1++;
		dns_rdataset_current(rdataset, &rdata1);
		dns_rdataset_clone(rdataset, &tmprdataset);
		DNS_RDATASET_FOREACH (&tmprdataset) {
			dns_rdata_t rdata2 = DNS_RDATA_INIT;
			dns_difftuple_t *tuple = NULL;
			count2++;
			dns_rdataset_current(&tmprdataset, &rdata2);
			if (count1 < count2 &&
			    dns_rdata_casecompare(&rdata1, &rdata2) == 0)
			{
				vbprintf(2, "removing duplicate at %s/%s\n",
					 namestr, typestr);
				dns_difftuple_create(mctx, DNS_DIFFOP_DELRESIGN,
						     name, rdataset->ttl,
						     &rdata2, &tuple);
				dns_diff_append(del, &tuple);
			} else if (set_maxttl && rdataset->ttl > maxttl) {
				vbprintf(2,
					 "reducing ttl of %s/%s "
					 "from %d to %d\n",
					 namestr, typestr, rdataset->ttl,
					 maxttl);
				dns_difftuple_create(mctx, DNS_DIFFOP_DELRESIGN,
						     name, rdataset->ttl,
						     &rdata2, &tuple);
				dns_diff_append(del, &tuple);
				tuple = NULL;
				dns_difftuple_create(mctx, DNS_DIFFOP_ADDRESIGN,
						     name, maxttl, &rdata2,
						     &tuple);
				dns_diff_append(add, &tuple);
			}
		}
		dns_rdataset_disassociate(&tmprdataset);
	}
}

static void
cleanup_zone(void) {
	isc_result_t result;
	dns_dbiterator_t *dbiter = NULL;
	dns_rdatasetiter_t *rdsiter = NULL;
	dns_diff_t add, del;
	dns_dbnode_t *node = NULL;
	dns_fixedname_t fname;
	dns_name_t *name = dns_fixedname_initname(&fname);

	dns_diff_init(mctx, &add);
	dns_diff_init(mctx, &del);

	result = dns_db_createiterator(gdb, 0, &dbiter);
	check_result(result, "dns_db_createiterator()");

	DNS_DBITERATOR_FOREACH (dbiter) {
		dns_rdataset_t rdataset = DNS_RDATASET_INIT;
		result = dns_dbiterator_current(dbiter, &node, name);
		check_dns_dbiterator_current(result);
		result = dns_db_allrdatasets(gdb, node, gversion, 0, 0,
					     &rdsiter);
		check_result(result, "dns_db_allrdatasets()");
		DNS_RDATASETITER_FOREACH (rdsiter) {
			dns_rdatasetiter_current(rdsiter, &rdataset);
			rrset_cleanup(name, &rdataset, &add, &del);
			dns_rdataset_disassociate(&rdataset);
		}
		dns_rdatasetiter_destroy(&rdsiter);
		dns_db_detachnode(gdb, &node);
	}

	result = dns_diff_applysilently(&del, gdb, gversion);
	check_result(result, "dns_diff_applysilently");

	result = dns_diff_applysilently(&add, gdb, gversion);
	check_result(result, "dns_diff_applysilently");

	dns_diff_clear(&del);
	dns_diff_clear(&add);
	dns_dbiterator_destroy(&dbiter);
}

/*
 * Generate NSEC3 records for the zone.
 */
static void
nsec3ify(unsigned int hashalg, dns_iterations_t iterations,
	 const unsigned char *salt, size_t salt_len, hashlist_t *hashlist) {
	dns_dbiterator_t *dbiter = NULL;
	dns_dbnode_t *node = NULL, *nextnode = NULL;
	dns_fixedname_t fname, fnextname, fzonecut;
	dns_name_t *name, *nextname, *zonecut;
	dns_rdataset_t rdataset;
	int order;
	bool active;
	bool done = false;
	isc_result_t result;
	uint32_t nsttl = 0;
	unsigned int count, nlabels;

	dns_rdataset_init(&rdataset);
	name = dns_fixedname_initname(&fname);
	nextname = dns_fixedname_initname(&fnextname);
	zonecut = NULL;

	/*
	 * Walk the zone generating the hash names.
	 */
	result = dns_db_createiterator(gdb, DNS_DB_NONSEC3, &dbiter);
	check_result(result, "dns_db_createiterator()");

	result = dns_dbiterator_first(dbiter);
	check_result(result, "dns_dbiterator_first()");

	while (!done) {
		result = dns_dbiterator_current(dbiter, &node, name);
		check_dns_dbiterator_current(result);
		/*
		 * Skip out-of-zone records.
		 */
		if (!dns_name_issubdomain(name, gorigin)) {
			result = dns_dbiterator_next(dbiter);
			if (result == ISC_R_NOMORE) {
				done = true;
			} else {
				check_result(result, "dns_dbiterator_next()");
			}
			dns_db_detachnode(gdb, &node);
			continue;
		}

		if (dns_name_equal(name, gorigin)) {
			remove_records(node, dns_rdatatype_nsec, true);
			/* Clean old rrsigs at apex. */
			(void)active_node(node);
		}

		if (has_dname(gdb, gversion, node)) {
			zonecut = savezonecut(&fzonecut, name);
		}

		result = dns_dbiterator_next(dbiter);
		nextnode = NULL;
		while (result == ISC_R_SUCCESS) {
			result = dns_dbiterator_current(dbiter, &nextnode,
							nextname);
			check_dns_dbiterator_current(result);
			active = active_node(nextnode);
			if (!active) {
				dns_db_detachnode(gdb, &nextnode);
				result = dns_dbiterator_next(dbiter);
				continue;
			}
			if (!dns_name_issubdomain(nextname, gorigin) ||
			    (zonecut != NULL &&
			     dns_name_issubdomain(nextname, zonecut)))
			{
				remove_sigs(nextnode, false, 0);
				dns_db_detachnode(gdb, &nextnode);
				result = dns_dbiterator_next(dbiter);
				continue;
			}
			if (is_delegation(gdb, gversion, gorigin, nextname,
					  nextnode, &nsttl))
			{
				zonecut = savezonecut(&fzonecut, nextname);
				remove_sigs(nextnode, true, 0);
				if (generateds) {
					add_ds(nextname, nextnode, nsttl);
				}
				if (OPTOUT(nsec3flags) &&
				    !secure(nextname, nextnode))
				{
					dns_db_detachnode(gdb, &nextnode);
					result = dns_dbiterator_next(dbiter);
					continue;
				}
			} else if (has_dname(gdb, gversion, nextnode)) {
				zonecut = savezonecut(&fzonecut, nextname);
			}
			dns_db_detachnode(gdb, &nextnode);
			break;
		}
		if (result == ISC_R_NOMORE) {
			dns_name_copy(gorigin, nextname);
			done = true;
		} else if (result != ISC_R_SUCCESS) {
			fatal("iterating through the database failed: %s",
			      isc_result_totext(result));
		}
		dns_name_downcase(name, name);
		hashlist_add_dns_name(hashlist, name, hashalg, iterations, salt,
				      salt_len, false);
		dns_db_detachnode(gdb, &node);
		/*
		 * Add hashes for empty nodes.  Use closest encloser logic.
		 * The closest encloser either has data or is a empty
		 * node for another <name,nextname> span so we don't add
		 * it here.  Empty labels on nextname are within the span.
		 */
		dns_name_downcase(nextname, nextname);
		dns_name_fullcompare(name, nextname, &order, &nlabels);
		addnowildcardhash(hashlist, name, hashalg, iterations, salt,
				  salt_len);
		count = dns_name_countlabels(nextname);
		while (count > nlabels + 1) {
			count--;
			dns_name_split(nextname, count, NULL, nextname);
			hashlist_add_dns_name(hashlist, nextname, hashalg,
					      iterations, salt, salt_len,
					      false);
			addnowildcardhash(hashlist, nextname, hashalg,
					  iterations, salt, salt_len);
		}
	}
	dns_dbiterator_destroy(&dbiter);

	/*
	 * We have all the hashes now so we can sort them.
	 */
	hashlist_sort(hashlist);

	/*
	 * Check for duplicate hashes.  If found the salt needs to
	 * be changed.
	 */
	if (hashlist_hasdup(hashlist)) {
		fatal("Duplicate hash detected. Pick a different salt.");
	}

	/*
	 * Generate the nsec3 records.
	 */
	zonecut = NULL;
	done = false;

	addnsec3param(salt, salt_len, iterations);

	/*
	 * Clean out NSEC3 records which don't match this chain.
	 */
	result = dns_db_createiterator(gdb, DNS_DB_NSEC3ONLY, &dbiter);
	check_result(result, "dns_db_createiterator()");

	DNS_DBITERATOR_FOREACH (dbiter) {
		result = dns_dbiterator_current(dbiter, &node, name);
		check_dns_dbiterator_current(result);
		nsec3clean(name, node, hashalg, iterations, salt, salt_len,
			   hashlist);
		dns_db_detachnode(gdb, &node);
	}
	dns_dbiterator_destroy(&dbiter);

	/*
	 * Generate / complete the new chain.
	 */
	result = dns_db_createiterator(gdb, DNS_DB_NONSEC3, &dbiter);
	check_result(result, "dns_db_createiterator()");

	result = dns_dbiterator_first(dbiter);
	check_result(result, "dns_dbiterator_first()");

	while (!done) {
		result = dns_dbiterator_current(dbiter, &node, name);
		check_dns_dbiterator_current(result);
		/*
		 * Skip out-of-zone records.
		 */
		if (!dns_name_issubdomain(name, gorigin)) {
			result = dns_dbiterator_next(dbiter);
			if (result == ISC_R_NOMORE) {
				done = true;
			} else {
				check_result(result, "dns_dbiterator_next()");
			}
			dns_db_detachnode(gdb, &node);
			continue;
		}

		if (has_dname(gdb, gversion, node)) {
			zonecut = savezonecut(&fzonecut, name);
		}

		result = dns_dbiterator_next(dbiter);
		nextnode = NULL;
		while (result == ISC_R_SUCCESS) {
			result = dns_dbiterator_current(dbiter, &nextnode,
							nextname);
			check_dns_dbiterator_current(result);
			active = active_node(nextnode);
			if (!active) {
				dns_db_detachnode(gdb, &nextnode);
				result = dns_dbiterator_next(dbiter);
				continue;
			}
			if (!dns_name_issubdomain(nextname, gorigin) ||
			    (zonecut != NULL &&
			     dns_name_issubdomain(nextname, zonecut)))
			{
				dns_db_detachnode(gdb, &nextnode);
				result = dns_dbiterator_next(dbiter);
				continue;
			}
			if (is_delegation(gdb, gversion, gorigin, nextname,
					  nextnode, NULL))
			{
				zonecut = savezonecut(&fzonecut, nextname);
				if (OPTOUT(nsec3flags) &&
				    !secure(nextname, nextnode))
				{
					dns_db_detachnode(gdb, &nextnode);
					result = dns_dbiterator_next(dbiter);
					continue;
				}
			} else if (has_dname(gdb, gversion, nextnode)) {
				zonecut = savezonecut(&fzonecut, nextname);
			}
			dns_db_detachnode(gdb, &nextnode);
			break;
		}
		if (result == ISC_R_NOMORE) {
			dns_name_copy(gorigin, nextname);
			done = true;
		} else if (result != ISC_R_SUCCESS) {
			fatal("iterating through the database failed: %s",
			      isc_result_totext(result));
		}
		/*
		 * We need to pause here to release the lock on the database.
		 */
		dns_dbiterator_pause(dbiter);
		addnsec3(name, node, salt, salt_len, iterations, hashlist,
			 zone_soa_min_ttl);
		dns_db_detachnode(gdb, &node);
		/*
		 * Add NSEC3's for empty nodes.  Use closest encloser logic.
		 */
		dns_name_fullcompare(name, nextname, &order, &nlabels);
		count = dns_name_countlabels(nextname);
		while (count > nlabels + 1) {
			count--;
			dns_name_split(nextname, count, NULL, nextname);
			addnsec3(nextname, NULL, salt, salt_len, iterations,
				 hashlist, zone_soa_min_ttl);
		}
	}
	dns_dbiterator_destroy(&dbiter);
}

/*%
 * Load the zone file from disk
 */
static void
loadzone(char *file, char *origin, dns_rdataclass_t rdclass, dns_db_t **db) {
	isc_buffer_t b;
	int len;
	dns_fixedname_t fname;
	dns_name_t *name;
	isc_result_t result;

	len = strlen(origin);
	isc_buffer_init(&b, origin, len);
	isc_buffer_add(&b, len);

	name = dns_fixedname_initname(&fname);
	result = dns_name_fromtext(name, &b, dns_rootname, 0);
	if (result != ISC_R_SUCCESS) {
		fatal("failed converting name '%s' to dns format: %s", origin,
		      isc_result_totext(result));
	}

	result = dns_db_create(mctx, ZONEDB_DEFAULT, name, dns_dbtype_zone,
			       rdclass, 0, NULL, db);
	check_result(result, "dns_db_create()");

	result = dns_db_load(*db, file, inputformat, 0);
	if (result != ISC_R_SUCCESS && result != DNS_R_SEENINCLUDE) {
		fatal("failed loading zone from '%s': %s", file,
		      isc_result_totext(result));
	}
}

/*%
 * Finds all public zone keys in the zone, and attempts to load the
 * private keys from disk.
 */
static void
loadzonekeys(bool preserve_keys, bool load_public) {
	dns_dbnode_t *node;
	dns_dbversion_t *currentversion = NULL;
	isc_result_t result;
	dns_rdataset_t rdataset, keysigs, soasigs;

	node = NULL;
	result = dns_db_findnode(gdb, gorigin, false, &node);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to find the zone's origin: %s",
		      isc_result_totext(result));
	}

	dns_db_currentversion(gdb, &currentversion);

	dns_rdataset_init(&rdataset);
	dns_rdataset_init(&soasigs);
	dns_rdataset_init(&keysigs);

	/* Make note of the keys which signed the SOA, if any */
	result = dns_db_findrdataset(gdb, node, currentversion,
				     dns_rdatatype_soa, 0, 0, &rdataset,
				     &soasigs);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	/* Preserve the TTL of the DNSKEY RRset, if any */
	dns_rdataset_disassociate(&rdataset);
	result = dns_db_findrdataset(gdb, node, currentversion,
				     dns_rdatatype_dnskey, 0, 0, &rdataset,
				     &keysigs);

	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	if (set_keyttl && keyttl != rdataset.ttl) {
		fprintf(stderr,
			"User-specified TTL %u conflicts "
			"with existing DNSKEY RRset TTL.\n",
			keyttl);
		fprintf(stderr,
			"Imported keys will use the RRSet "
			"TTL %u instead.\n",
			rdataset.ttl);
	}
	keyttl = rdataset.ttl;

	/* Load keys corresponding to the existing DNSKEY RRset. */
	result = dns_dnssec_keylistfromrdataset(
		gorigin, NULL, directory, mctx, &rdataset, &keysigs, &soasigs,
		preserve_keys, load_public, &keylist);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to load the zone keys: %s",
		      isc_result_totext(result));
	}

cleanup:
	if (dns_rdataset_isassociated(&rdataset)) {
		dns_rdataset_disassociate(&rdataset);
	}
	if (dns_rdataset_isassociated(&keysigs)) {
		dns_rdataset_disassociate(&keysigs);
	}
	if (dns_rdataset_isassociated(&soasigs)) {
		dns_rdataset_disassociate(&soasigs);
	}
	dns_db_detachnode(gdb, &node);
	dns_db_closeversion(gdb, &currentversion, false);
}

static void
loadexplicitkeys(char *keyfiles[], int n, bool setksk) {
	isc_result_t result;
	int i;

	for (i = 0; i < n; i++) {
		dns_dnsseckey_t *key = NULL;
		dst_key_t *newkey = NULL;

		result = dst_key_fromnamedfile(
			keyfiles[i], directory,
			DST_TYPE_PUBLIC | DST_TYPE_PRIVATE, mctx, &newkey);
		if (result != ISC_R_SUCCESS) {
			fatal("cannot load dnskey %s: %s", keyfiles[i],
			      isc_result_totext(result));
		}

		if (!dns_name_equal(gorigin, dst_key_name(newkey))) {
			fatal("key %s not at origin\n", keyfiles[i]);
		}

		if (!dst_key_isprivate(newkey)) {
			fatal("cannot sign zone with non-private dnskey %s",
			      keyfiles[i]);
		}

		/* Skip any duplicates */
		ISC_LIST_FOREACH (keylist, k, link) {
			if (dst_key_id(k->key) == dst_key_id(newkey) &&
			    dst_key_alg(k->key) == dst_key_alg(newkey))
			{
				key = k;
				break;
			}
		}

		if (key == NULL) {
			/* We haven't seen this key before */
			dns_dnsseckey_create(mctx, &newkey, &key);
			ISC_LIST_APPEND(keylist, key, link);
			key->source = dns_keysource_user;
		} else {
			dst_key_free(&key->key);
			key->key = newkey;
		}

		key->force_publish = true;
		key->force_sign = true;

		if (setksk) {
			key->ksk = true;
		}
	}
}

static void
report(const char *format, ...) {
	if (!quiet) {
		FILE *out = output_stdout ? stderr : stdout;
		char buf[4096];
		va_list args;

		va_start(args, format);
		vsnprintf(buf, sizeof(buf), format, args);
		va_end(args);
		fprintf(out, "%s\n", buf);
	}
}

static void
clear_keylist(dns_dnsseckeylist_t *list) {
	ISC_LIST_FOREACH (*list, key, link) {
		ISC_LIST_UNLINK(*list, key, link);
		dns_dnsseckey_destroy(mctx, &key);
	}
}

static void
add_digest(char *str, size_t dlen, dns_kasp_digestlist_t *digests,
	   bool *cdnskey) {
	isc_result_t result;
	isc_textregion_t r;
	dns_dsdigest_t alg;
	dns_kasp_digest_t *digest;

	if (dlen == 7 && strncmp(str, "cdnskey", dlen) == 0) {
		*cdnskey = true;
		return;
	}

	if (dlen < 5 || strncmp(str, "cds:", 4) != 0) {
		fatal("digest must specify cds:algorithm ('%.*s')", (int)dlen,
		      str);
	}

	r.base = str + 4;
	r.length = dlen - 4;
	result = dns_dsdigest_fromtext(&alg, &r);
	if (result == DNS_R_UNKNOWN) {
		fatal("bad digest '%.*s'", (int)dlen, str);
	} else if (result != ISC_R_SUCCESS) {
		fatal("bad digest '%.*s': %s", (int)dlen, str,
		      isc_result_totext(result));
	} else if (!dst_ds_digest_supported(alg)) {
		fatal("unsupported digest '%.*s'", (int)dlen, str);
	}

	/* Suppress duplicates */
	ISC_LIST_FOREACH (*digests, d, link) {
		if (d->digest == alg) {
			return;
		}
	}

	digest = isc_mem_get(mctx, sizeof(*digest));
	digest->digest = alg;
	ISC_LINK_INIT(digest, link);
	ISC_LIST_APPEND(*digests, digest, link);
}

static void
build_final_keylist(void) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	dns_dbversion_t *ver = NULL;
	dns_diff_t diff;
	dns_dnsseckeylist_t rmkeys, matchkeys;
	char name[DNS_NAME_FORMATSIZE];
	dns_rdataset_t cdsset, cdnskeyset, soaset;
	dns_kasp_digestlist_t digests;
	bool cdnskey = false;

	ISC_LIST_INIT(rmkeys);
	ISC_LIST_INIT(matchkeys);
	ISC_LIST_INIT(digests);

	dns_rdataset_init(&soaset);
	dns_rdataset_init(&cdsset);
	dns_rdataset_init(&cdnskeyset);

	if (strlen(sync_records) > 0) {
		const char delim = ',';
		char *digest;
		char *s;
		size_t dlen;

		digest = UNCONST(sync_records);
	next_digest:
		s = strchr(digest, delim);
		if (s == NULL) {
			dlen = strlen(digest);
			add_digest(digest, dlen, &digests, &cdnskey);
			goto findkeys;
		}
		dlen = s - digest;
		add_digest(digest, dlen, &digests, &cdnskey);
		digest = s + 1;
		goto next_digest;
	}

findkeys:
	/*
	 * Find keys that match this zone in the key repository.
	 */
	result = dns_dnssec_findmatchingkeys(gorigin, NULL, directory, NULL,
					     now, mctx, &matchkeys);
	if (result == ISC_R_NOTFOUND) {
		result = ISC_R_SUCCESS;
	}
	check_result(result, "dns_dnssec_findmatchingkeys");

	result = dns_db_newversion(gdb, &ver);
	check_result(result, "dns_db_newversion");

	result = dns_db_getoriginnode(gdb, &node);
	check_result(result, "dns_db_getoriginnode");

	/* Get the CDS rdataset */
	result = dns_db_findrdataset(gdb, node, ver, dns_rdatatype_cds,
				     dns_rdatatype_none, 0, &cdsset, NULL);
	if (result != ISC_R_SUCCESS && dns_rdataset_isassociated(&cdsset)) {
		dns_rdataset_disassociate(&cdsset);
	}

	/* Get the CDNSKEY rdataset */
	result = dns_db_findrdataset(gdb, node, ver, dns_rdatatype_cdnskey,
				     dns_rdatatype_none, 0, &cdnskeyset, NULL);
	if (result != ISC_R_SUCCESS && dns_rdataset_isassociated(&cdnskeyset)) {
		dns_rdataset_disassociate(&cdnskeyset);
	}

	dns_diff_init(mctx, &diff);

	/*
	 * Update keylist with information from from the key repository.
	 */
	dns_dnssec_updatekeys(&keylist, &matchkeys, NULL, gorigin, keyttl,
			      &diff, mctx, report);

	/*
	 * Update keylist with sync records.
	 */

	dns_dnssec_syncupdate(&keylist, &rmkeys, &cdsset, &cdnskeyset, now,
			      &digests, cdnskey, keyttl, &diff, mctx);

	dns_name_format(gorigin, name, sizeof(name));

	result = dns_diff_applysilently(&diff, gdb, ver);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to update DNSKEY RRset at node '%s': %s", name,
		      isc_result_totext(result));
	}

	dns_db_detachnode(gdb, &node);
	dns_db_closeversion(gdb, &ver, true);

	dns_diff_clear(&diff);

	if (dns_rdataset_isassociated(&cdsset)) {
		dns_rdataset_disassociate(&cdsset);
	}
	if (dns_rdataset_isassociated(&cdnskeyset)) {
		dns_rdataset_disassociate(&cdnskeyset);
	}

	clear_keylist(&rmkeys);
	clear_keylist(&matchkeys);

	ISC_LIST_FOREACH (digests, d, link) {
		ISC_LIST_UNLINK(digests, d, link);
		isc_mem_put(mctx, d, sizeof(*d));
	}
	INSIST(ISC_LIST_EMPTY(digests));
}

static void
warnifallksk(dns_db_t *db) {
	dns_dbversion_t *currentversion = NULL;
	dns_dbnode_t *node = NULL;
	dns_rdataset_t rdataset;
	isc_result_t result;
	dns_rdata_dnskey_t dnskey;
	bool have_non_ksk = false;

	dns_db_currentversion(db, &currentversion);

	result = dns_db_findnode(db, gorigin, false, &node);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to find the zone's origin: %s",
		      isc_result_totext(result));
	}

	dns_rdataset_init(&rdataset);
	result = dns_db_findrdataset(db, node, currentversion,
				     dns_rdatatype_dnskey, 0, 0, &rdataset,
				     NULL);
	if (result != ISC_R_SUCCESS) {
		fatal("failed to find keys at the zone apex: %s",
		      isc_result_totext(result));
	}

	result = dns_rdataset_first(&rdataset);
	check_result(result, "dns_rdataset_first");

	DNS_RDATASET_FOREACH (&rdataset) {
		dns_rdata_t rdata = DNS_RDATA_INIT;
		dns_rdataset_current(&rdataset, &rdata);

		result = dns_rdata_tostruct(&rdata, &dnskey, NULL);
		check_result(result, "dns_rdata_tostruct");
		if ((dnskey.flags & DNS_KEYFLAG_KSK) == 0) {
			have_non_ksk = true;
			break;
		}
		dns_rdata_freestruct(&dnskey);
	}
	dns_rdataset_disassociate(&rdataset);
	dns_db_detachnode(db, &node);
	dns_db_closeversion(db, &currentversion, false);
	if (!have_non_ksk && !ignore_kskflag) {
		if (disable_zone_check) {
			fprintf(stderr,
				"%s: warning: No non-KSK DNSKEY found; "
				"supply a ZSK or use '-z'.\n",
				isc_commandline_progname);
		} else {
			fatal("No non-KSK DNSKEY found; "
			      "supply a ZSK or use '-z'.");
		}
	}
}

static void
set_nsec3params(bool update, bool set_salt, bool set_optout, bool set_iter) {
	isc_result_t result;
	dns_dbversion_t *ver = NULL;
	dns_dbnode_t *node = NULL;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_nsec3_t nsec3;
	dns_fixedname_t fname;
	dns_name_t *hashname;
	unsigned char orig_salt[255];
	size_t orig_saltlen;
	dns_hash_t orig_hash;
	uint16_t orig_iter;

	dns_db_currentversion(gdb, &ver);
	dns_rdataset_init(&rdataset);

	orig_saltlen = sizeof(orig_salt);
	result = dns_db_getnsec3parameters(gdb, ver, &orig_hash, NULL,
					   &orig_iter, orig_salt,
					   &orig_saltlen);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	nsec_datatype = dns_rdatatype_nsec3;

	if (!update && set_salt) {
		if (salt_length != orig_saltlen ||
		    !isc_safe_memequal(saltbuf, orig_salt, salt_length))
		{
			fatal("An NSEC3 chain exists with a different salt. "
			      "Use -u to update it.");
		}
	} else if (!set_salt) {
		salt_length = orig_saltlen;
		memmove(saltbuf, orig_salt, orig_saltlen);
		gsalt = saltbuf;
	}

	if (!update && set_iter) {
		if (nsec3iter != orig_iter) {
			fatal("An NSEC3 chain exists with different "
			      "iterations. Use -u to update it.");
		}
	} else if (!set_iter) {
		nsec3iter = orig_iter;
	}

	/*
	 * Find an NSEC3 record to get the current OPTOUT value.
	 * (This assumes all NSEC3 records agree.)
	 */

	hashname = dns_fixedname_initname(&fname);
	result = dns_nsec3_hashname(&fname, NULL, NULL, gorigin, gorigin,
				    dns_hash_sha1, orig_iter, orig_salt,
				    orig_saltlen);
	check_result(result, "dns_nsec3_hashname");

	result = dns_db_findnsec3node(gdb, hashname, false, &node);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	result = dns_db_findrdataset(gdb, node, ver, dns_rdatatype_nsec3, 0, 0,
				     &rdataset, NULL);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	result = dns_rdataset_first(&rdataset);
	check_result(result, "dns_rdataset_first");
	dns_rdataset_current(&rdataset, &rdata);
	result = dns_rdata_tostruct(&rdata, &nsec3, NULL);
	check_result(result, "dns_rdata_tostruct");

	if (!update && set_optout) {
		if (nsec3flags != nsec3.flags) {
			fatal("An NSEC3 chain exists with%s OPTOUT. "
			      "Use -u -%s to %s it.",
			      OPTOUT(nsec3.flags) ? "" : "out",
			      OPTOUT(nsec3.flags) ? "AA" : "A",
			      OPTOUT(nsec3.flags) ? "clear" : "set");
		}
	} else if (!set_optout) {
		nsec3flags = nsec3.flags;
	}

	dns_rdata_freestruct(&nsec3);

cleanup:
	if (dns_rdataset_isassociated(&rdataset)) {
		dns_rdataset_disassociate(&rdataset);
	}
	if (node != NULL) {
		dns_db_detachnode(gdb, &node);
	}
	dns_db_closeversion(gdb, &ver, false);
}

static void
writeset(const char *prefix, dns_rdatatype_t type) {
	char *filename;
	char namestr[DNS_NAME_FORMATSIZE];
	dns_db_t *db = NULL;
	dns_dbversion_t *dbversion = NULL;
	dns_diff_t diff;
	dns_difftuple_t *tuple = NULL;
	dns_name_t *name;
	dns_rdata_t rdata, ds;
	bool have_ksk = false;
	bool have_non_ksk = false;
	isc_buffer_t b;
	isc_buffer_t namebuf;
	isc_region_t r;
	isc_result_t result;
	unsigned char dsbuf[DNS_DS_BUFFERSIZE];
	unsigned char keybuf[DST_KEY_MAXSIZE];
	unsigned int filenamelen;
	const dns_master_style_t *style = (type == dns_rdatatype_dnskey)
						  ? masterstyle
						  : dsstyle;

	isc_buffer_init(&namebuf, namestr, sizeof(namestr));
	result = dns_name_tofilenametext(gorigin, false, &namebuf);
	check_result(result, "dns_name_tofilenametext");
	isc_buffer_putuint8(&namebuf, 0);
	filenamelen = strlen(prefix) + strlen(namestr) + 1;
	if (dsdir != NULL) {
		filenamelen += strlen(dsdir) + 1;
	}
	filename = isc_mem_get(mctx, filenamelen);
	if (dsdir != NULL) {
		snprintf(filename, filenamelen, "%s/", dsdir);
	} else {
		filename[0] = 0;
	}
	strlcat(filename, prefix, filenamelen);
	strlcat(filename, namestr, filenamelen);

	dns_diff_init(mctx, &diff);

	name = gorigin;

	ISC_LIST_FOREACH (keylist, key, link) {
		if (REVOKE(key->key)) {
			continue;
		}
		if (isksk(key)) {
			have_ksk = true;
			have_non_ksk = false;
		} else {
			have_ksk = false;
			have_non_ksk = true;
		}

		ISC_LIST_FOREACH (keylist, curr, link) {
			if (dst_key_alg(key->key) != dst_key_alg(curr->key)) {
				continue;
			}
			if (REVOKE(curr->key)) {
				continue;
			}
			if (isksk(curr)) {
				have_ksk = true;
			} else {
				have_non_ksk = true;
			}
		}
		if (have_ksk && have_non_ksk && !isksk(key)) {
			continue;
		}
		dns_rdata_init(&rdata);
		dns_rdata_init(&ds);
		isc_buffer_init(&b, keybuf, sizeof(keybuf));
		result = dst_key_todns(key->key, &b);
		check_result(result, "dst_key_todns");
		isc_buffer_usedregion(&b, &r);
		dns_rdata_fromregion(&rdata, gclass, dns_rdatatype_dnskey, &r);
		if (type != dns_rdatatype_dnskey) {
			result = dns_ds_buildrdata(gorigin, &rdata,
						   DNS_DSDIGEST_SHA256, dsbuf,
						   sizeof(dsbuf), &ds);
			check_result(result, "dns_ds_buildrdata");
			dns_difftuple_create(mctx, DNS_DIFFOP_ADDRESIGN, name,
					     0, &ds, &tuple);
		} else {
			dns_difftuple_create(mctx, DNS_DIFFOP_ADDRESIGN,
					     gorigin, zone_soa_min_ttl, &rdata,
					     &tuple);
		}
		dns_diff_append(&diff, &tuple);
	}

	result = dns_db_create(mctx, ZONEDB_DEFAULT, dns_rootname,
			       dns_dbtype_zone, gclass, 0, NULL, &db);
	check_result(result, "dns_db_create");

	result = dns_db_newversion(db, &dbversion);
	check_result(result, "dns_db_newversion");

	result = dns_diff_apply(&diff, db, dbversion);
	check_result(result, "dns_diff_apply");
	dns_diff_clear(&diff);

	result = dns_master_dump(mctx, db, dbversion, style, filename,
				 dns_masterformat_text, NULL);
	check_result(result, "dns_master_dump");

	isc_mem_put(mctx, filename, filenamelen);

	dns_db_closeversion(db, &dbversion, false);
	dns_db_detach(&db);
}

static void
print_time(FILE *fp) {
	time_t currenttime = time(NULL);
	struct tm t, *tm = localtime_r(&currenttime, &t);
	unsigned int flen;
	char timebuf[80];

	if (tm == NULL || outputformat != dns_masterformat_text) {
		return;
	}

	flen = strftime(timebuf, sizeof(timebuf), "%a %b %e %H:%M:%S %Y", tm);
	INSIST(flen > 0U && flen < sizeof(timebuf));
	fprintf(fp, "; File written on %s\n", timebuf);
}

static void
print_version(FILE *fp) {
	if (outputformat != dns_masterformat_text) {
		return;
	}

	fprintf(fp, "; %s version %s\n", isc_commandline_progname,
		PACKAGE_VERSION);
}

ISC_NORETURN static void
usage(void);

static void
usage(void) {
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s [options] zonefile [keys]\n",
		isc_commandline_progname);

	fprintf(stderr, "\n");

	fprintf(stderr, "Version: %s\n", PACKAGE_VERSION);

	fprintf(stderr, "Options: (default value in parenthesis) \n");
	fprintf(stderr, "\t-S:\tsmart signing: automatically finds key files\n"
			"\t\tfor the zone and determines how they are to "
			"be used\n");
	fprintf(stderr, "\t-K directory:\n");
	fprintf(stderr, "\t\tdirectory to find key files (.)\n");
	fprintf(stderr, "\t-d directory:\n");
	fprintf(stderr, "\t\tdirectory to find dsset-* files (.)\n");
	fprintf(stderr, "\t-F:\tFIPS mode\n");
	fprintf(stderr, "\t-g:\t");
	fprintf(stderr, "update DS records based on child zones' "
			"dsset-* files\n");
	fprintf(stderr, "\t-G sync-records:\t");
	fprintf(stderr, "what CDNSKEY and CDS to publish\n");
	fprintf(stderr, "\t-s [YYYYMMDDHHMMSS|+offset]:\n");
	fprintf(stderr, "\t\tRRSIG start time "
			"- absolute|offset (now - 1 hour)\n");
	fprintf(stderr, "\t-e [YYYYMMDDHHMMSS|+offset|\"now\"+offset]:\n");
	fprintf(stderr, "\t\tRRSIG end time "
			"- absolute|from start|from now "
			"(now + 30 days)\n");
	fprintf(stderr, "\t-X [YYYYMMDDHHMMSS|+offset|\"now\"+offset]:\n");
	fprintf(stderr, "\t\tDNSKEY RRSIG end "
			"- absolute|from start|from now "
			"(matches -e)\n");
	fprintf(stderr, "\t-i interval:\n");
	fprintf(stderr, "\t\tcycle interval - resign "
			"if < interval from end ( (end-start)/4 )\n");
	fprintf(stderr, "\t-j jitter:\n");
	fprintf(stderr, "\t\trandomize signature end time up to jitter "
			"seconds\n");
	fprintf(stderr, "\t-v debuglevel (0)\n");
	fprintf(stderr, "\t-q quiet\n");
	fprintf(stderr, "\t-V:\tprint version information\n");
	fprintf(stderr, "\t-o origin:\n");
	fprintf(stderr, "\t\tzone origin (name of zonefile)\n");
	fprintf(stderr, "\t-f outfile:\n");
	fprintf(stderr, "\t\tfile the signed zone is written in "
			"(zonefile + .signed)\n");
	fprintf(stderr, "\t-I format:\n");
	fprintf(stderr, "\t\tfile format of input zonefile (text)\n");
	fprintf(stderr, "\t-O format:\n");
	fprintf(stderr, "\t\tfile format of signed zone file (text)\n");
	fprintf(stderr, "\t-N format:\n");
	fprintf(stderr, "\t\tsoa serial format of signed zone file (keep)\n");
	fprintf(stderr, "\t-D:\n");
	fprintf(stderr, "\t\toutput only DNSSEC-related records\n");
	fprintf(stderr, "\t-a:\t");
	fprintf(stderr, "verify generated signatures\n");
	fprintf(stderr, "\t-c class (IN)\n");
	fprintf(stderr, "\t-P:\t");
	fprintf(stderr, "disable post-sign verification\n");
	fprintf(stderr, "\t-Q:\t");
	fprintf(stderr, "remove signatures from keys that are no "
			"longer active\n");
	fprintf(stderr, "\t-R:\t");
	fprintf(stderr, "remove signatures from keys that no longer exist\n");
	fprintf(stderr, "\t-T TTL:\tTTL for newly added DNSKEYs\n");
	fprintf(stderr, "\t-t:\t");
	fprintf(stderr, "print statistics\n");
	fprintf(stderr, "\t-u:\t");
	fprintf(stderr, "update or replace an existing NSEC/NSEC3 chain\n");
	fprintf(stderr, "\t-x:\tsign DNSKEY record with KSKs only, not ZSKs\n");
	fprintf(stderr, "\t-z:\tsign all records with KSKs\n");
	fprintf(stderr, "\t-C:\tgenerate a keyset file, for compatibility\n"
			"\t\twith older versions of dnssec-signzone -g\n");
	fprintf(stderr, "\t-n ncpus (number of cpus present)\n");
	fprintf(stderr, "\t-k key_signing_key\n");
	fprintf(stderr, "\t-3 NSEC3 salt\n");
	fprintf(stderr, "\t-H NSEC3 iterations (10)\n");
	fprintf(stderr, "\t-A NSEC3 optout\n");

	fprintf(stderr, "\n");

	fprintf(stderr, "Signing Keys: ");
	fprintf(stderr, "(default: all zone keys that have private keys)\n");
	fprintf(stderr, "\tkeyfile (Kname+alg+tag)\n");

	exit(EXIT_FAILURE);
}

static void
removetempfile(void) {
	if (removefile) {
		isc_file_remove(tempfile);
	}
}

static void
print_stats(isc_time_t *timer_start, isc_time_t *timer_finish,
	    isc_time_t *sign_start, isc_time_t *sign_finish) {
	uint64_t time_us; /* Time in microseconds */
	uint64_t time_ms; /* Time in milliseconds */
	uint64_t sig_ms;  /* Signatures per millisecond */
	FILE *out = output_stdout ? stderr : stdout;

	fprintf(out, "Signatures generated:               %10" PRIuFAST32 "\n",
		atomic_load(&nsigned));
	fprintf(out, "Signatures retained:                %10" PRIuFAST32 "\n",
		atomic_load(&nretained));
	fprintf(out, "Signatures dropped:                 %10" PRIuFAST32 "\n",
		atomic_load(&ndropped));
	fprintf(out, "Signatures successfully verified:   %10" PRIuFAST32 "\n",
		atomic_load(&nverified));
	fprintf(out, "Signatures unsuccessfully verified: %10" PRIuFAST32 "\n",
		atomic_load(&nverifyfailed));

	time_us = isc_time_microdiff(sign_finish, sign_start);
	time_ms = time_us / 1000;
	fprintf(out, "Signing time in seconds:           %7u.%03u\n",
		(unsigned int)(time_ms / 1000), (unsigned int)(time_ms % 1000));
	if (time_us > 0) {
		sig_ms = ((uint64_t)atomic_load(&nsigned) * 1000000000) /
			 time_us;
		fprintf(out, "Signatures per second:             %7u.%03u\n",
			(unsigned int)sig_ms / 1000,
			(unsigned int)sig_ms % 1000);
	}

	time_us = isc_time_microdiff(timer_finish, timer_start);
	time_ms = time_us / 1000;
	fprintf(out, "Runtime in seconds:                %7u.%03u\n",
		(unsigned int)(time_ms / 1000), (unsigned int)(time_ms % 1000));
}

int
main(int argc, char *argv[]) {
	int ch;
	char *startstr = NULL, *endstr = NULL, *classname = NULL;
	char *dnskey_endstr = NULL;
	char *origin = NULL, *file = NULL, *output = NULL;
	char *inputformatstr = NULL, *outputformatstr = NULL;
	char *serialformatstr = NULL;
	char *dskeyfile[MAXDSKEYS];
	int ndskeys = 0;
	char *endp;
	isc_time_t timer_start, timer_finish;
	isc_time_t sign_start, sign_finish;
	isc_result_t result, vresult;
	bool free_output = false;
	int tempfilelen = 0;
	dns_rdataclass_t rdclass;
	hashlist_t hashlist;
	bool make_keyset = false;
	bool set_salt = false;
	bool set_optout = false;
	bool set_iter = false;
	bool nonsecify = false;

	atomic_init(&shuttingdown, false);
	atomic_init(&finished, false);

	isc_commandline_init(argc, argv);

	/*
	 * Unused letters: Bb G J l q Yy (and F is reserved).
	 * l was previously used for DLV lookaside.
	 */
#define CMDLINE_FLAGS                                                        \
	"3:AaCc:Dd:E:e:f:FgG:hH:i:I:j:J:K:k:L:m:M:n:N:o:O:PpQqRr:s:ST:tuUv:" \
	"VX:xzZ:"

	/*
	 * Process memory debugging argument first.
	 */
	while ((ch = isc_commandline_parse(argc, argv, CMDLINE_FLAGS)) != -1) {
		switch (ch) {
		case 'm':
			if (strcasecmp(isc_commandline_argument, "record") == 0)
			{
				isc_mem_debugging |= ISC_MEM_DEBUGRECORD;
			}
			if (strcasecmp(isc_commandline_argument, "trace") == 0)
			{
				isc_mem_debugging |= ISC_MEM_DEBUGTRACE;
			}
			if (strcasecmp(isc_commandline_argument, "usage") == 0)
			{
				isc_mem_debugging |= ISC_MEM_DEBUGUSAGE;
			}
			break;
		default:
			break;
		}
	}
	isc_commandline_reset = true;

	masterstyle = &dns_master_style_explicitttl;

	isc_commandline_errprint = false;
	while ((ch = isc_commandline_parse(argc, argv, CMDLINE_FLAGS)) != -1) {
		switch (ch) {
		case '3':
			set_salt = true;
			nsec_datatype = dns_rdatatype_nsec3;
			if (strcmp(isc_commandline_argument, "-") != 0) {
				isc_buffer_t target;
				char *sarg;

				sarg = isc_commandline_argument;
				isc_buffer_init(&target, saltbuf,
						sizeof(saltbuf));
				result = isc_hex_decodestring(sarg, &target);
				check_result(result, "isc_hex_decodestring("
						     "salt)");
				salt_length = isc_buffer_usedlength(&target);
			}
			break;

		case 'A':
			set_optout = true;
			if (OPTOUT(nsec3flags)) {
				nsec3flags &= ~DNS_NSEC3FLAG_OPTOUT;
			} else {
				nsec3flags |= DNS_NSEC3FLAG_OPTOUT;
			}
			break;

		case 'a':
			tryverify = true;
			break;

		case 'C':
			make_keyset = true;
			break;

		case 'c':
			classname = isc_commandline_argument;
			break;

		case 'd':
			dsdir = isc_commandline_argument;
			if (strlen(dsdir) == 0U) {
				fatal("DS directory must be non-empty string");
			}
			result = try_dir(dsdir);
			if (result != ISC_R_SUCCESS) {
				fatal("cannot open directory %s: %s", dsdir,
				      isc_result_totext(result));
			}
			break;

		case 'D':
			output_dnssec_only = true;
			break;

		case 'E':
			fatal("%s", isc_result_totext(DST_R_NOENGINE));
			break;

		case 'e':
			endstr = isc_commandline_argument;
			break;

		case 'f':
			output = isc_commandline_argument;
			if (strcmp(output, "-") == 0) {
				output_stdout = true;
			}
			break;

		case 'g':
			generateds = true;
			break;

		case 'G':
			sync_records = isc_commandline_argument;
			break;

		case 'H':
			set_iter = true;
			/* too-many is NOT DOCUMENTED */
			if (strcmp(isc_commandline_argument, "too-many") == 0) {
				nsec3iter = 51;
				no_max_check = true;
				break;
			}
			nsec3iter = strtoul(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fatal("iterations must be numeric");
			}
			if (nsec3iter > 0xffffU) {
				fatal("iterations too big");
			}
			break;

		case 'I':
			inputformatstr = isc_commandline_argument;
			break;

		case 'i':
			endp = NULL;
			cycle = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0' || cycle < 0) {
				fatal("cycle period must be numeric and "
				      "positive");
			}
			break;

		case 'j':
			endp = NULL;
			jitter = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0' || jitter < 0) {
				fatal("jitter must be numeric and positive");
			}
			break;

		case 'J':
			journal = isc_commandline_argument;
			break;

		case 'K':
			directory = isc_commandline_argument;
			break;

		case 'k':
			if (ndskeys == MAXDSKEYS) {
				fatal("too many key-signing keys specified");
			}
			dskeyfile[ndskeys++] = isc_commandline_argument;
			break;

		case 'L':
			snset = true;
			endp = NULL;
			serialnum = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fprintf(stderr, "source serial number "
						"must be numeric");
				exit(EXIT_FAILURE);
			}
			break;

		case 'M':
			endp = NULL;
			set_maxttl = true;
			maxttl = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fprintf(stderr, "maximum TTL "
						"must be numeric");
				exit(EXIT_FAILURE);
			}
			break;

		case 'm':
			break;

		case 'N':
			serialformatstr = isc_commandline_argument;
			break;

		case 'n':
			endp = NULL;
			nloops = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0' || nloops > INT32_MAX) {
				fatal("number of cpus must be numeric");
			}
			break;

		case 'O':
			outputformatstr = isc_commandline_argument;
			break;

		case 'o':
			origin = isc_commandline_argument;
			break;

		case 'P':
			disable_zone_check = true;
			break;

		case 'p':
			fatal("The -p option has been deprecated.\n");
			break;

		case 'Q':
			remove_inactkeysigs = true;
			break;

		case 'R':
			remove_orphansigs = true;
			break;

		case 'r':
			fatal("The -r options has been deprecated.\n");
			break;

		case 'S':
			smartsign = true;
			break;

		case 's':
			startstr = isc_commandline_argument;
			break;

		case 'T':
			endp = NULL;
			set_keyttl = true;
			keyttl = strtottl(isc_commandline_argument);
			break;

		case 't':
			printstats = true;
			break;

		case 'U': /* Undocumented for testing only. */
			unknownalg = true;
			break;

		case 'u':
			update_chain = true;
			break;

		case 'v':
			endp = NULL;
			verbose = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fatal("verbose level must be numeric");
			}
			break;

		case 'q':
			quiet = true;
			break;

		case 'X':
			dnskey_endstr = isc_commandline_argument;
			break;

		case 'x':
			keyset_kskonly = true;
			break;

		case 'z':
			ignore_kskflag = true;
			break;

		case 'F':
			if (isc_crypto_fips_enable() != ISC_R_SUCCESS) {
				fatal("setting FIPS mode failed");
			}
			break;

		case '?':
			if (isc_commandline_option != '?') {
				fprintf(stderr, "%s: invalid argument -%c\n",
					isc_commandline_progname,
					isc_commandline_option);
			}
			FALLTHROUGH;
		case 'h':
			/* Does not return. */
			usage();

		case 'V':
			/* Does not return. */
			version(isc_commandline_progname);

		case 'Z': /* Undocumented test options */
			if (!strcmp(isc_commandline_argument, "nonsecify")) {
				nonsecify = true;
			}
			break;

		default:
			fprintf(stderr, "%s: unhandled option -%c\n",
				isc_commandline_progname,
				isc_commandline_option);
			exit(EXIT_FAILURE);
		}
	}

	now = isc_stdtime_now();

	if (startstr != NULL) {
		starttime = strtotime(startstr, now, now, NULL);
	} else {
		starttime = now - 3600; /* Allow for some clock skew. */
	}

	if (endstr != NULL) {
		endtime = strtotime(endstr, now, starttime, NULL);
	} else {
		endtime = starttime + (30 * 24 * 60 * 60);
	}

	if (dnskey_endstr != NULL) {
		dnskey_endtime = strtotime(dnskey_endstr, now, starttime, NULL);
		if (endstr != NULL && dnskey_endtime == endtime) {
			fprintf(stderr, "WARNING: -e and -X were both set, "
					"but have identical values.\n");
		}
	} else {
		dnskey_endtime = endtime;
	}

	if (cycle == -1) {
		cycle = (endtime - starttime) / 4;
	}

	if (nloops == 0) {
		nloops = isc_os_ncpus();
	}
	vbprintf(4, "using %d cpus\n", nloops);

	rdclass = strtoclass(classname);

	if (directory == NULL) {
		directory = ".";
	}

	isc_managers_create(&mctx, nloops);

	setup_logging();

	argc -= isc_commandline_index;
	argv += isc_commandline_index;

	if (argc < 1) {
		usage();
	}

	file = argv[0];

	argc -= 1;
	argv += 1;

	if (origin == NULL) {
		origin = file;
	}

	if (output == NULL) {
		size_t size;
		free_output = true;
		size = strlen(file) + strlen(".signed") + 1;
		output = isc_mem_allocate(mctx, size);
		snprintf(output, size, "%s.signed", file);
	}

	if (inputformatstr != NULL) {
		if (strcasecmp(inputformatstr, "text") == 0) {
			inputformat = dns_masterformat_text;
		} else if (strcasecmp(inputformatstr, "raw") == 0) {
			inputformat = dns_masterformat_raw;
		} else if (strncasecmp(inputformatstr, "raw=", 4) == 0) {
			inputformat = dns_masterformat_raw;
			fprintf(stderr, "WARNING: input format version "
					"ignored\n");
		} else {
			fatal("unknown file format: %s", inputformatstr);
		}
	}

	if (outputformatstr != NULL) {
		if (strcasecmp(outputformatstr, "text") == 0) {
			outputformat = dns_masterformat_text;
		} else if (strcasecmp(outputformatstr, "full") == 0) {
			outputformat = dns_masterformat_text;
			masterstyle = &dns_master_style_full;
		} else if (strcasecmp(outputformatstr, "raw") == 0) {
			outputformat = dns_masterformat_raw;
		} else if (strncasecmp(outputformatstr, "raw=", 4) == 0) {
			char *end;

			outputformat = dns_masterformat_raw;
			rawversion = strtol(outputformatstr + 4, &end, 10);
			if (end == outputformatstr + 4 || *end != '\0' ||
			    rawversion > 1U)
			{
				fprintf(stderr, "unknown raw format version\n");
				exit(EXIT_FAILURE);
			}
		} else {
			fatal("unknown file format: %s", outputformatstr);
		}
	}

	if (serialformatstr != NULL) {
		if (strcasecmp(serialformatstr, "keep") == 0) {
			serialformat = SOA_SERIAL_KEEP;
		} else if (strcasecmp(serialformatstr, "increment") == 0 ||
			   strcasecmp(serialformatstr, "incr") == 0)
		{
			serialformat = SOA_SERIAL_INCREMENT;
		} else if (strcasecmp(serialformatstr, "unixtime") == 0) {
			serialformat = SOA_SERIAL_UNIXTIME;
		} else if (strcasecmp(serialformatstr, "date") == 0) {
			serialformat = SOA_SERIAL_DATE;
		} else {
			fatal("unknown soa serial format: %s", serialformatstr);
		}
	}

	if (output_dnssec_only && outputformat != dns_masterformat_text) {
		fatal("option -D can only be used with \"-O text\"");
	}

	if (output_dnssec_only && serialformat != SOA_SERIAL_KEEP) {
		fatal("option -D can only be used with \"-N keep\"");
	}

	if (output_dnssec_only && set_maxttl) {
		fatal("option -D cannot be used with -M");
	}

	result = dns_master_stylecreate(&dsstyle, DNS_STYLEFLAG_NO_TTL, 0, 24,
					0, 0, 0, 8, 0xffffffff, mctx);
	check_result(result, "dns_master_stylecreate");

	gdb = NULL;
	timer_start = isc_time_now();
	loadzone(file, origin, rdclass, &gdb);
	if (journal != NULL) {
		loadjournal(mctx, gdb, journal);
	}
	gorigin = dns_db_origin(gdb);
	gclass = dns_db_class(gdb);
	get_soa_ttls();

	if (set_maxttl && set_keyttl && keyttl > maxttl) {
		fprintf(stderr,
			"%s: warning: Specified key TTL %u "
			"exceeds maximum zone TTL; reducing to %u\n",
			isc_commandline_progname, keyttl, maxttl);
		keyttl = maxttl;
	}

	if (!set_keyttl) {
		keyttl = soa_ttl;
	}

	/*
	 * Check for any existing NSEC3 parameters in the zone,
	 * and use them as defaults if -u was not specified.
	 */
	if (update_chain && !set_optout && !set_iter && !set_salt) {
		nsec_datatype = dns_rdatatype_nsec;
	} else {
		set_nsec3params(update_chain, set_salt, set_optout, set_iter);
	}

	/*
	 * We need to do this early on, as we start messing with the list
	 * of keys rather early.
	 */
	ISC_LIST_INIT(keylist);
	isc_rwlock_init(&keylist_lock);

	/*
	 * Fill keylist with:
	 * 1) Keys listed in the DNSKEY set that have
	 *    private keys associated, *if* no keys were
	 *    set on the command line.
	 * 2) ZSKs set on the command line
	 * 3) KSKs set on the command line
	 * 4) Any keys remaining in the DNSKEY set which
	 *    do not have private keys associated and were
	 *    not specified on the command line.
	 */
	if (argc == 0 || smartsign) {
		loadzonekeys(!smartsign, false);
	}
	loadexplicitkeys(argv, argc, false);
	loadexplicitkeys(dskeyfile, ndskeys, true);
	loadzonekeys(!smartsign, true);

	/*
	 * If we're doing smart signing, look in the key repository for
	 * key files with metadata, and merge them with the keylist
	 * we have now.
	 */
	if (smartsign) {
		build_final_keylist();
	}

	/* Now enumerate the key list */
	ISC_LIST_FOREACH (keylist, key, link) {
		key->index = keycount++;
	}

	if (keycount == 0) {
		if (disable_zone_check) {
			fprintf(stderr,
				"%s: warning: No keys specified "
				"or found\n",
				isc_commandline_progname);
		} else {
			fatal("No signing keys specified or found.");
		}
		nokeys = true;
	}

	warnifallksk(gdb);

	if (IS_NSEC3) {
		bool answer;

		hash_length = dns_nsec3_hashlength(dns_hash_sha1);
		hashlist_init(&hashlist,
			      dns_db_nodecount(gdb, dns_dbtree_main) * 2,
			      hash_length);
		result = dns_nsec_nseconly(gdb, gversion, NULL, &answer);
		if (result == ISC_R_NOTFOUND) {
			fprintf(stderr,
				"%s: warning: NSEC3 generation "
				"requested with no DNSKEY; ignoring\n",
				isc_commandline_progname);
		} else if (result != ISC_R_SUCCESS) {
			check_result(result, "dns_nsec_nseconly");
		} else if (answer) {
			fatal("NSEC3 generation requested with "
			      "NSEC-only DNSKEY");
		}

		if (nsec3iter > dns_nsec3_maxiterations()) {
			if (no_max_check) {
				fprintf(stderr,
					"Ignoring max iterations check.\n");
			} else {
				fatal("NSEC3 iterations too big. Maximum "
				      "iterations allowed %u.",
				      dns_nsec3_maxiterations());
			}
		}
	} else {
		hashlist_init(&hashlist, 0, 0); /* silence clang */
	}

	gversion = NULL;
	result = dns_db_newversion(gdb, &gversion);
	check_result(result, "dns_db_newversion()");

	switch (serialformat) {
	case SOA_SERIAL_INCREMENT:
		setsoaserial(0, dns_updatemethod_increment);
		break;
	case SOA_SERIAL_UNIXTIME:
		setsoaserial(now, dns_updatemethod_unixtime);
		break;
	case SOA_SERIAL_DATE:
		setsoaserial(now, dns_updatemethod_date);
		break;
	case SOA_SERIAL_KEEP:
	default:
		/* do nothing */
		break;
	}

	/* Remove duplicates and cap TTLs at maxttl */
	cleanup_zone();

	if (!nonsecify) {
		if (IS_NSEC3) {
			nsec3ify(dns_hash_sha1, nsec3iter, gsalt, salt_length,
				 &hashlist);
		} else {
			nsecify();
		}
	}

	if (!nokeys) {
		writeset("dsset-", dns_rdatatype_ds);
		if (make_keyset) {
			writeset("keyset-", dns_rdatatype_dnskey);
		}
	}

	if (output_stdout) {
		outfp = stdout;
		if (outputformatstr == NULL) {
			masterstyle = &dns_master_style_full;
		}
	} else {
		tempfilelen = strlen(output) + 20;
		tempfile = isc_mem_get(mctx, tempfilelen);

		result = isc_file_mktemplate(output, tempfile, tempfilelen);
		check_result(result, "isc_file_mktemplate");

		result = isc_file_openunique(tempfile, &outfp);
		if (result != ISC_R_SUCCESS) {
			fatal("failed to open temporary output file: %s",
			      isc_result_totext(result));
		}
		removefile = true;
		setfatalcallback(&removetempfile);
	}

	print_time(outfp);
	print_version(outfp);

	isc_mutex_init(&namelock);

	presign();
	sign_start = isc_time_now();
	signapex();
	if (!atomic_load(&finished)) {
		/*
		 * There is more work to do.  Spread it out over multiple
		 * processors if possible.
		 */
		isc_loopmgr_setup(assignwork, NULL);
		isc_loopmgr_teardown(abortwork, NULL);
		isc_loopmgr_run();

		if (!atomic_load(&finished)) {
			fatal("process aborted by user");
		}
	}
	postsign();
	sign_finish = isc_time_now();

	if (disable_zone_check) {
		vresult = ISC_R_SUCCESS;
	} else {
		vresult = dns_zoneverify_dnssec(NULL, gdb, gversion, gorigin,
						NULL, mctx, ignore_kskflag,
						keyset_kskonly, report);
		if (vresult != ISC_R_SUCCESS) {
			fprintf(output_stdout ? stderr : stdout,
				"Zone verification failed (%s)\n",
				isc_result_totext(vresult));
		}
	}

	if (!output_dnssec_only) {
		dns_masterrawheader_t header;
		dns_master_initrawheader(&header);
		if (rawversion == 0U) {
			header.flags = DNS_MASTERRAW_COMPAT;
		} else if (snset) {
			header.flags = DNS_MASTERRAW_SOURCESERIALSET;
			header.sourceserial = serialnum;
		}
		result = dns_master_dumptostream(mctx, gdb, gversion,
						 masterstyle, outputformat,
						 &header, outfp);
		check_result(result, "dns_master_dumptostream");
	}

	if (!output_stdout) {
		result = isc_stdio_close(outfp);
		check_result(result, "isc_stdio_close");
		removefile = false;

		if (vresult == ISC_R_SUCCESS) {
			result = isc_file_rename(tempfile, output);
			if (result != ISC_R_SUCCESS) {
				fatal("failed to rename temp file to %s: %s",
				      output, isc_result_totext(result));
			}
			printf("%s\n", output);
		} else {
			isc_file_remove(tempfile);
		}
	}

	dns_db_closeversion(gdb, &gversion, false);
	dns_db_detach(&gdb);

	hashlist_free(&hashlist);

	ISC_LIST_FOREACH (keylist, key, link) {
		ISC_LIST_UNLINK(keylist, key, link);
		dns_dnsseckey_destroy(mctx, &key);
	}

	if (tempfilelen != 0) {
		isc_mem_put(mctx, tempfile, tempfilelen);
	}

	if (free_output) {
		isc_mem_free(mctx, output);
	}

	dns_master_styledestroy(&dsstyle, mctx);

	if (verbose > 10) {
		isc_mem_stats(mctx, stdout);
	}

	isc_managers_destroy(&mctx);

	if (printstats) {
		timer_finish = isc_time_now();
		print_stats(&timer_start, &timer_finish, &sign_start,
			    &sign_finish);
	}
	isc_mutex_destroy(&namelock);

	return vresult == ISC_R_SUCCESS ? 0 : 1;
}
