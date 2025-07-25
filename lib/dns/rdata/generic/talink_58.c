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

#ifndef RDATA_GENERIC_TALINK_58_C
#define RDATA_GENERIC_TALINK_58_C

#define RRTYPE_TALINK_ATTRIBUTES 0

static isc_result_t
fromtext_talink(ARGS_FROMTEXT) {
	isc_token_t token;
	isc_buffer_t buffer;
	int i;

	REQUIRE(type == dns_rdatatype_talink);

	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(callbacks);

	if (origin == NULL) {
		origin = dns_rootname;
	}

	for (i = 0; i < 2; i++) {
		RETERR(isc_lex_getmastertoken(lexer, &token,
					      isc_tokentype_string, false));

		buffer_fromregion(&buffer, &token.value.as_region);
		RETTOK(dns_name_wirefromtext(&buffer, origin, options, target));
	}

	return ISC_R_SUCCESS;
}

static isc_result_t
totext_talink(ARGS_TOTEXT) {
	isc_region_t dregion;
	dns_name_t prev;
	dns_name_t next;
	dns_name_t prefix;
	unsigned int opts;

	REQUIRE(rdata->type == dns_rdatatype_talink);
	REQUIRE(rdata->length != 0);

	dns_name_init(&prev);
	dns_name_init(&next);
	dns_name_init(&prefix);

	dns_rdata_toregion(rdata, &dregion);

	dns_name_fromregion(&prev, &dregion);
	isc_region_consume(&dregion, name_length(&prev));

	dns_name_fromregion(&next, &dregion);
	isc_region_consume(&dregion, name_length(&next));

	opts = name_prefix(&prev, tctx->origin, &prefix) ? DNS_NAME_OMITFINALDOT
							 : 0;
	RETERR(dns_name_totext(&prefix, opts, target));

	RETERR(str_totext(" ", target));

	opts = name_prefix(&next, tctx->origin, &prefix) ? DNS_NAME_OMITFINALDOT
							 : 0;
	return dns_name_totext(&prefix, opts, target);
}

static isc_result_t
fromwire_talink(ARGS_FROMWIRE) {
	dns_name_t prev;
	dns_name_t next;

	REQUIRE(type == dns_rdatatype_talink);

	UNUSED(type);
	UNUSED(rdclass);

	dctx = dns_decompress_setpermitted(dctx, false);

	dns_name_init(&prev);
	dns_name_init(&next);

	RETERR(dns_name_fromwire(&prev, source, dctx, target));
	return dns_name_fromwire(&next, source, dctx, target);
}

static isc_result_t
towire_talink(ARGS_TOWIRE) {
	isc_region_t sregion;
	dns_name_t prev;
	dns_name_t next;

	REQUIRE(rdata->type == dns_rdatatype_talink);
	REQUIRE(rdata->length != 0);

	dns_compress_setpermitted(cctx, false);

	dns_name_init(&prev);
	dns_name_init(&next);

	dns_rdata_toregion(rdata, &sregion);

	dns_name_fromregion(&prev, &sregion);
	isc_region_consume(&sregion, name_length(&prev));
	RETERR(dns_name_towire(&prev, cctx, target));

	dns_name_fromregion(&next, &sregion);
	isc_region_consume(&sregion, name_length(&next));
	return dns_name_towire(&next, cctx, target);
}

static int
compare_talink(ARGS_COMPARE) {
	isc_region_t region1;
	isc_region_t region2;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == dns_rdatatype_talink);
	REQUIRE(rdata1->length != 0);
	REQUIRE(rdata2->length != 0);

	dns_rdata_toregion(rdata1, &region1);
	dns_rdata_toregion(rdata2, &region2);
	return isc_region_compare(&region1, &region2);
}

static isc_result_t
fromstruct_talink(ARGS_FROMSTRUCT) {
	dns_rdata_talink_t *talink = source;
	isc_region_t region;

	REQUIRE(type == dns_rdatatype_talink);
	REQUIRE(talink != NULL);
	REQUIRE(talink->common.rdtype == type);
	REQUIRE(talink->common.rdclass == rdclass);

	UNUSED(type);
	UNUSED(rdclass);

	dns_name_toregion(&talink->prev, &region);
	RETERR(isc_buffer_copyregion(target, &region));
	dns_name_toregion(&talink->next, &region);
	return isc_buffer_copyregion(target, &region);
}

static isc_result_t
tostruct_talink(ARGS_TOSTRUCT) {
	isc_region_t region;
	dns_rdata_talink_t *talink = target;
	dns_name_t name;

	REQUIRE(rdata->type == dns_rdatatype_talink);
	REQUIRE(talink != NULL);
	REQUIRE(rdata->length != 0);

	talink->common.rdclass = rdata->rdclass;
	talink->common.rdtype = rdata->type;

	dns_rdata_toregion(rdata, &region);

	dns_name_init(&name);
	dns_name_fromregion(&name, &region);
	isc_region_consume(&region, name_length(&name));
	dns_name_init(&talink->prev);
	name_duporclone(&name, mctx, &talink->prev);

	dns_name_fromregion(&name, &region);
	isc_region_consume(&region, name_length(&name));
	dns_name_init(&talink->next);
	name_duporclone(&name, mctx, &talink->next);

	talink->mctx = mctx;
	return ISC_R_SUCCESS;
}

static void
freestruct_talink(ARGS_FREESTRUCT) {
	dns_rdata_talink_t *talink = source;

	REQUIRE(talink != NULL);
	REQUIRE(talink->common.rdtype == dns_rdatatype_talink);

	if (talink->mctx == NULL) {
		return;
	}

	dns_name_free(&talink->prev, talink->mctx);
	dns_name_free(&talink->next, talink->mctx);
	talink->mctx = NULL;
}

static isc_result_t
additionaldata_talink(ARGS_ADDLDATA) {
	REQUIRE(rdata->type == dns_rdatatype_talink);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(add);
	UNUSED(arg);

	return ISC_R_SUCCESS;
}

static isc_result_t
digest_talink(ARGS_DIGEST) {
	isc_region_t r;

	REQUIRE(rdata->type == dns_rdatatype_talink);

	dns_rdata_toregion(rdata, &r);
	return (digest)(arg, &r);
}

static bool
checkowner_talink(ARGS_CHECKOWNER) {
	REQUIRE(type == dns_rdatatype_talink);

	UNUSED(name);
	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(wildcard);

	return true;
}

static bool
checknames_talink(ARGS_CHECKNAMES) {
	REQUIRE(rdata->type == dns_rdatatype_talink);

	UNUSED(bad);
	UNUSED(owner);

	return true;
}

static int
casecompare_talink(ARGS_COMPARE) {
	return compare_talink(rdata1, rdata2);
}

#endif /* RDATA_GENERIC_TALINK_58_C */
