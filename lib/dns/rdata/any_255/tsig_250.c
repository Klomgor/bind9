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

#ifndef RDATA_ANY_255_TSIG_250_C
#define RDATA_ANY_255_TSIG_250_C

#define RRTYPE_TSIG_ATTRIBUTES \
	(DNS_RDATATYPEATTR_META | DNS_RDATATYPEATTR_NOTQUESTION)

static isc_result_t
fromtext_any_tsig(ARGS_FROMTEXT) {
	isc_token_t token;
	uint64_t sigtime;
	isc_buffer_t buffer;
	dns_rcode_t rcode;
	long i;
	char *e;

	REQUIRE(type == dns_rdatatype_tsig);
	REQUIRE(rdclass == dns_rdataclass_any);

	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(callbacks);

	/*
	 * Algorithm Name.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_string,
				      false));
	buffer_fromregion(&buffer, &token.value.as_region);
	if (origin == NULL) {
		origin = dns_rootname;
	}
	RETTOK(dns_name_wirefromtext(&buffer, origin, options, target));

	/*
	 * Time Signed: 48 bits.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_string,
				      false));
	sigtime = strtoull(DNS_AS_STR(token), &e, 10);
	if (*e != 0) {
		RETTOK(DNS_R_SYNTAX);
	}
	if ((sigtime >> 48) != 0) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint16_tobuffer((uint16_t)(sigtime >> 32), target));
	RETERR(uint32_tobuffer((uint32_t)(sigtime & 0xffffffffU), target));

	/*
	 * Fudge.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0xffffU) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint16_tobuffer(token.value.as_ulong, target));

	/*
	 * Signature Size.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0xffffU) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint16_tobuffer(token.value.as_ulong, target));

	/*
	 * Signature.
	 */
	RETERR(isc_base64_tobuffer(lexer, target, (int)token.value.as_ulong));

	/*
	 * Original ID.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0xffffU) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint16_tobuffer(token.value.as_ulong, target));

	/*
	 * Error.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_string,
				      false));
	if (dns_tsigrcode_fromtext(&rcode, &token.value.as_textregion) !=
	    ISC_R_SUCCESS)
	{
		i = strtol(DNS_AS_STR(token), &e, 10);
		if (*e != 0) {
			RETTOK(DNS_R_UNKNOWN);
		}
		if (i < 0 || i > 0xffff) {
			RETTOK(ISC_R_RANGE);
		}
		rcode = (dns_rcode_t)i;
	}
	RETERR(uint16_tobuffer(rcode, target));

	/*
	 * Other Len.
	 */
	RETERR(isc_lex_getmastertoken(lexer, &token, isc_tokentype_number,
				      false));
	if (token.value.as_ulong > 0xffffU) {
		RETTOK(ISC_R_RANGE);
	}
	RETERR(uint16_tobuffer(token.value.as_ulong, target));

	/*
	 * Other Data.
	 */
	return isc_base64_tobuffer(lexer, target, (int)token.value.as_ulong);
}

