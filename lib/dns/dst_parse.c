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
 * Copyright (C) Network Associates, Inc.
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

#include "dst_parse.h"
#include <inttypes.h>
#include <stdbool.h>
#include <unistd.h>

#include <isc/base64.h>
#include <isc/dir.h>
#include <isc/file.h>
#include <isc/lex.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/stdtime.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/time.h>

#include "dst_internal.h"
#include "isc/result.h"

#define DST_AS_STR(t) ((t).value.as_textregion.base)

#define PRIVATE_KEY_STR "Private-key-format:"
#define ALGORITHM_STR	"Algorithm:"

#define TIMING_NTAGS (DST_MAX_TIMES + 1)
static const char *timetags[TIMING_NTAGS] = {
	"Created:",    "Publish:", "Activate:",	 "Revoke:",
	"Inactive:",   "Delete:",  "DSPublish:", "SyncPublish:",
	"SyncDelete:", NULL,	   NULL,	 NULL,
	NULL
};

#define NUMERIC_NTAGS (DST_MAX_NUMERIC + 1)
static const char *numerictags[NUMERIC_NTAGS] = {
	"Predecessor:", "Successor:", "MaxTTL:", "RollPeriod:", NULL, NULL, NULL
};

struct parse_map {
	const int value;
	const char *tag;
};

static struct parse_map map[] = { { TAG_RSA_MODULUS, "Modulus:" },
				  { TAG_RSA_PUBLICEXPONENT, "PublicExponent:" },
				  { TAG_RSA_PRIVATEEXPONENT, "PrivateExponent"
							     ":" },
				  { TAG_RSA_PRIME1, "Prime1:" },
				  { TAG_RSA_PRIME2, "Prime2:" },
				  { TAG_RSA_EXPONENT1, "Exponent1:" },
				  { TAG_RSA_EXPONENT2, "Exponent2:" },
				  { TAG_RSA_COEFFICIENT, "Coefficient:" },
				  { TAG_RSA_ENGINE, "Engine:" },
				  { TAG_RSA_LABEL, "Label:" },

				  { TAG_ECDSA_PRIVATEKEY, "PrivateKey:" },
				  { TAG_ECDSA_ENGINE, "Engine:" },
				  { TAG_ECDSA_LABEL, "Label:" },

				  { TAG_EDDSA_PRIVATEKEY, "PrivateKey:" },
				  { TAG_EDDSA_ENGINE, "Engine:" },
				  { TAG_EDDSA_LABEL, "Label:" },

				  { TAG_HMACMD5_KEY, "Key:" },
				  { TAG_HMACMD5_BITS, "Bits:" },

				  { TAG_HMACSHA1_KEY, "Key:" },
				  { TAG_HMACSHA1_BITS, "Bits:" },

				  { TAG_HMACSHA224_KEY, "Key:" },
				  { TAG_HMACSHA224_BITS, "Bits:" },

				  { TAG_HMACSHA256_KEY, "Key:" },
				  { TAG_HMACSHA256_BITS, "Bits:" },

				  { TAG_HMACSHA384_KEY, "Key:" },
				  { TAG_HMACSHA384_BITS, "Bits:" },

				  { TAG_HMACSHA512_KEY, "Key:" },
				  { TAG_HMACSHA512_BITS, "Bits:" },

				  { 0, NULL } };

static int
find_value(const char *s, const unsigned int alg) {
	int i;

	for (i = 0; map[i].tag != NULL; i++) {
		if (strcasecmp(s, map[i].tag) == 0 &&
		    (TAG_ALG(map[i].value) == alg))
		{
			return map[i].value;
		}
	}
	return -1;
}

static const char *
find_tag(const int value) {
	int i;

	for (i = 0;; i++) {
		if (map[i].tag == NULL) {
			return NULL;
		} else if (value == map[i].value) {
			return map[i].tag;
		}
	}
}

static int
find_metadata(const char *s, const char *tags[], int ntags) {
	int i;

	for (i = 0; i < ntags; i++) {
		if (tags[i] != NULL && strcasecmp(s, tags[i]) == 0) {
			return i;
		}
	}

	return -1;
}

static int
find_timedata(const char *s) {
	return find_metadata(s, timetags, TIMING_NTAGS);
}

static int
find_numericdata(const char *s) {
	return find_metadata(s, numerictags, NUMERIC_NTAGS);
}

