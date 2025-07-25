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

#ifndef RDATA_GENERIC_IPSECKEY_45_C
#define RDATA_GENERIC_IPSECKEY_45_C

#include <string.h>

#include <isc/net.h>

#define RRTYPE_IPSECKEY_ATTRIBUTES (0)

static isc_result_t
fromtext_ipseckey(ARGS_FROMTEXT) {
	isc_token_t token;
	isc_buffer_t buffer;
	unsigned int gateway;
	struct in_addr addr;
	unsigned char addr6[16];
	isc_region_t region;

	REQUIRE(type == dns_rdatatype_ipseckey);

	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(callbacks);

	/*
	 * Precedence.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0xffU) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint8_tobuffer(token.value.as_ulong, target));

	/*
	 * Gateway type.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0x3U) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint8_tobuffer(token.value.as_ulong, target));
	gateway = token.value.as_ulong;

	/*
	 * Algorithm.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0xffU) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint8_tobuffer(token.value.as_ulong, target));

	/*
	 * Gateway.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_string,
				      false));

	switch (gateway) {
	case 0:
		if (strcmp(DNS_AS_STR(token), ".") != 0) {
			RETTOK(DNS_R_SYNTAX);
		}
		break;

	case 1:
		if (inet_pton(AF_INET, DNS_AS_STR(token), &addr) != 1) {
			RETTOK(DNS_R_BADDOTTEDQUAD);
		}
		isc_buffer_availableregion(target, &region);
		if (region.length < 4) {
			return ISC_R_NOSPACE;
		}
		memmove(region.base, &addr, 4);
		isc_buffer_add(target, 4);
		break;

	case 2:
		if (inet_pton(AF_INET6, DNS_AS_STR(token), addr6) != 1) {
			RETTOK(DNS_R_BADAAAA);
		}
		isc_buffer_availableregion(target, &region);
		if (region.length < 16) {
			return ISC_R_NOSPACE;
		}
		memmove(region.base, addr6, 16);
		isc_buffer_add(target, 16);
		break;

	case 3:
		buffer_fromregion(&buffer, &token.value.as_region);
		if (origin == NULL) {
			origin = dns_rootname;
		}
		RETTOK(dns_name_wirefromtext(&buffer, origin, options, target));
		break;
	}

	/*
	 * Public key.
	 */
	return isc_base64_tobuffer(lexer, target, -2);
}

static isc_result_t
totext_ipseckey(ARGS_TOTEXT) {
	isc_region_t region;
	dns_name_t name;
	char buf[sizeof("255 ")];
	unsigned short num;
	unsigned short gateway;

	REQUIRE(rdata->type == dns_rdatatype_ipseckey);
	REQUIRE(rdata->length >= 3);

	dns_name_init(&name);

	if (rdata->data[1] > 3U) {
		return ISC_R_NOTIMPLEMENTED;
	}

	if ((tctx->flags & DNS_STYLEFLAG_MULTILINE) != 0) {
		RETERR(str_totext("( ", target));
	}

	/*
	 * Precedence.
	 */
	dns_rdata_toregion(rdata, &region);
	num = uint8_fromregion(&region);
	isc_region_consume(&region, 1);
	snprintf(buf, sizeof(buf), "%u ", num);
	RETERR(str_totext(buf, target));

	/*
	 * Gateway type.
	 */
	gateway = uint8_fromregion(&region);
	isc_region_consume(&region, 1);
	snprintf(buf, sizeof(buf), "%u ", gateway);
	RETERR(str_totext(buf, target));

	/*
	 * Algorithm.
	 */
	num = uint8_fromregion(&region);
	isc_region_consume(&region, 1);
	snprintf(buf, sizeof(buf), "%u ", num);
	RETERR(str_totext(buf, target));

	/*
	 * Gateway.
	 */
	switch (gateway) {
	case 0:
		RETERR(str_totext(".", target));
		break;

	case 1:
		RETERR(inet_totext(AF_INET, tctx->flags, &region, target));
		isc_region_consume(&region, 4);
		break;

	case 2:
		RETERR(inet_totext(AF_INET6, tctx->flags, &region, target));
		isc_region_consume(&region, 16);
		break;

	case 3:
		dns_name_fromregion(&name, &region);
		RETERR(dns_name_totext(&name, 0, target));
		isc_region_consume(&region, name_length(&name));
		break;
	}

	/*
	 * Key.
	 */
	if (region.length > 0U) {
		RETERR(str_totext(tctx->linebreak, target));
		if (tctx->width == 0) { /* No splitting */
			RETERR(isc_base64_totext(&region, 60, "", target));
		} else {
			RETERR(isc_base64_totext(&region, tctx->width - 2,
						 tctx->linebreak, target));
		}
	}

	if ((tctx->flags & DNS_STYLEFLAG_MULTILINE) != 0) {
		RETERR(str_totext(" )", target));
	}
	return ISC_R_SUCCESS;
}