static isc_result_t
totext_any_tsig(ARGS_TOTEXT) {
	isc_region_t sr;
	isc_region_t sigr;
	char buf[sizeof(" 281474976710655 ")];
	char *bufp;
	dns_name_t name;
	dns_name_t prefix;
	uint64_t sigtime;
	unsigned short n;
	unsigned int opts;

	REQUIRE(rdata->type == dns_rdatatype_tsig);
	REQUIRE(rdata->rdclass == dns_rdataclass_any);
	REQUIRE(rdata->length != 0);

	dns_rdata_toregion(rdata, &sr);
	/*
	 * Algorithm Name.
	 */
	dns_name_init(&name);
	dns_name_init(&prefix);
	dns_name_fromregion(&name, &sr);
	opts = name_prefix(&name, tctx->origin, &prefix) ? DNS_NAME_OMITFINALDOT
							 : 0;
	RETERR(dns_name_totext(&prefix, opts, target));
	RETERR(str_totext(" ", target));
	isc_region_consume(&sr, name_length(&name));

	/*
	 * Time Signed.
	 */
	sigtime = ((uint64_t)sr.base[0] << 40) | ((uint64_t)sr.base[1] << 32) |
		  ((uint64_t)sr.base[2] << 24) | ((uint64_t)sr.base[3] << 16) |
		  ((uint64_t)sr.base[4] << 8) | (uint64_t)sr.base[5];
	isc_region_consume(&sr, 6);
	bufp = &buf[sizeof(buf) - 1];
	*bufp-- = 0;
	*bufp-- = ' ';
	do {
		*bufp-- = '0' + sigtime % 10;
		sigtime /= 10;
	} while (sigtime != 0);
	bufp++;
	RETERR(str_totext(bufp, target));

	/*
	 * Fudge.
	 */
	n = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);
	snprintf(buf, sizeof(buf), "%u ", n);
	RETERR(str_totext(buf, target));

	/*
	 * Signature Size.
	 */
	n = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);
	snprintf(buf, sizeof(buf), "%u", n);
	RETERR(str_totext(buf, target));

	/*
	 * Signature.
	 */
	if (n != 0U) {
		REQUIRE(n <= sr.length);
		sigr = sr;
		sigr.length = n;
		if ((tctx->flags & DNS_STYLEFLAG_MULTILINE) != 0) {
			RETERR(str_totext(" (", target));
		}
		RETERR(str_totext(tctx->linebreak, target));
		if (tctx->width == 0) { /* No splitting */
			RETERR(isc_base64_totext(&sigr, 60, "", target));
		} else {
			RETERR(isc_base64_totext(&sigr, tctx->width - 2,
						 tctx->linebreak, target));
		}
		if ((tctx->flags & DNS_STYLEFLAG_MULTILINE) != 0) {
			RETERR(str_totext(" ) ", target));
		} else {
			RETERR(str_totext(" ", target));
		}
		isc_region_consume(&sr, n);
	} else {
		RETERR(str_totext(" ", target));
	}

	/*
	 * Original ID.
	 */
	n = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);
	snprintf(buf, sizeof(buf), "%u ", n);
	RETERR(str_totext(buf, target));

	/*
	 * Error.
	 */
	n = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);
	RETERR(dns_tsigrcode_totext((dns_rcode_t)n, target));

	/*
	 * Other Size.
	 */
	n = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);
	snprintf(buf, sizeof(buf), " %u ", n);
	RETERR(str_totext(buf, target));

	/*
	 * Other.
	 */
	if (tctx->width == 0) { /* No splitting */
		return isc_base64_totext(&sr, 60, "", target);
	} else {
		return isc_base64_totext(&sr, 60, " ", target);
	}
}

static isc_result_t
fromwire_any_tsig(ARGS_FROMWIRE) {
	isc_region_t sr;
	dns_name_t name;
	unsigned long n;

	REQUIRE(type == dns_rdatatype_tsig);
	REQUIRE(rdclass == dns_rdataclass_any);

	UNUSED(type);
	UNUSED(rdclass);

	dctx = dns_decompress_setpermitted(dctx, false);

	/*
	 * Algorithm Name.
	 */
	dns_name_init(&name);
	RETERR(dns_name_fromwire(&name, source, dctx, target));

	isc_buffer_activeregion(source, &sr);
	/*
	 * Time Signed + Fudge.
	 */
	if (sr.length < 8) {
		return ISC_R_UNEXPECTEDEND;
	}
	RETERR(mem_tobuffer(target, sr.base, 8));
	isc_region_consume(&sr, 8);
	isc_buffer_forward(source, 8);

	/*
	 * Signature Length + Signature.
	 */
	if (sr.length < 2) {
		return ISC_R_UNEXPECTEDEND;
	}
	n = uint16_fromregion(&sr);
	if (sr.length < n + 2) {
		return ISC_R_UNEXPECTEDEND;
	}
	RETERR(mem_tobuffer(target, sr.base, n + 2));
	isc_region_consume(&sr, n + 2);
	isc_buffer_forward(source, n + 2);

	/*
	 * Original ID + Error.
	 */
	if (sr.length < 4) {
		return ISC_R_UNEXPECTEDEND;
	}
	RETERR(mem_tobuffer(target, sr.base, 4));
	isc_region_consume(&sr, 4);
	isc_buffer_forward(source, 4);

	/*
	 * Other Length + Other.
	 */
	if (sr.length < 2) {
		return ISC_R_UNEXPECTEDEND;
	}
	n = uint16_fromregion(&sr);
	if (sr.length < n + 2) {
		return ISC_R_UNEXPECTEDEND;
	}
	isc_buffer_forward(source, n + 2);
	return mem_tobuffer(target, sr.base, n + 2);
}