static int
check_rsa(const dst_private_t *priv, bool external) {
	int i, j;
	bool have[RSA_NTAGS];
	bool ok;
	unsigned int mask;

	if (external) {
		return (priv->nelements == 0) ? 0 : -1;
	}

	for (i = 0; i < RSA_NTAGS; i++) {
		have[i] = false;
	}

	for (j = 0; j < priv->nelements; j++) {
		for (i = 0; i < RSA_NTAGS; i++) {
			if (priv->elements[j].tag == TAG(DST_ALG_RSA, i)) {
				break;
			}
		}
		if (i == RSA_NTAGS) {
			return -1;
		}
		have[i] = true;
	}

	mask = (1ULL << TAG_SHIFT) - 1;

	if (have[TAG_RSA_LABEL & mask]) {
		ok = have[TAG_RSA_MODULUS & mask] &&
		     have[TAG_RSA_PUBLICEXPONENT & mask];
	} else {
		ok = have[TAG_RSA_MODULUS & mask] &&
		     have[TAG_RSA_PUBLICEXPONENT & mask] &&
		     have[TAG_RSA_PRIVATEEXPONENT & mask] &&
		     have[TAG_RSA_PRIME1 & mask] &&
		     have[TAG_RSA_PRIME2 & mask] &&
		     have[TAG_RSA_EXPONENT1 & mask] &&
		     have[TAG_RSA_EXPONENT2 & mask] &&
		     have[TAG_RSA_COEFFICIENT & mask];
	}
	return ok ? 0 : -1;
}

static int
check_ecdsa(const dst_private_t *priv, bool external) {
	int i, j;
	bool have[ECDSA_NTAGS];
	bool ok;
	unsigned int mask;

	if (external) {
		return (priv->nelements == 0) ? 0 : -1;
	}

	for (i = 0; i < ECDSA_NTAGS; i++) {
		have[i] = false;
	}
	for (j = 0; j < priv->nelements; j++) {
		for (i = 0; i < ECDSA_NTAGS; i++) {
			if (priv->elements[j].tag == TAG(DST_ALG_ECDSA256, i)) {
				break;
			}
		}
		if (i == ECDSA_NTAGS) {
			return -1;
		}
		have[i] = true;
	}

	mask = (1ULL << TAG_SHIFT) - 1;

	ok = have[TAG_ECDSA_LABEL & mask] || have[TAG_ECDSA_PRIVATEKEY & mask];

	return ok ? 0 : -1;
}

static int
check_eddsa(const dst_private_t *priv, bool external) {
	int i, j;
	bool have[EDDSA_NTAGS];
	bool ok;
	unsigned int mask;

	if (external) {
		return (priv->nelements == 0) ? 0 : -1;
	}

	for (i = 0; i < EDDSA_NTAGS; i++) {
		have[i] = false;
	}
	for (j = 0; j < priv->nelements; j++) {
		for (i = 0; i < EDDSA_NTAGS; i++) {
			if (priv->elements[j].tag == TAG(DST_ALG_ED25519, i)) {
				break;
			}
		}
		if (i == EDDSA_NTAGS) {
			return -1;
		}
		have[i] = true;
	}

	mask = (1ULL << TAG_SHIFT) - 1;

	ok = have[TAG_EDDSA_LABEL & mask] || have[TAG_EDDSA_PRIVATEKEY & mask];

	return ok ? 0 : -1;
}

static int
check_hmac_md5(const dst_private_t *priv, bool old) {
	int i, j;

	if (priv->nelements != HMACMD5_NTAGS) {
		/*
		 * If this is a good old format and we are accepting
		 * the old format return success.
		 */
		if (old && priv->nelements == OLD_HMACMD5_NTAGS &&
		    priv->elements[0].tag == TAG_HMACMD5_KEY)
		{
			return 0;
		}
		return -1;
	}
	/*
	 * We must be new format at this point.
	 */
	for (i = 0; i < HMACMD5_NTAGS; i++) {
		for (j = 0; j < priv->nelements; j++) {
			if (priv->elements[j].tag == TAG(DST_ALG_HMACMD5, i)) {
				break;
			}
		}
		if (j == priv->nelements) {
			return -1;
		}
	}
	return 0;
}

static int
check_hmac_sha(const dst_private_t *priv, unsigned int ntags,
	       unsigned int alg) {
	unsigned int i, j;
	if (priv->nelements != ntags) {
		return -1;
	}
	for (i = 0; i < ntags; i++) {
		for (j = 0; j < priv->nelements; j++) {
			if (priv->elements[j].tag == TAG(alg, i)) {
				break;
			}
		}
		if (j == priv->nelements) {
			return -1;
		}
	}
	return 0;
}