static isc_result_t
fromwire_ipseckey(ARGS_FROMWIRE) {
	dns_name_t name;
	isc_region_t region;

	REQUIRE(type == dns_rdatatype_ipseckey);

	UNUSED(type);
	UNUSED(rdclass);

	dctx = dns_decompress_setpermitted(dctx, false);

	dns_name_init(&name);

	isc_buffer_activeregion(source, &region);
	if (region.length < 3) {
		return ISC_R_UNEXPECTEDEND;
	}

	switch (region.base[1]) {
	case 0:
		if (region.length < 4) {
			return ISC_R_UNEXPECTEDEND;
		}
		isc_buffer_forward(source, region.length);
		return mem_tobuffer(target, region.base, region.length);

	case 1:
		if (region.length < 8) {
			return ISC_R_UNEXPECTEDEND;
		}
		isc_buffer_forward(source, region.length);
		return mem_tobuffer(target, region.base, region.length);

	case 2:
		if (region.length < 20) {
			return ISC_R_UNEXPECTEDEND;
		}
		isc_buffer_forward(source, region.length);
		return mem_tobuffer(target, region.base, region.length);

	case 3:
		RETERR(mem_tobuffer(target, region.base, 3));
		isc_buffer_forward(source, 3);
		RETERR(dns_name_fromwire(&name, source, dctx, target));
		isc_buffer_activeregion(source, &region);
		isc_buffer_forward(source, region.length);
		if (region.length < 1) {
			return ISC_R_UNEXPECTEDEND;
		}
		return mem_tobuffer(target, region.base, region.length);

	default:
		return ISC_R_NOTIMPLEMENTED;
	}
}

static isc_result_t
towire_ipseckey(ARGS_TOWIRE) {
	isc_region_t region;

	REQUIRE(rdata->type == dns_rdatatype_ipseckey);
	REQUIRE(rdata->length != 0);

	UNUSED(cctx);

	dns_rdata_toregion(rdata, &region);
	return mem_tobuffer(target, region.base, region.length);
}

static int
compare_ipseckey(ARGS_COMPARE) {
	isc_region_t region1;
	isc_region_t region2;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == dns_rdatatype_ipseckey);
	REQUIRE(rdata1->length >= 3);
	REQUIRE(rdata2->length >= 3);

	dns_rdata_toregion(rdata1, &region1);
	dns_rdata_toregion(rdata2, &region2);

	return isc_region_compare(&region1, &region2);
}

static isc_result_t
fromstruct_ipseckey(ARGS_FROMSTRUCT) {
	dns_rdata_ipseckey_t *ipseckey = source;
	isc_region_t region;
	uint32_t n;

	REQUIRE(type == dns_rdatatype_ipseckey);
	REQUIRE(ipseckey != NULL);
	REQUIRE(ipseckey->common.rdtype == type);
	REQUIRE(ipseckey->common.rdclass == rdclass);

	UNUSED(type);
	UNUSED(rdclass);

	if (ipseckey->gateway_type > 3U) {
		return ISC_R_NOTIMPLEMENTED;
	}

	RETERR(uint8_tobuffer(ipseckey->precedence, target));
	RETERR(uint8_tobuffer(ipseckey->gateway_type, target));
	RETERR(uint8_tobuffer(ipseckey->algorithm, target));

	switch (ipseckey->gateway_type) {
	case 0:
		break;

	case 1:
		n = ntohl(ipseckey->in_addr.s_addr);
		RETERR(uint32_tobuffer(n, target));
		break;

	case 2:
		RETERR(mem_tobuffer(target, ipseckey->in6_addr.s6_addr, 16));
		break;

	case 3:
		dns_name_toregion(&ipseckey->gateway, &region);
		RETERR(isc_buffer_copyregion(target, &region));
		break;
	}

	return mem_tobuffer(target, ipseckey->key, ipseckey->keylength);
}