static isc_result_t
towire_any_tsig(ARGS_TOWIRE) {
	isc_region_t sr;
	dns_name_t name;

	REQUIRE(rdata->type == dns_rdatatype_tsig);
	REQUIRE(rdata->rdclass == dns_rdataclass_any);
	REQUIRE(rdata->length != 0);

	dns_compress_setpermitted(cctx, false);
	dns_rdata_toregion(rdata, &sr);
	dns_name_init(&name);
	dns_name_fromregion(&name, &sr);
	RETERR(dns_name_towire(&name, cctx, target));
	isc_region_consume(&sr, name_length(&name));
	return mem_tobuffer(target, sr.base, sr.length);
}

static int
compare_any_tsig(ARGS_COMPARE) {
	isc_region_t r1;
	isc_region_t r2;
	dns_name_t name1;
	dns_name_t name2;
	int order;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == dns_rdatatype_tsig);
	REQUIRE(rdata1->rdclass == dns_rdataclass_any);
	REQUIRE(rdata1->length != 0);
	REQUIRE(rdata2->length != 0);

	dns_rdata_toregion(rdata1, &r1);
	dns_rdata_toregion(rdata2, &r2);
	dns_name_init(&name1);
	dns_name_init(&name2);
	dns_name_fromregion(&name1, &r1);
	dns_name_fromregion(&name2, &r2);
	order = dns_name_rdatacompare(&name1, &name2);
	if (order != 0) {
		return order;
	}
	isc_region_consume(&r1, name_length(&name1));
	isc_region_consume(&r2, name_length(&name2));
	return isc_region_compare(&r1, &r2);
}

static isc_result_t
fromstruct_any_tsig(ARGS_FROMSTRUCT) {
	dns_rdata_any_tsig_t *tsig = source;
	isc_region_t tr;

	REQUIRE(type == dns_rdatatype_tsig);
	REQUIRE(rdclass == dns_rdataclass_any);
	REQUIRE(tsig != NULL);
	REQUIRE(tsig->common.rdclass == rdclass);
	REQUIRE(tsig->common.rdtype == type);

	UNUSED(type);
	UNUSED(rdclass);

	/*
	 * Algorithm Name.
	 */
	RETERR(name_tobuffer(&tsig->algorithm, target));

	isc_buffer_availableregion(target, &tr);
	if (tr.length < 6 + 2 + 2) {
		return ISC_R_NOSPACE;
	}

	/*
	 * Time Signed: 48 bits.
	 */
	RETERR(uint16_tobuffer((uint16_t)(tsig->timesigned >> 32), target));
	RETERR(uint32_tobuffer((uint32_t)(tsig->timesigned & 0xffffffffU),
			       target));

	/*
	 * Fudge.
	 */
	RETERR(uint16_tobuffer(tsig->fudge, target));

	/*
	 * Signature Size.
	 */
	RETERR(uint16_tobuffer(tsig->siglen, target));

	/*
	 * Signature.
	 */
	RETERR(mem_tobuffer(target, tsig->signature, tsig->siglen));

	isc_buffer_availableregion(target, &tr);
	if (tr.length < 2 + 2 + 2) {
		return ISC_R_NOSPACE;
	}

	/*
	 * Original ID.
	 */
	RETERR(uint16_tobuffer(tsig->originalid, target));

	/*
	 * Error.
	 */
	RETERR(uint16_tobuffer(tsig->error, target));

	/*
	 * Other Len.
	 */
	RETERR(uint16_tobuffer(tsig->otherlen, target));

	/*
	 * Other Data.
	 */
	return mem_tobuffer(target, tsig->other, tsig->otherlen);
}

