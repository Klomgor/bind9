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

/* RFC2874 */

#ifndef RDATA_IN_1_A6_28_C
#define RDATA_IN_1_A6_28_C

#include <isc/net.h>

#define RRTYPE_A6_ATTRIBUTES (0)

static isc_result_t
fromtext_in_a6(ARGS_FROMTEXT) {
	isc_token_t token;
	unsigned char addr[16];
	unsigned char prefixlen;
	unsigned char octets;
	unsigned char mask;
	isc_buffer_t buffer;
	dns_fixedname_t fn;
	dns_name_t *name = dns_fixedname_initname(&fn);
	bool ok;

	REQUIRE(type == dns_rdatatype_a6);
	REQUIRE(rdclass == dns_rdataclass_in);

	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(callbacks);

	/*
	 * Prefix length.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 128U) {
		RETTOK(ISC_R_RANGE);
	}

	prefixlen = (unsigned char)token.value.as_ulong;
	RETERR(mem_tobuffer(target, &prefixlen, 1));

	/*
	 * Suffix.
	 */
	if (prefixlen != 128) {
		/*
		 * Prefix 0..127.
		 */
		octets = prefixlen / 8;
		/*
		 * Octets 0..15.
		 */
		RETERR(isc_lex_getmastertoken(lexer, &token,
					      isc_tokentype_string, false));
		if (inet_pton(AF_INET6, DNS_AS_STR(token), addr) != 1) {
			RETTOK(DNS_R_BADAAAA);
		}
		mask = 0xff >> (prefixlen % 8);
		addr[octets] &= mask;
		RETERR(mem_tobuffer(target, &addr[octets], 16 - octets));
	}

	if (prefixlen == 0) {
		return ISC_R_SUCCESS;
	}

	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_string,
				      false));
	buffer_fromregion(&buffer, &token.value.as_region);
	if (origin == NULL) {
		origin = dns_rootname;
	}

	RETTOK(dns_name_fromtext(name, &buffer, origin, options));
	RETTOK(dns_name_towire(name, NULL, target));

	ok = true;
	if ((options & DNS_RDATA_CHECKNAMES) != 0) {
		ok = dns_name_ishostname(name, false);
	}
	if (!ok && (options & DNS_RDATA_CHECKNAMESFAIL) != 0) {
		RETTOK(DNS_R_BADNAME);
	}
	if (!ok && callbacks != NULL) {
		warn_badname(name, lexer, callbacks);
	}
	return ISC_R_SUCCESS;
}

static isc_result_t
totext_in_a6(ARGS_TOTEXT) {
	isc_region_t sr, ar;
	unsigned char addr[16];
	unsigned char prefixlen;
	unsigned char octets;
	unsigned char mask;
	char buf[sizeof("128")];
	dns_name_t name;
	dns_name_t prefix;
	unsigned int opts;

	REQUIRE(rdata->type == dns_rdatatype_a6);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);
	REQUIRE(rdata->length != 0);

	dns_rdata_toregion(rdata, &sr);
	prefixlen = sr.base[0];
	INSIST(prefixlen <= 128);
	isc_region_consume(&sr, 1);
	snprintf(buf, sizeof(buf), "%u", prefixlen);
	RETERR(str_totext(buf, target));
	RETERR(str_totext(" ", target));

	if (prefixlen != 128) {
		octets = prefixlen / 8;
		memset(addr, 0, sizeof(addr));
		memmove(&addr[octets], sr.base, 16 - octets);
		mask = 0xff >> (prefixlen % 8);
		addr[octets] &= mask;
		ar.base = addr;
		ar.length = sizeof(addr);
		RETERR(inet_totext(AF_INET6, tctx->flags, &ar, target));
		isc_region_consume(&sr, 16 - octets);
	}

	if (prefixlen == 0) {
		return ISC_R_SUCCESS;
	}

	RETERR(str_totext(" ", target));
	dns_name_init(&name);
	dns_name_init(&prefix);
	dns_name_fromregion(&name, &sr);
	opts = name_prefix(&name, tctx->origin, &prefix) ? DNS_NAME_OMITFINALDOT
							 : 0;
	return dns_name_totext(&prefix, opts, target);
}

