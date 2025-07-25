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

/* RFC2163 */

#ifndef RDATA_IN_1_PX_26_C
#define RDATA_IN_1_PX_26_C

#define RRTYPE_PX_ATTRIBUTES (0)

static isc_result_t
fromtext_in_px(ARGS_FROMTEXT) {
	isc_token_t token;
	isc_buffer_t buffer;

	REQUIRE(type == dns_rdatatype_px);
	REQUIRE(rdclass == dns_rdataclass_in);

	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(callbacks);

	if (origin == NULL) {
		origin = dns_rootname;
	}

	/*
	 * Preference.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0xffffU) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint16_tobuffer(token.value.as_ulong, target));

	/*
	 * MAP822.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_string,
				      false));
	buffer_fromregion(&buffer, &token.value.as_region);
	RETTOK(dns_name_wirefromtext(&buffer, origin, options, target));

	/*
	 * MAPX400.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_string,
				      false));
	buffer_fromregion(&buffer, &token.value.as_region);
	RETTOK(dns_name_wirefromtext(&buffer, origin, options, target));
	return ISC_R_SUCCESS;
}

static isc_result_t
totext_in_px(ARGS_TOTEXT) {
	isc_region_t region;
	dns_name_t name;
	dns_name_t prefix;
	unsigned int opts;
	char buf[sizeof("64000")];
	unsigned short num;

	REQUIRE(rdata->type == dns_rdatatype_px);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);
	REQUIRE(rdata->length != 0);

	dns_name_init(&name);
	dns_name_init(&prefix);

	/*
	 * Preference.
	 */
	dns_rdata_toregion(rdata, &region);
	num = uint16_fromregion(&region);
	isc_region_consume(&region, 2);
	snprintf(buf, sizeof(buf), "%u", num);
	RETERR(str_totext(buf, target));
	RETERR(str_totext(" ", target));

	/*
	 * MAP822.
	 */
	dns_name_fromregion(&name, &region);
	opts = name_prefix(&name, tctx->origin, &prefix) ? DNS_NAME_OMITFINALDOT
							 : 0;
	isc_region_consume(&region, name_length(&name));
	RETERR(dns_name_totext(&prefix, opts, target));
	RETERR(str_totext(" ", target));

	/*
	 * MAPX400.
	 */
	dns_name_fromregion(&name, &region);
	opts = name_prefix(&name, tctx->origin, &prefix) ? DNS_NAME_OMITFINALDOT
							 : 0;
	return dns_name_totext(&prefix, opts, target);
}

static isc_result_t
fromwire_in_px(ARGS_FROMWIRE) {
	dns_name_t name;
	isc_region_t sregion;

	REQUIRE(type == dns_rdatatype_px);
	REQUIRE(rdclass == dns_rdataclass_in);

	UNUSED(type);
	UNUSED(rdclass);

	dctx = dns_decompress_setpermitted(dctx, false);

	dns_name_init(&name);

	/*
	 * Preference.
	 */
	isc_buffer_activeregion(source, &sregion);
	if (sregion.length < 2) {
		return ISC_R_UNEXPECTEDEND;
	}
	RETERR(mem_tobuffer(target, sregion.base, 2));
	isc_buffer_forward(source, 2);

	/*
	 * MAP822.
	 */
	RETERR(dns_name_fromwire(&name, source, dctx, target));

	/*
	 * MAPX400.
	 */
	return dns_name_fromwire(&name, source, dctx, target);
}

static isc_result_t
towire_in_px(ARGS_TOWIRE) {
	dns_name_t name;
	isc_region_t region;

	REQUIRE(rdata->type == dns_rdatatype_px);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);
	REQUIRE(rdata->length != 0);

	dns_compress_setpermitted(cctx, false);
	/*
	 * Preference.
	 */
	dns_rdata_toregion(rdata, &region);
	RETERR(mem_tobuffer(target, region.base, 2));
	isc_region_consume(&region, 2);

	/*
	 * MAP822.
	 */
	dns_name_init(&name);
	dns_name_fromregion(&name, &region);
	RETERR(dns_name_towire(&name, cctx, target));
	isc_region_consume(&region, name_length(&name));

	/*
	 * MAPX400.
	 */
	dns_name_init(&name);
	dns_name_fromregion(&name, &region);
	return dns_name_towire(&name, cctx, target);
}