static isc_result_t
tostruct_any_tsig(ARGS_TOSTRUCT) {
	dns_rdata_any_tsig_t *tsig;
	dns_name_t alg;
	isc_region_t sr;

	REQUIRE(rdata->type == dns_rdatatype_tsig);
	REQUIRE(rdata->rdclass == dns_rdataclass_any);
	REQUIRE(rdata->length != 0);

	tsig = (dns_rdata_any_tsig_t *)target;
	tsig->common.rdclass = rdata->rdclass;
	tsig->common.rdtype = rdata->type;

	dns_rdata_toregion(rdata, &sr);

	/*
	 * Algorithm Name.
	 */
	dns_name_init(&alg);
	dns_name_fromregion(&alg, &sr);
	dns_name_init(&tsig->algorithm);
	name_duporclone(&alg, mctx, &tsig->algorithm);

	isc_region_consume(&sr, name_length(&tsig->algorithm));

	/*
	 * Time Signed.
	 */
	INSIST(sr.length >= 6);
	tsig->timesigned = ((uint64_t)sr.base[0] << 40) |
			   ((uint64_t)sr.base[1] << 32) |
			   ((uint64_t)sr.base[2] << 24) |
			   ((uint64_t)sr.base[3] << 16) |
			   ((uint64_t)sr.base[4] << 8) | (uint64_t)sr.base[5];
	isc_region_consume(&sr, 6);

	/*
	 * Fudge.
	 */
	tsig->fudge = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);

	/*
	 * Signature Size.
	 */
	tsig->siglen = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);

	/*
	 * Signature.
	 */
	INSIST(sr.length >= tsig->siglen);
	tsig->signature = mem_maybedup(mctx, sr.base, tsig->siglen);
	isc_region_consume(&sr, tsig->siglen);

	/*
	 * Original ID.
	 */
	tsig->originalid = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);

	/*
	 * Error.
	 */
	tsig->error = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);

	/*
	 * Other Size.
	 */
	tsig->otherlen = uint16_fromregion(&sr);
	isc_region_consume(&sr, 2);

	/*
	 * Other.
	 */
	INSIST(sr.length == tsig->otherlen);
	tsig->other = mem_maybedup(mctx, sr.base, tsig->otherlen);

	tsig->mctx = mctx;
	return ISC_R_SUCCESS;
}

static void
freestruct_any_tsig(ARGS_FREESTRUCT) {
	dns_rdata_any_tsig_t *tsig = (dns_rdata_any_tsig_t *)source;

	REQUIRE(tsig != NULL);
	REQUIRE(tsig->common.rdtype == dns_rdatatype_tsig);
	REQUIRE(tsig->common.rdclass == dns_rdataclass_any);

	if (tsig->mctx == NULL) {
		return;
	}

	dns_name_free(&tsig->algorithm, tsig->mctx);
	if (tsig->signature != NULL) {
		isc_mem_free(tsig->mctx, tsig->signature);
	}
	if (tsig->other != NULL) {
		isc_mem_free(tsig->mctx, tsig->other);
	}
	tsig->mctx = NULL;
}

static isc_result_t
additionaldata_any_tsig(ARGS_ADDLDATA) {
	REQUIRE(rdata->type == dns_rdatatype_tsig);
	REQUIRE(rdata->rdclass == dns_rdataclass_any);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(add);
	UNUSED(arg);

	return ISC_R_SUCCESS;
}

static isc_result_t
digest_any_tsig(ARGS_DIGEST) {
	REQUIRE(rdata->type == dns_rdatatype_tsig);
	REQUIRE(rdata->rdclass == dns_rdataclass_any);

	UNUSED(rdata);
	UNUSED(digest);
	UNUSED(arg);

	return ISC_R_NOTIMPLEMENTED;
}

static bool
checkowner_any_tsig(ARGS_CHECKOWNER) {
	REQUIRE(type == dns_rdatatype_tsig);
	REQUIRE(rdclass == dns_rdataclass_any);

	UNUSED(name);
	UNUSED(type);
	UNUSED(rdclass);
	UNUSED(wildcard);

	return true;
}

static bool
checknames_any_tsig(ARGS_CHECKNAMES) {
	REQUIRE(rdata->type == dns_rdatatype_tsig);
	REQUIRE(rdata->rdclass == dns_rdataclass_any);

	UNUSED(rdata);
	UNUSED(owner);
	UNUSED(bad);

	return true;
}

static int
casecompare_any_tsig(ARGS_COMPARE) {
	return compare_any_tsig(rdata1, rdata2);
}

#endif /* RDATA_ANY_255_TSIG_250_C */