static isc_result_t
tostruct_ipseckey(ARGS_TOSTRUCT) {
	isc_region_t region;
	dns_rdata_ipseckey_t *ipseckey = target;
	dns_name_t name;
	uint32_t n;

	REQUIRE(rdata->type == dns_rdatatype_ipseckey);
	REQUIRE(ipseckey != NULL);
	REQUIRE(rdata->length >= 3);

	ipseckey->common.rdclass = rdata->rdclass;
	ipseckey->common.rdtype = rdata->type;

	dns_name_init(&name);
	dns_rdata_toregion(rdata, &region);

	ipseckey->precedence = uint8_fromregion(&region);
	isc_region_consume(&region, 1);

	ipseckey->gateway_type = uint8_fromregion(&region);
	isc_region_consume(&region, 1);

	ipseckey->algorithm = uint8_fromregion(&region);
	isc_region_consume(&region, 1);

	switch (ipseckey->gateway_type) {
	case 0:
		break;

	case 1:
		n = uint32_fromregion(&region);
		ipseckey->in_addr.s_addr = htonl(n);
		isc_region_consume(&region, 4);
		break;

	case 2:
		INSIST(region.length >= 16U);
		memmove(ipseckey->in6_addr.s6_addr, region.base, 16);
		isc_region_consume(&region, 16);
		break;

	case 3:
		dns_name_init(&ipseckey->gateway);
		dns_name_fromregion(&name, &region);
		name_duporclone(&name, mctx, &ipseckey->gateway);
		isc_region_consume(&region, name_length(&name));
		break;
	}

	ipseckey->keylength = region.length;
	if (ipseckey->keylength != 0U) {
		ipseckey->key = mem_maybedup(mctx, region.base,
					     ipseckey->keylength);
	} else {
		ipseckey->key = NULL;
	}

	ipseckey->mctx = mctx;
	return ISC_R_SUCCESS;
}

static void
freestruct_ipseckey(ARGS_FREESTRUCT) {
	dns_rdata_ipseckey_t *ipseckey = source;

	REQUIRE(ipseckey != NULL);
	REQUIRE(ipseckey->common.rdtype == dns_rdatatype_ipseckey);

	if (ipseckey->mctx == NULL) {
		return;
	}

	if (ipseckey->gateway_type == 3) {
		dns_name_free(&ipseckey->gateway, ipseckey->mctx);
	}

	if (ipseckey->key != NULL) {
		isc_mem_free(ipseckey->mctx, ipseckey->key);
	}

	ipseckey->mctx = NULL;
}

static isc_result_t
additionaldata_ipseckey(ARGS_ADDLDATA) {
	REQUIRE(rdata->type == dns_rdatatype_ipseckey);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(add);
	UNUSED(arg);

	return ISC_R_SUCCESS;
}

static isc_result_t
digest_ipseckey(ARGS_DIGEST) {
	isc_region_t region;

	REQUIRE(rdata->type == dns_rdatatype_ipseckey);

	dns_rdata_toregion(rdata, &region);
	return (digest)(arg, &region);
}

static bool
checkowner_ipseckey(ARGS_CHECKOWNER) {
	REQUIRE(type == dns_rdatatype_ipseckey);

	UNUSED(name);
	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(wildcard);

	return true;
}

static bool
checknames_ipseckey(ARGS_CHECKNAMES) {
	REQUIRE(rdata->type == dns_rdatatype_ipseckey);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(bad);

	return true;
}

static int
casecompare_ipseckey(ARGS_COMPARE) {
	isc_region_t region1;
	isc_region_t region2;
	dns_name_t name1;
	dns_name_t name2;
	int order;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == dns_rdatatype_ipseckey);
	REQUIRE(rdata1->length >= 3);
	REQUIRE(rdata2->length >= 3);

	dns_rdata_toregion(rdata1, &region1);
	dns_rdata_toregion(rdata2, &region2);

	if (memcmp(region1.base, region2.base, 3) != 0 || region1.base[1] != 3)
	{
		return isc_region_compare(&region1, &region2);
	}

	dns_name_init(&name1);
	dns_name_init(&name2);

	isc_region_consume(&region1, 3);
	isc_region_consume(&region2, 3);

	dns_name_fromregion(&name1, &region1);
	dns_name_fromregion(&name2, &region2);

	order = dns_name_rdatacompare(&name1, &name2);
	if (order != 0) {
		return order;
	}

	isc_region_consume(&region1, name_length(&name1));
	isc_region_consume(&region2, name_length(&name2));

	return isc_region_compare(&region1, &region2);
}

#endif /* RDATA_GENERIC_IPSECKEY_45_C */