static int
check_data(const dst_private_t *priv, const unsigned int alg, bool old,
	   bool external) {
	switch (alg) {
	case DST_ALG_RSA:
	case DST_ALG_RSASHA1:
	case DST_ALG_NSEC3RSASHA1:
	case DST_ALG_RSASHA256:
	case DST_ALG_RSASHA512:
	case DST_ALG_RSASHA256PRIVATEOID:
	case DST_ALG_RSASHA512PRIVATEOID:
		return check_rsa(priv, external);
	case DST_ALG_ECDSA256:
	case DST_ALG_ECDSA384:
		return check_ecdsa(priv, external);
	case DST_ALG_ED25519:
	case DST_ALG_ED448:
		return check_eddsa(priv, external);
	case DST_ALG_HMACMD5:
		return check_hmac_md5(priv, old);
	case DST_ALG_HMACSHA1:
		return check_hmac_sha(priv, HMACSHA1_NTAGS, alg);
	case DST_ALG_HMACSHA224:
		return check_hmac_sha(priv, HMACSHA224_NTAGS, alg);
	case DST_ALG_HMACSHA256:
		return check_hmac_sha(priv, HMACSHA256_NTAGS, alg);
	case DST_ALG_HMACSHA384:
		return check_hmac_sha(priv, HMACSHA384_NTAGS, alg);
	case DST_ALG_HMACSHA512:
		return check_hmac_sha(priv, HMACSHA512_NTAGS, alg);
	default:
		return DST_R_UNSUPPORTEDALG;
	}
}

void
dst__privstruct_free(dst_private_t *priv, isc_mem_t *mctx) {
	int i;

	if (priv == NULL) {
		return;
	}
	for (i = 0; i < priv->nelements; i++) {
		if (priv->elements[i].data == NULL) {
			continue;
		}
		memset(priv->elements[i].data, 0, MAXFIELDSIZE);
		isc_mem_put(mctx, priv->elements[i].data, MAXFIELDSIZE);
	}
	priv->nelements = 0;
}