static int
compare_in_px(ARGS_COMPARE) {
	dns_name_t name1;
	dns_name_t name2;
	isc_region_t region1;
	isc_region_t region2;
	int order;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == dns_rdatatype_px);
	REQUIRE(rdata1->rdclass == dns_rdataclass_in);
	REQUIRE(rdata1->length != 0);
	REQUIRE(rdata2->length != 0);

	order = memcmp(rdata1->data, rdata2->data, 2);
	if (order != 0) {
		return order < 0 ? -1 : 1;
	}

	dns_name_init(&name1);
	dns_name_init(&name2);

	dns_rdata_toregion(rdata1, &region1);
	dns_rdata_toregion(rdata2, &region2);

	isc_region_consume(&region1, 2);
	isc_region_consume(&region2, 2);

	dns_name_fromregion(&name1, &region1);
	dns_name_fromregion(&name2, &region2);

	order = dns_name_rdatacompare(&name1, &name2);
	if (order != 0) {
		return order;
	}

	isc_region_consume(&region1, name_length(&name1));
	isc_region_consume(&region2, name_length(&name2));

	dns_name_fromregion(&name1, &region1);
	dns_name_fromregion(&name2, &region2);

	return dns_name_rdatacompare(&name1, &name2);
}

static isc_result_t
fromstruct_in_px(ARGS_FROMSTRUCT) {
	dns_rdata_in_px_t *px = source;
	isc_region_t region;

	REQUIRE(type == dns_rdatatype_px);
	REQUIRE(rdclass == dns_rdataclass_in);
	REQUIRE(px != NULL);
	REQUIRE(px->common.rdtype == type);
	REQUIRE(px->common.rdclass == rdclass);

	UNUSED(type);
	UNUSED(rdclass);

	RETERR(uint16_tobuffer(px->preference, target));
	dns_name_toregion(&px->map822, &region);
	RETERR(isc_buffer_copyregion(target, &region));
	dns_name_toregion(&px->mapx400, &region);
	return isc_buffer_copyregion(target, &region);
}

static isc_result_t
tostruct_in_px(ARGS_TOSTRUCT) {
	dns_rdata_in_px_t *px = target;
	dns_name_t name;
	isc_region_t region;

	REQUIRE(rdata->type == dns_rdatatype_px);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);
	REQUIRE(px != NULL);
	REQUIRE(rdata->length != 0);

	px->common.rdclass = rdata->rdclass;
	px->common.rdtype = rdata->type;

	dns_name_init(&name);
	dns_rdata_toregion(rdata, &region);

	px->preference = uint16_fromregion(&region);
	isc_region_consume(&region, 2);

	dns_name_fromregion(&name, &region);

	dns_name_init(&px->map822);
	name_duporclone(&name, mctx, &px->map822);
	isc_region_consume(&region, name_length(&px->map822));

	dns_name_init(&px->mapx400);
	name_duporclone(&name, mctx, &px->mapx400);

	px->mctx = mctx;
	return ISC_R_SUCCESS;
}

static void
freestruct_in_px(ARGS_FREESTRUCT) {
	dns_rdata_in_px_t *px = source;

	REQUIRE(px != NULL);
	REQUIRE(px->common.rdclass == dns_rdataclass_in);
	REQUIRE(px->common.rdtype == dns_rdatatype_px);

	if (px->mctx == NULL) {
		return;
	}

	dns_name_free(&px->map822, px->mctx);
	dns_name_free(&px->mapx400, px->mctx);
	px->mctx = NULL;
}

static isc_result_t
additionaldata_in_px(ARGS_ADDLDATA) {
	REQUIRE(rdata->type == dns_rdatatype_px);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(add);
	UNUSED(arg);

	return ISC_R_SUCCESS;
}

static isc_result_t
digest_in_px(ARGS_DIGEST) {
	isc_region_t r1, r2;
	dns_name_t name;
	isc_result_t result;

	REQUIRE(rdata->type == dns_rdatatype_px);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);

	dns_rdata_toregion(rdata, &r1);
	r2 = r1;
	isc_region_consume(&r2, 2);
	r1.length = 2;
	result = (digest)(arg, &r1);
	if (result != ISC_R_SUCCESS) {
		return result;
	}
	dns_name_init(&name);
	dns_name_fromregion(&name, &r2);
	result = dns_name_digest(&name, digest, arg);
	if (result != ISC_R_SUCCESS) {
		return result;
	}
	isc_region_consume(&r2, name_length(&name));
	dns_name_init(&name);
	dns_name_fromregion(&name, &r2);

	return dns_name_digest(&name, digest, arg);
}

static bool
checkowner_in_px(ARGS_CHECKOWNER) {
	REQUIRE(type == dns_rdatatype_px);
	REQUIRE(rdclass == dns_rdataclass_in);

	UNUSED(name);
	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(wildcard);

	return true;
}

static bool
checknames_in_px(ARGS_CHECKNAMES) {
	REQUIRE(rdata->type == dns_rdatatype_px);
	REQUIRE(rdata->rdclass == dns_rdataclass_in);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(bad);

	return true;
}

static int
casecompare_in_px(ARGS_COMPARE) {
	return compare_in_px(rdata1, rdata2);
}

#endif /* RDATA_IN_1_PX_26_C */