static isc_result_t
fromwire_in_a6(ARGS_FROMWIRE) {
	isc_region_t sr;
	unsigned char prefixlen;
	unsigned char octets;
	unsigned char mask;
	dns_name_t name;

	REQUIRE(type == dns_rdatatype_a6);
	REQUIRE(rdclass == dns_rdataclass_in);

	UNUSED(type);
	UNUSED(rdclass);

	dctx = dns_decompress_setpermitted(dctx, false);

	isc_buffer_activeregion(source, &sr);
	/*
	 * Prefix length.
	 */
	if (sr.length < 1) {
		return ISC_R_UNEXPECTEDEND;
	}
	prefixlen = sr.base[0];
	if (prefixlen > 128) {
		return ISC_R_RANGE;
	}
	isc_region_consume(&sr, 1);
	RETERR(mem_tobuffer(target, &prefixlen, 1));
	isc_buffer_forward(source, 1);

	/*
	 * Suffix.
	 */
	if (prefixlen != 128) {
		octets = 16 - prefixlen / 8;
		if (sr.length < octets) {
			return ISC_R_UNEXPECTEDEND;
		}
		mask = 0xff >> (prefixlen % 8);
		if ((sr.base[0] & ~mask) != 0) {
			return DNS_R_FORMERR;
		}
		RETERR(mem_tobuffer(target, sr.base, octets));
		isc_buffer_forward(source, octets);
	}

	if (prefixlen == 0) {
		return ISC_R_SUCCESS;
	}

	dns_name_init(&name);
	return dns_name_fromwire(&name, source, dctx, target);
}

static isc_result_t
towire_in_a6(ARGS_TOWIRE) {
	isc_region_t sr;
	dns_name_t name;
	unsigned char prefixlen;
	unsigned char octets;

	REQUIRE(rdata->type == dns_rdatatype_a6);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);
	REQUIRE(rdata->length != 0);

	dns_compress_setpermitted(cctx, false);
	dns_rdata_toregion(rdata, &sr);
	prefixlen = sr.base[0];
	INSIST(prefixlen <= 128);

	octets = 1 + 16 - prefixlen / 8;
	RETERR(mem_tobuffer(target, sr.base, octets));
	isc_region_consume(&sr, octets);

	if (prefixlen == 0) {
		return ISC_R_SUCCESS;
	}

	dns_name_init(&name);
	dns_name_fromregion(&name, &sr);
	return dns_name_towire(&name, cctx, target);
}

static int
compare_in_a6(ARGS_COMPARE) {
	int order;
	unsigned char prefixlen1, prefixlen2;
	unsigned char octets;
	dns_name_t name1;
	dns_name_t name2;
	isc_region_t region1;
	isc_region_t region2;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == dns_rdatatype_a6);
	REQUIRE(rdata1->rdclass == dns_rdataclass_in);
	REQUIRE(rdata1->length != 0);
	REQUIRE(rdata2->length != 0);

	dns_rdata_toregion(rdata1, &region1);
	dns_rdata_toregion(rdata2, &region2);
	prefixlen1 = region1.base[0];
	prefixlen2 = region2.base[0];
	isc_region_consume(&region1, 1);
	isc_region_consume(&region2, 1);
	if (prefixlen1 < prefixlen2) {
		return -1;
	} else if (prefixlen1 > prefixlen2) {
		return 1;
	}
	/*
	 * Prefix lengths are equal.
	 */
	octets = 16 - prefixlen1 / 8;

	if (octets > 0) {
		order = memcmp(region1.base, region2.base, octets);
		if (order < 0) {
			return -1;
		} else if (order > 0) {
			return 1;
		}
		/*
		 * Address suffixes are equal.
		 */
		if (prefixlen1 == 0) {
			return order;
		}
		isc_region_consume(&region1, octets);
		isc_region_consume(&region2, octets);
	}

	dns_name_init(&name1);
	dns_name_init(&name2);
	dns_name_fromregion(&name1, &region1);
	dns_name_fromregion(&name2, &region2);
	return dns_name_rdatacompare(&name1, &name2);
}

static isc_result_t
fromstruct_in_a6(ARGS_FROMSTRUCT) {
	dns_rdata_in_a6_t *a6 = source;
	isc_region_t region;
	int octets;
	uint8_t bits;
	uint8_t first;
	uint8_t mask;

	REQUIRE(type == dns_rdatatype_a6);
	REQUIRE(rdclass == dns_rdataclass_in);
	REQUIRE(a6 != NULL);
	REQUIRE(a6->common.rdtype == type);
	REQUIRE(a6->common.rdclass == rdclass);

	UNUSED(type);
	UNUSED(rdclass);

	if (a6->prefixlen > 128) {
		return ISC_R_RANGE;
	}

	RETERR(uint8_tobuffer(a6->prefixlen, target));

	/* Suffix */
	if (a6->prefixlen != 128) {
		octets = 16 - a6->prefixlen / 8;
		bits = a6->prefixlen % 8;
		if (bits != 0) {
			mask = 0xffU >> bits;
			first = a6->in6_addr.s6_addr[16 - octets] & mask;
			RETERR(uint8_tobuffer(first, target));
			octets--;
		}
		if (octets > 0) {
			RETERR(mem_tobuffer(target,
					    a6->in6_addr.s6_addr + 16 - octets,
					    octets));
		}
	}

	if (a6->prefixlen == 0) {
		return ISC_R_SUCCESS;
	}
	dns_name_toregion(&a6->prefix, &region);
	return isc_buffer_copyregion(target, &region);
}