isc_result_t
dst__privstruct_parse(dst_key_t *key, unsigned int alg, isc_lex_t *lex,
		      isc_mem_t *mctx, dst_private_t *priv) {
	int n = 0, major, minor, check;
	isc_buffer_t b;
	isc_token_t token;
	unsigned char *data = NULL;
	unsigned int opt = ISC_LEXOPT_EOL;
	isc_stdtime_t when;
	isc_result_t ret;
	bool external = false;

	REQUIRE(priv != NULL);

	priv->nelements = 0;
	memset(priv->elements, 0, sizeof(priv->elements));

#define NEXTTOKEN(lex, opt, token)                       \
	do {                                             \
		ret = isc_lex_gettoken(lex, opt, token); \
		if (ret != ISC_R_SUCCESS)                \
			goto fail;                       \
	} while (0)

#define READLINE(lex, opt, token)                        \
	do {                                             \
		ret = isc_lex_gettoken(lex, opt, token); \
		if (ret == ISC_R_EOF)                    \
			break;                           \
		else if (ret != ISC_R_SUCCESS)           \
			goto fail;                       \
	} while ((*token).type != isc_tokentype_eol)

	/*
	 * Read the description line.
	 */
	NEXTTOKEN(lex, opt, &token);
	if (token.type != isc_tokentype_string ||
	    strcmp(DST_AS_STR(token), PRIVATE_KEY_STR) != 0)
	{
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	}

	NEXTTOKEN(lex, opt, &token);
	if (token.type != isc_tokentype_string || (DST_AS_STR(token))[0] != 'v')
	{
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	}
	if (sscanf(DST_AS_STR(token), "v%d.%d", &major, &minor) != 2) {
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	}

	if (major > DST_MAJOR_VERSION) {
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	}

	/*
	 * Store the private key format version number
	 */
	dst_key_setprivateformat(key, major, minor);

	READLINE(lex, opt, &token);

	/*
	 * Read the algorithm line.
	 */
	NEXTTOKEN(lex, opt, &token);
	if (token.type != isc_tokentype_string ||
	    strcmp(DST_AS_STR(token), ALGORITHM_STR) != 0)
	{
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	}

	NEXTTOKEN(lex, opt | ISC_LEXOPT_NUMBER, &token);
	if (token.type != isc_tokentype_number ||
	    token.value.as_ulong != (unsigned long)dst_key_alg(key))
	{
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	}

	READLINE(lex, opt, &token);

	/*
	 * Read the key data.
	 */
	for (n = 0; n < MAXFIELDS; n++) {
		int tag;
		isc_region_t r;
		do {
			ret = isc_lex_gettoken(lex, opt, &token);
			if (ret == ISC_R_EOF) {
				goto done;
			}
			if (ret != ISC_R_SUCCESS) {
				goto fail;
			}
		} while (token.type == isc_tokentype_eol);

		if (token.type != isc_tokentype_string) {
			ret = DST_R_INVALIDPRIVATEKEY;
			goto fail;
		}

		if (strcmp(DST_AS_STR(token), "External:") == 0) {
			external = true;
			goto next;
		}

		/* Numeric metadata */
		tag = find_numericdata(DST_AS_STR(token));
		if (tag >= 0) {
			INSIST(tag < NUMERIC_NTAGS);

			NEXTTOKEN(lex, opt | ISC_LEXOPT_NUMBER, &token);
			if (token.type != isc_tokentype_number) {
				ret = DST_R_INVALIDPRIVATEKEY;
				goto fail;
			}

			dst_key_setnum(key, tag, token.value.as_ulong);
			goto next;
		}

		/* Timing metadata */
		tag = find_timedata(DST_AS_STR(token));
		if (tag >= 0) {
			INSIST(tag < TIMING_NTAGS);

			NEXTTOKEN(lex, opt, &token);
			if (token.type != isc_tokentype_string) {
				ret = DST_R_INVALIDPRIVATEKEY;
				goto fail;
			}

			ret = dns_time32_fromtext(DST_AS_STR(token), &when);
			if (ret != ISC_R_SUCCESS) {
				goto fail;
			}

			dst_key_settime(key, tag, when);

			goto next;
		}

		/* Key data */
		tag = find_value(DST_AS_STR(token), alg);
		if (tag < 0 && minor > DST_MINOR_VERSION) {
			goto next;
		} else if (tag < 0) {
			ret = DST_R_INVALIDPRIVATEKEY;
			goto fail;
		}

		priv->elements[n].tag = tag;

		data = isc_mem_get(mctx, MAXFIELDSIZE);

		isc_buffer_init(&b, data, MAXFIELDSIZE);
		ret = isc_base64_tobuffer(lex, &b, -1);
		if (ret != ISC_R_SUCCESS) {
			goto fail;
		}

		isc_buffer_usedregion(&b, &r);
		priv->elements[n].length = r.length;
		priv->elements[n].data = r.base;
		priv->nelements++;

	next:
		READLINE(lex, opt, &token);
		data = NULL;
	}

done:
	if (external && priv->nelements != 0) {
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	}

	check = check_data(priv, alg, true, external);
	if (check < 0) {
		ret = DST_R_INVALIDPRIVATEKEY;
		goto fail;
	} else if (check != ISC_R_SUCCESS) {
		ret = check;
		goto fail;
	}

	key->external = external;

	return ISC_R_SUCCESS;

fail:
	dst__privstruct_free(priv, mctx);
	if (data != NULL) {
		isc_mem_put(mctx, data, MAXFIELDSIZE);
	}

	return ret;
}

isc_result_t
dst__privstruct_writefile(const dst_key_t *key, const dst_private_t *priv,
			  const char *directory) {
	FILE *fp;
	isc_result_t result;
	char filename[NAME_MAX];
	char tmpname[NAME_MAX];
	char buffer[MAXFIELDSIZE * 2];
	isc_stdtime_t when;
	uint32_t value;
	isc_buffer_t b;
	isc_buffer_t fileb;
	isc_buffer_t tmpb;
	isc_region_t r;
	int major, minor;
	mode_t mode;
	int i, ret;

	REQUIRE(priv != NULL);

	ret = check_data(priv, dst_key_alg(key), false, key->external);
	if (ret < 0) {
		return DST_R_INVALIDPRIVATEKEY;
	} else if (ret != ISC_R_SUCCESS) {
		return ret;
	}

	isc_buffer_init(&fileb, filename, sizeof(filename));
	result = dst_key_buildfilename(key, DST_TYPE_PRIVATE, directory,
				       &fileb);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	result = isc_file_mode(filename, &mode);
	if (result == ISC_R_SUCCESS && mode != (S_IRUSR | S_IWUSR)) {
		/* File exists; warn that we are changing its permissions */
		int level;

		level = ISC_LOG_WARNING;
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_DNSSEC,
			      level,
			      "Permissions on the file %s "
			      "have changed from 0%o to 0600 as "
			      "a result of this operation.",
			      filename, (unsigned int)mode);
	}

	isc_buffer_init(&tmpb, tmpname, sizeof(tmpname));
	result = dst_key_buildfilename(key, DST_TYPE_TEMPLATE, directory,
				       &tmpb);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	fp = dst_key_open(tmpname, S_IRUSR | S_IWUSR);
	if (fp == NULL) {
		return DST_R_WRITEERROR;
	}

	dst_key_getprivateformat(key, &major, &minor);
	if (major == 0 && minor == 0) {
		major = DST_MAJOR_VERSION;
		minor = DST_MINOR_VERSION;
	}

	/* XXXDCL return value should be checked for full filesystem */
	fprintf(fp, "%s v%d.%d\n", PRIVATE_KEY_STR, major, minor);

	fprintf(fp, "%s %u ", ALGORITHM_STR, dst_key_alg(key));

	switch (dst_key_alg(key)) {
	case DST_ALG_RSASHA1:
		fprintf(fp, "(RSASHA1)\n");
		break;
	case DST_ALG_NSEC3RSASHA1:
		fprintf(fp, "(NSEC3RSASHA1)\n");
		break;
	case DST_ALG_RSASHA256:
		fprintf(fp, "(RSASHA256)\n");
		break;
	case DST_ALG_RSASHA512:
		fprintf(fp, "(RSASHA512)\n");
		break;
	case DST_ALG_ECDSA256:
		fprintf(fp, "(ECDSAP256SHA256)\n");
		break;
	case DST_ALG_ECDSA384:
		fprintf(fp, "(ECDSAP384SHA384)\n");
		break;
	case DST_ALG_ED25519:
		fprintf(fp, "(ED25519)\n");
		break;
	case DST_ALG_ED448:
		fprintf(fp, "(ED448)\n");
		break;
	case DST_ALG_HMACMD5:
		fprintf(fp, "(HMAC_MD5)\n");
		break;
	case DST_ALG_HMACSHA1:
		fprintf(fp, "(HMAC_SHA1)\n");
		break;
	case DST_ALG_HMACSHA224:
		fprintf(fp, "(HMAC_SHA224)\n");
		break;
	case DST_ALG_HMACSHA256:
		fprintf(fp, "(HMAC_SHA256)\n");
		break;
	case DST_ALG_HMACSHA384:
		fprintf(fp, "(HMAC_SHA384)\n");
		break;
	case DST_ALG_HMACSHA512:
		fprintf(fp, "(HMAC_SHA512)\n");
		break;
	case DST_ALG_RSASHA256PRIVATEOID:
		fprintf(fp, "(OID:RSASHA256)\n");
		break;
	case DST_ALG_RSASHA512PRIVATEOID:
		fprintf(fp, "(OID:RSASHA512)\n");
		break;
	default:
		fprintf(fp, "(?)\n");
		break;
	}

	for (i = 0; i < priv->nelements; i++) {
		const char *s;

		s = find_tag(priv->elements[i].tag);

		r.base = priv->elements[i].data;
		r.length = priv->elements[i].length;
		isc_buffer_init(&b, buffer, sizeof(buffer));
		result = isc_base64_totext(&r, sizeof(buffer), "", &b);
		if (result != ISC_R_SUCCESS) {
			return dst_key_cleanup(tmpname, fp);
		}
		isc_buffer_usedregion(&b, &r);

		fprintf(fp, "%s %.*s\n", s, (int)r.length, r.base);
	}

	if (key->external) {
		fprintf(fp, "External:\n");
	}

	/* Add the metadata tags */
	if (major > 1 || (major == 1 && minor >= 3)) {
		for (i = 0; i < NUMERIC_NTAGS; i++) {
			result = dst_key_getnum(key, i, &value);
			if (result != ISC_R_SUCCESS) {
				continue;
			}
			if (numerictags[i] != NULL) {
				fprintf(fp, "%s %u\n", numerictags[i], value);
			}
		}
		for (i = 0; i < TIMING_NTAGS; i++) {
			result = dst_key_gettime(key, i, &when);
			if (result != ISC_R_SUCCESS) {
				continue;
			}

			isc_buffer_init(&b, buffer, sizeof(buffer));
			result = dns_time32_totext(when, &b);
			if (result != ISC_R_SUCCESS) {
				return dst_key_cleanup(tmpname, fp);
			}

			isc_buffer_usedregion(&b, &r);

			if (timetags[i] != NULL) {
				fprintf(fp, "%s %.*s\n", timetags[i],
					(int)r.length, r.base);
			}
		}
	}

	result = dst_key_close(tmpname, fp, filename);
	return result;
}

/*! \file */