static isc_result_t
tostruct_in_a6(ARGS_TOSTRUCT) {
	dns_rdata_in_a6_t *a6 = target;
	unsigned char octets;
	dns_name_t name;
	isc_region_t r;

	REQUIRE(rdata->type == dns_rdatatype_a6);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);
	REQUIRE(a6 != NULL);
	REQUIRE(rdata->length != 0);

	a6->common.rdclass = rdata->rdclass;
	a6->common.rdtype = rdata->type;

	dns_rdata_toregion(rdata, &r);

	a6->prefixlen = uint8_fromregion(&r);
	isc_region_consume(&r, 1);
	memset(a6->in6_addr.s6_addr, 0, sizeof(a6->in6_addr.s6_addr));

	/*
	 * Suffix.
	 */
	if (a6->prefixlen != 128) {
		octets = 16 - a6->prefixlen / 8;
		INSIST(r.length >= octets);
		memmove(a6->in6_addr.s6_addr + 16 - octets, r.base, octets);
		isc_region_consume(&r, octets);
	}

	/*
	 * Prefix.
	 */
	dns_name_init(&a6->prefix);
	if (a6->prefixlen != 0) {
		dns_name_init(&name);
		dns_name_fromregion(&name, &r);
		name_duporclone(&name, mctx, &a6->prefix);
	}
	a6->mctx = mctx;
	return ISC_R_SUCCESS;
}

static void
freestruct_in_a6(ARGS_FREESTRUCT) {
	dns_rdata_in_a6_t *a6 = source;

	REQUIRE(a6 != NULL);
	REQUIRE(a6->common.rdclass == dns_rdataclass_in);
	REQUIRE(a6->common.rdtype == dns_rdatatype_a6);

	if (a6->mctx == NULL) {
		return;
	}

	if (dns_name_dynamic(&a6->prefix)) {
		dns_name_free(&a6->prefix, a6->mctx);
	}
	a6->mctx = NULL;
}

static isc_result_t
additionaldata_in_a6(ARGS_ADDLDATA) {
	REQUIRE(rdata->type == dns_rdatatype_a6);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(add);
	UNUSED(arg);

	return ISC_R_SUCCESS;
}

static isc_result_t
digest_in_a6(ARGS_DIGEST) {
	isc_region_t r1, r2;
	unsigned char prefixlen, octets;
	isc_result_t result;
	dns_name_t name;

	REQUIRE(rdata->type == dns_rdatatype_a6);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);

	dns_rdata_toregion(rdata, &r1);
	r2 = r1;
	prefixlen = r1.base[0];
	octets = 1 + 16 - prefixlen / 8;

	r1.length = octets;
	result = (digest)(arg, &r1);
	if (result != ISC_R_SUCCESS) {
		return result;
	}
	if (prefixlen == 0) {
		return ISC_R_SUCCESS;
	}

	isc_region_consume(&r2, octets);
	dns_name_init(&name);
	dns_name_fromregion(&name, &r2);
	return dns_name_digest(&name, digest, arg);
}

static bool
checkowner_in_a6(ARGS_CHECKOWNER) {
	REQUIRE(type == dns_rdatatype_a6);
	REQUIRE(rdclass == dns_rdataclass_in);

	UNUSED(type);
	UNUSED(rdclass);

	return dns_name_ishostname(name, wildcard);
}

static bool
checknames_in_a6(ARGS_CHECKNAMES) {
	isc_region_t region;
	dns_name_t name;
	unsigned int prefixlen;

	REQUIRE(rdata->type == dns_rdatatype_a6);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);

	UNUSED(owner);

	dns_rdata_toregion(rdata, &region);
	prefixlen = uint8_fromregion(&region);
	if (prefixlen == 0) {
		return true;
	}
	isc_region_consume(&region, 1 + 16 - prefixlen / 8);
	dns_name_init(&name);
	dns_name_fromregion(&name, &region);
	if (!dns_name_ishostname(&name, false)) {
		if (bad != NULL) {
			dns_name_clone(&name, bad);
		}
		return false;
	}
	return true;
}

static int
casecompare_in_a6(ARGS_COMPARE) {
	return compare_in_a6(rdata1, rdata2);
}

#endif /* RDATA_IN_1_A6_38_C */
