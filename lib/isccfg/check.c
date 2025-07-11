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
#include <stdint.h>
#include <stdlib.h>

#include <openssl/opensslv.h>

#ifdef HAVE_DNSTAP
#include <fstrm.h>
#endif

#include <isc/base64.h>
#include <isc/buffer.h>
#include <isc/dir.h>
#include <isc/file.h>
#include <isc/hex.h>
#include <isc/log.h>
#include <isc/md.h>
#include <isc/mem.h>
#include <isc/netaddr.h>
#include <isc/parseint.h>
#include <isc/region.h>
#include <isc/result.h>
#include <isc/siphash.h>
#include <isc/sockaddr.h>
#include <isc/string.h>
#include <isc/symtab.h>
#include <isc/util.h>

#include <dns/acl.h>
#include <dns/dnstap.h>
#include <dns/fixedname.h>
#include <dns/journal.h>
#include <dns/kasp.h>
#include <dns/keystore.h>
#include <dns/keyvalues.h>
#include <dns/peer.h>
#include <dns/rdataclass.h>
#include <dns/rdatatype.h>
#include <dns/rpz.h>
#include <dns/rrl.h>
#include <dns/secalg.h>
#include <dns/ssu.h>

#include <dst/dst.h>

#include <isccfg/aclconf.h>
#include <isccfg/cfg.h>
#include <isccfg/check.h>
#include <isccfg/grammar.h>
#include <isccfg/kaspconf.h>
#include <isccfg/namedconf.h>

#include <ns/hooks.h>

#define NAMED_CONTROL_PORT 953

static in_port_t dnsport = 53;

static isc_result_t
fileexist(const cfg_obj_t *obj, isc_symtab_t *symtab, bool writeable);

static isc_result_t
keydirexist(const cfg_obj_t *zcgf, const char *optname, dns_name_t *zname,
	    const char *dirname, const char *kaspnamestr, isc_symtab_t *symtab,
	    isc_mem_t *mctx);

static const cfg_obj_t *
find_maplist(const cfg_obj_t *config, const char *listname, const char *name);

static void
freekey(char *key, unsigned int type, isc_symvalue_t value, void *userarg) {
	UNUSED(type);
	UNUSED(value);
	isc_mem_free(userarg, key);
}

static isc_result_t
check_orderent(const cfg_obj_t *ent) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	isc_textregion_t r;
	dns_fixedname_t fixed;
	const cfg_obj_t *obj;
	dns_rdataclass_t rdclass;
	dns_rdatatype_t rdtype;
	isc_buffer_t b;
	const char *str;

	dns_fixedname_init(&fixed);
	obj = cfg_tuple_get(ent, "class");
	if (cfg_obj_isstring(obj)) {
		r.base = UNCONST(cfg_obj_asstring(obj));
		r.length = strlen(r.base);
		tresult = dns_rdataclass_fromtext(&rdclass, &r);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "rrset-order: invalid class '%s'", r.base);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	obj = cfg_tuple_get(ent, "type");
	if (cfg_obj_isstring(obj)) {
		r.base = UNCONST(cfg_obj_asstring(obj));
		r.length = strlen(r.base);
		tresult = dns_rdatatype_fromtext(&rdtype, &r);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "rrset-order: invalid type '%s'", r.base);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	obj = cfg_tuple_get(ent, "name");
	if (cfg_obj_isstring(obj)) {
		str = cfg_obj_asstring(obj);
		isc_buffer_constinit(&b, str, strlen(str));
		isc_buffer_add(&b, strlen(str));
		tresult = dns_name_fromtext(dns_fixedname_name(&fixed), &b,
					    dns_rootname, 0);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "rrset-order: invalid name '%s'", str);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	obj = cfg_tuple_get(ent, "order");
	if (!cfg_obj_isstring(obj) ||
	    strcasecmp("order", cfg_obj_asstring(obj)) != 0)
	{
		cfg_obj_log(ent, ISC_LOG_ERROR,
			    "rrset-order: keyword 'order' missing");
		if (result == ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	}

	obj = cfg_tuple_get(ent, "ordering");
	if (!cfg_obj_isstring(obj)) {
		cfg_obj_log(ent, ISC_LOG_ERROR,
			    "rrset-order: missing ordering");
		if (result == ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	} else if (strcasecmp(cfg_obj_asstring(obj), "random") != 0 &&
		   strcasecmp(cfg_obj_asstring(obj), "cyclic") != 0 &&
		   strcasecmp(cfg_obj_asstring(obj), "none") != 0)
	{
		cfg_obj_log(obj, ISC_LOG_ERROR,
			    "rrset-order: invalid order '%s'",
			    cfg_obj_asstring(obj));
		if (result == ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	}
	return result;
}

static isc_result_t
check_order(const cfg_obj_t *options) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	const cfg_obj_t *obj = NULL;

	if (cfg_map_get(options, "rrset-order", &obj) != ISC_R_SUCCESS) {
		return result;
	}

	CFG_LIST_FOREACH (obj, element) {
		tresult = check_orderent(cfg_listelt_value(element));
		if (result == ISC_R_SUCCESS && tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
	}
	return result;
}

static isc_result_t
check_dual_stack(const cfg_obj_t *options) {
	const cfg_obj_t *alternates = NULL;
	const cfg_obj_t *value = NULL;
	const cfg_obj_t *obj = NULL;
	const char *str = NULL;
	dns_fixedname_t fixed;
	dns_name_t *name;
	isc_buffer_t buffer;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;

	(void)cfg_map_get(options, "dual-stack-servers", &alternates);

	if (alternates == NULL) {
		return ISC_R_SUCCESS;
	}

	obj = cfg_tuple_get(alternates, "port");
	if (cfg_obj_isuint32(obj)) {
		uint32_t val = cfg_obj_asuint32(obj);
		if (val > UINT16_MAX) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "port '%u' out of range", val);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}
	}
	obj = cfg_tuple_get(alternates, "addresses");
	CFG_LIST_FOREACH (obj, element) {
		value = cfg_listelt_value(element);
		if (cfg_obj_issockaddr(value)) {
			continue;
		}
		obj = cfg_tuple_get(value, "name");
		str = cfg_obj_asstring(obj);
		isc_buffer_constinit(&buffer, str, strlen(str));
		isc_buffer_add(&buffer, strlen(str));
		name = dns_fixedname_initname(&fixed);
		tresult = dns_name_fromtext(name, &buffer, dns_rootname, 0);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR, "bad name '%s'", str);
			if (result == ISC_R_SUCCESS) {
				result = tresult;
			}
		}
		obj = cfg_tuple_get(value, "port");
		if (cfg_obj_isuint32(obj)) {
			uint32_t val = cfg_obj_asuint32(obj);
			if (val > UINT16_MAX) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "port '%u' out of range", val);
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_RANGE;
				}
			}
		}
	}
	return result;
}

static isc_result_t
validate_tls(const cfg_obj_t *config, const cfg_obj_t *obj, const char *str) {
	dns_fixedname_t fname;
	dns_name_t *nm = dns_fixedname_initname(&fname);
	isc_result_t result = dns_name_fromstring(nm, str, dns_rootname, 0,
						  NULL);

	if (result != ISC_R_SUCCESS) {
		cfg_obj_log(obj, ISC_LOG_ERROR, "'%s' is not a valid name",
			    str);
		return result;
	}

	if (strcasecmp(str, "ephemeral") != 0) {
		const cfg_obj_t *tlsmap = find_maplist(config, "tls", str);

		if (tlsmap == NULL) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "tls '%s' is not defined", str);
			return ISC_R_FAILURE;
		}
	}

	return ISC_R_SUCCESS;
}

static isc_result_t
check_forward(const cfg_obj_t *config, const cfg_obj_t *options,
	      const cfg_obj_t *global) {
	const cfg_obj_t *forward = NULL;
	const cfg_obj_t *forwarders = NULL;
	const cfg_obj_t *faddresses = NULL;

	(void)cfg_map_get(options, "forward", &forward);
	(void)cfg_map_get(options, "forwarders", &forwarders);

	if (forwarders != NULL && global != NULL) {
		const char *file = cfg_obj_file(global);
		unsigned int line = cfg_obj_line(global);
		cfg_obj_log(forwarders, ISC_LOG_ERROR,
			    "forwarders declared in root zone and "
			    "in general configuration: %s:%u",
			    file, line);
		return ISC_R_FAILURE;
	}
	if (forward != NULL && forwarders == NULL) {
		cfg_obj_log(forward, ISC_LOG_ERROR,
			    "no matching 'forwarders' statement");
		return ISC_R_FAILURE;
	}
	if (forwarders != NULL) {
		isc_result_t result = ISC_R_SUCCESS;
		const cfg_obj_t *tlspobj = cfg_tuple_get(forwarders, "tls");

		if (tlspobj != NULL && cfg_obj_isstring(tlspobj)) {
			const char *tls = cfg_obj_asstring(tlspobj);
			if (tls != NULL) {
				result = validate_tls(config, tlspobj, tls);
				if (result != ISC_R_SUCCESS) {
					return result;
				}
			}
		}

		faddresses = cfg_tuple_get(forwarders, "addresses");
		CFG_LIST_FOREACH (faddresses, element) {
			const cfg_obj_t *forwarder = cfg_listelt_value(element);
			const char *tls = cfg_obj_getsockaddrtls(forwarder);
			if (tls != NULL) {
				result = validate_tls(config, faddresses, tls);
				if (result != ISC_R_SUCCESS) {
					return result;
				}
			}
		}
	}

	return ISC_R_SUCCESS;
}

static isc_result_t
disabled_algorithms(const cfg_obj_t *disabled) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	const char *str = NULL;
	isc_buffer_t b;
	dns_fixedname_t fixed;
	dns_name_t *name = NULL;
	const cfg_obj_t *obj = NULL;

	name = dns_fixedname_initname(&fixed);
	obj = cfg_tuple_get(disabled, "name");
	str = cfg_obj_asstring(obj);
	isc_buffer_constinit(&b, str, strlen(str));
	isc_buffer_add(&b, strlen(str));
	tresult = dns_name_fromtext(name, &b, dns_rootname, 0);
	if (tresult != ISC_R_SUCCESS) {
		cfg_obj_log(obj, ISC_LOG_ERROR, "bad domain name '%s'", str);
		result = tresult;
	}

	obj = cfg_tuple_get(disabled, "algorithms");

	CFG_LIST_FOREACH (obj, element) {
		isc_textregion_t r;
		dst_algorithm_t alg;

		r.base = UNCONST(cfg_obj_asstring(cfg_listelt_value(element)));
		r.length = strlen(r.base);

		tresult = dst_algorithm_fromtext(&alg, &r);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(cfg_listelt_value(element), ISC_LOG_ERROR,
				    "invalid algorithm '%s'", r.base);
			result = tresult;
		}
	}
	return result;
}

static isc_result_t
disabled_ds_digests(const cfg_obj_t *disabled) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	const char *str = NULL;
	isc_buffer_t b;
	dns_fixedname_t fixed;
	dns_name_t *name = NULL;
	const cfg_obj_t *obj = NULL;

	name = dns_fixedname_initname(&fixed);
	obj = cfg_tuple_get(disabled, "name");
	str = cfg_obj_asstring(obj);
	isc_buffer_constinit(&b, str, strlen(str));
	isc_buffer_add(&b, strlen(str));
	tresult = dns_name_fromtext(name, &b, dns_rootname, 0);
	if (tresult != ISC_R_SUCCESS) {
		cfg_obj_log(obj, ISC_LOG_ERROR, "bad domain name '%s'", str);
		result = tresult;
	}

	obj = cfg_tuple_get(disabled, "digests");

	CFG_LIST_FOREACH (obj, element) {
		isc_textregion_t r;
		dns_dsdigest_t digest;

		r.base = UNCONST(cfg_obj_asstring(cfg_listelt_value(element)));
		r.length = strlen(r.base);

		/* works with a numeric argument too */
		tresult = dns_dsdigest_fromtext(&digest, &r);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(cfg_listelt_value(element), ISC_LOG_ERROR,
				    "invalid digest type '%s'", r.base);
			result = tresult;
		}
	}
	return result;
}

static isc_result_t
exists(const cfg_obj_t *obj, const char *name, int value, isc_symtab_t *symtab,
       const char *fmt, isc_mem_t *mctx) {
	char *key;
	const char *file;
	unsigned int line;
	isc_result_t result;
	isc_symvalue_t symvalue;

	key = isc_mem_strdup(mctx, name);
	symvalue.as_cpointer = obj;
	result = isc_symtab_define(symtab, key, value, symvalue,
				   isc_symexists_reject);
	if (result == ISC_R_EXISTS) {
		RUNTIME_CHECK(isc_symtab_lookup(symtab, key, value,
						&symvalue) == ISC_R_SUCCESS);
		file = cfg_obj_file(symvalue.as_cpointer);
		line = cfg_obj_line(symvalue.as_cpointer);

		if (file == NULL) {
			file = "<unknown file>";
		}
		cfg_obj_log(obj, ISC_LOG_ERROR, fmt, key, file, line);
		isc_mem_free(mctx, key);
		result = ISC_R_EXISTS;
	}
	return result;
}

static isc_result_t
checkacl(const char *aclname, cfg_aclconfctx_t *actx, const cfg_obj_t *zconfig,
	 const cfg_obj_t *voptions, const cfg_obj_t *config, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS;
	const cfg_obj_t *aclobj = NULL;
	const cfg_obj_t *options;
	dns_acl_t *acl = NULL;

	if (zconfig != NULL) {
		options = cfg_tuple_get(zconfig, "options");
		cfg_map_get(options, aclname, &aclobj);
	}
	if (voptions != NULL && aclobj == NULL) {
		cfg_map_get(voptions, aclname, &aclobj);
	}
	if (config != NULL && aclobj == NULL) {
		options = NULL;
		cfg_map_get(config, "options", &options);
		if (options != NULL) {
			cfg_map_get(options, aclname, &aclobj);
		}
	}
	if (aclobj == NULL) {
		return ISC_R_SUCCESS;
	}
	result = cfg_acl_fromconfig(aclobj, config, actx, mctx, 0, &acl);
	if (acl != NULL) {
		dns_acl_detach(&acl);
	}

	if (strcasecmp(aclname, "allow-transfer") == 0 &&
	    cfg_obj_istuple(aclobj))
	{
		const cfg_obj_t *obj_port = cfg_tuple_get(
			cfg_tuple_get(aclobj, "port-transport"), "port");
		const cfg_obj_t *obj_proto = cfg_tuple_get(
			cfg_tuple_get(aclobj, "port-transport"), "transport");

		if (cfg_obj_isuint32(obj_port) &&
		    cfg_obj_asuint32(obj_port) >= UINT16_MAX)
		{
			cfg_obj_log(obj_port, ISC_LOG_ERROR,
				    "port value '%u' is out of range",

				    cfg_obj_asuint32(obj_port));
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}

		if (cfg_obj_isstring(obj_proto)) {
			const char *allowed[] = { "tcp", "tls" };
			const char *transport = cfg_obj_asstring(obj_proto);
			bool found = false;
			for (size_t i = 0; i < ARRAY_SIZE(allowed); i++) {
				if (strcasecmp(transport, allowed[i]) == 0) {
					found = true;
				}
			}

			if (!found) {
				cfg_obj_log(obj_proto, ISC_LOG_ERROR,
					    "'%s' is not a valid transport "
					    "protocol for "
					    "zone "
					    "transfers. Please specify either "
					    "'tcp' or 'tls'",
					    transport);
				result = ISC_R_FAILURE;
			}
		}
	}
	return result;
}

static isc_result_t
check_viewacls(cfg_aclconfctx_t *actx, const cfg_obj_t *voptions,
	       const cfg_obj_t *config, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	int i = 0;

	static const char *acls[] = {
		"allow-proxy",	      "allow-proxy-on",
		"allow-query",	      "allow-query-on",
		"allow-query-cache",  "allow-query-cache-on",
		"blackhole",	      "match-clients",
		"match-destinations", NULL
	};

	while (acls[i] != NULL) {
		tresult = checkacl(acls[i++], actx, NULL, voptions, config,
				   mctx);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
	}
	return result;
}

static void
dns64_error(const cfg_obj_t *obj, isc_netaddr_t *netaddr,
	    unsigned int prefixlen, const char *message) {
	char buf[ISC_NETADDR_FORMATSIZE + 1];
	isc_netaddr_format(netaddr, buf, sizeof(buf));
	cfg_obj_log(obj, ISC_LOG_ERROR, "dns64 prefix %s/%u %s", buf, prefixlen,
		    message);
}

static isc_result_t
check_dns64(cfg_aclconfctx_t *actx, const cfg_obj_t *voptions,
	    const cfg_obj_t *config, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS;
	const cfg_obj_t *dns64 = NULL;
	const cfg_obj_t *options;
	const cfg_obj_t *map = NULL, *obj = NULL;
	isc_netaddr_t na, sa;
	unsigned int prefixlen;
	int nbytes;
	int i;

	static const char *acls[] = { "clients", "exclude", "mapped", NULL };

	if (voptions != NULL) {
		cfg_map_get(voptions, "dns64", &dns64);
	}
	if (config != NULL && dns64 == NULL) {
		options = NULL;
		cfg_map_get(config, "options", &options);
		if (options != NULL) {
			cfg_map_get(options, "dns64", &dns64);
		}
	}
	if (dns64 == NULL) {
		return ISC_R_SUCCESS;
	}

	CFG_LIST_FOREACH (dns64, element) {
		map = cfg_listelt_value(element);
		obj = cfg_map_getname(map);

		cfg_obj_asnetprefix(obj, &na, &prefixlen);
		if (na.family != AF_INET6) {
			dns64_error(map, &na, prefixlen, "must be IPv6");
			result = ISC_R_FAILURE;
			continue;
		}

		if (na.type.in6.s6_addr[8] != 0) {
			dns64_error(map, &na, prefixlen,
				    "bits [64..71] must be zero");
			result = ISC_R_FAILURE;
			continue;
		}

		if (prefixlen != 32 && prefixlen != 40 && prefixlen != 48 &&
		    prefixlen != 56 && prefixlen != 64 && prefixlen != 96)
		{
			dns64_error(map, &na, prefixlen,
				    "length is not 32/40/48/56/64/96");
			result = ISC_R_FAILURE;
			continue;
		}

		for (i = 0; acls[i] != NULL; i++) {
			obj = NULL;
			(void)cfg_map_get(map, acls[i], &obj);
			if (obj != NULL) {
				dns_acl_t *acl = NULL;
				isc_result_t tresult;

				tresult = cfg_acl_fromconfig(obj, config, actx,
							     mctx, 0, &acl);
				if (acl != NULL) {
					dns_acl_detach(&acl);
				}
				if (tresult != ISC_R_SUCCESS) {
					result = tresult;
				}
			}
		}

		obj = NULL;
		(void)cfg_map_get(map, "suffix", &obj);
		if (obj != NULL) {
			static const unsigned char zeros[16];
			isc_netaddr_fromsockaddr(&sa, cfg_obj_assockaddr(obj));
			if (sa.family != AF_INET6) {
				cfg_obj_log(map, ISC_LOG_ERROR,
					    "dns64 requires a IPv6 suffix");
				result = ISC_R_FAILURE;
				continue;
			}
			nbytes = prefixlen / 8 + 4;
			if (prefixlen <= 64) {
				nbytes++;
			}
			if (memcmp(sa.type.in6.s6_addr, zeros, nbytes) != 0) {
				char netaddrbuf[ISC_NETADDR_FORMATSIZE];
				isc_netaddr_format(&sa, netaddrbuf,
						   sizeof(netaddrbuf));
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "bad suffix '%s' leading "
					    "%u octets not zeros",
					    netaddrbuf, nbytes);
				result = ISC_R_FAILURE;
			}
		}
	}

	return result;
}

#define CHECK_RRL(cond, pat, val1, val2)                                  \
	do {                                                              \
		if (!(cond)) {                                            \
			cfg_obj_log(obj, ISC_LOG_ERROR, pat, val1, val2); \
			if (result == ISC_R_SUCCESS)                      \
				result = ISC_R_RANGE;                     \
		}                                                         \
	} while (0)

#define CHECK_RRL_RATE(rate, def, max_rate, name)                          \
	do {                                                               \
		obj = NULL;                                                \
		mresult = cfg_map_get(map, name, &obj);                    \
		if (mresult == ISC_R_SUCCESS) {                            \
			rate = cfg_obj_asuint32(obj);                      \
			CHECK_RRL(rate <= max_rate, name " %d > %d", rate, \
				  max_rate);                               \
		}                                                          \
	} while (0)

static isc_result_t
check_ratelimit(cfg_aclconfctx_t *actx, const cfg_obj_t *voptions,
		const cfg_obj_t *config, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t mresult;
	const cfg_obj_t *map = NULL;
	const cfg_obj_t *options;
	const cfg_obj_t *obj;
	int min_entries, i;
	int all_per_second;
	int errors_per_second;
	int nodata_per_second;
	int nxdomains_per_second;
	int referrals_per_second;
	int responses_per_second;
	int slip;

	if (voptions != NULL) {
		cfg_map_get(voptions, "rate-limit", &map);
	}
	if (config != NULL && map == NULL) {
		options = NULL;
		cfg_map_get(config, "options", &options);
		if (options != NULL) {
			cfg_map_get(options, "rate-limit", &map);
		}
	}
	if (map == NULL) {
		return ISC_R_SUCCESS;
	}

	min_entries = 500;
	obj = NULL;
	mresult = cfg_map_get(map, "min-table-size", &obj);
	if (mresult == ISC_R_SUCCESS) {
		min_entries = cfg_obj_asuint32(obj);
		if (min_entries < 1) {
			min_entries = 1;
		}
	}

	obj = NULL;
	mresult = cfg_map_get(map, "max-table-size", &obj);
	if (mresult == ISC_R_SUCCESS) {
		i = cfg_obj_asuint32(obj);
		CHECK_RRL(i >= min_entries,
			  "max-table-size %d < min-table-size %d", i,
			  min_entries);
	}

	CHECK_RRL_RATE(responses_per_second, 0, DNS_RRL_MAX_RATE,
		       "responses-per-second");

	CHECK_RRL_RATE(referrals_per_second, responses_per_second,
		       DNS_RRL_MAX_RATE, "referrals-per-second");
	CHECK_RRL_RATE(nodata_per_second, responses_per_second,
		       DNS_RRL_MAX_RATE, "nodata-per-second");
	CHECK_RRL_RATE(nxdomains_per_second, responses_per_second,
		       DNS_RRL_MAX_RATE, "nxdomains-per-second");
	CHECK_RRL_RATE(errors_per_second, responses_per_second,
		       DNS_RRL_MAX_RATE, "errors-per-second");

	CHECK_RRL_RATE(all_per_second, 0, DNS_RRL_MAX_RATE, "all-per-second");

	CHECK_RRL_RATE(slip, 2, DNS_RRL_MAX_SLIP, "slip");

	obj = NULL;
	mresult = cfg_map_get(map, "window", &obj);
	if (mresult == ISC_R_SUCCESS) {
		i = cfg_obj_asuint32(obj);
		CHECK_RRL(i >= 1 && i <= DNS_RRL_MAX_WINDOW,
			  "window %d < 1 or > %d", i, DNS_RRL_MAX_WINDOW);
	}

	obj = NULL;
	mresult = cfg_map_get(map, "qps-scale", &obj);
	if (mresult == ISC_R_SUCCESS) {
		i = cfg_obj_asuint32(obj);
		CHECK_RRL(i >= 1, "invalid 'qps-scale %d'%s", i, "");
	}

	obj = NULL;
	mresult = cfg_map_get(map, "ipv4-prefix-length", &obj);
	if (mresult == ISC_R_SUCCESS) {
		i = cfg_obj_asuint32(obj);
		CHECK_RRL(i >= 8 && i <= 32,
			  "invalid 'ipv4-prefix-length %d'%s", i, "");
	}

	obj = NULL;
	mresult = cfg_map_get(map, "ipv6-prefix-length", &obj);
	if (mresult == ISC_R_SUCCESS) {
		i = cfg_obj_asuint32(obj);
		CHECK_RRL(i >= 16 && i <= DNS_RRL_MAX_PREFIX,
			  "ipv6-prefix-length %d < 16 or > %d", i,
			  DNS_RRL_MAX_PREFIX);
	}

	obj = NULL;
	(void)cfg_map_get(map, "exempt-clients", &obj);
	if (obj != NULL) {
		dns_acl_t *acl = NULL;
		isc_result_t tresult;

		tresult = cfg_acl_fromconfig(obj, config, actx, mctx, 0, &acl);
		if (acl != NULL) {
			dns_acl_detach(&acl);
		}
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	return result;
}

static isc_result_t
check_fetchlimit(const cfg_obj_t *voptions, const cfg_obj_t *config) {
	const cfg_obj_t *map = NULL;
	const cfg_obj_t *options = NULL;
	const cfg_obj_t *obj = NULL;
	double low, high, discount;

	if (voptions != NULL) {
		cfg_map_get(voptions, "fetch-quota-params", &map);
	}
	if (config != NULL && map == NULL) {
		options = NULL;
		cfg_map_get(config, "options", &options);
		if (options != NULL) {
			cfg_map_get(options, "fetch-quota-params", &map);
		}
	}
	if (map == NULL) {
		return ISC_R_SUCCESS;
	}

	obj = cfg_tuple_get(map, "low");
	low = (double)cfg_obj_asfixedpoint(obj) / 100.0;
	if (low < 0.0 || low > 1.0) {
		cfg_obj_log(obj, ISC_LOG_ERROR,
			    "fetch-quota-param low value (%0.1f) "
			    "out of range",
			    low);
		return ISC_R_RANGE;
	}

	obj = cfg_tuple_get(map, "high");
	high = (double)cfg_obj_asfixedpoint(obj) / 100.0;
	if (high < 0.0 || high > 1.0) {
		cfg_obj_log(obj, ISC_LOG_ERROR,
			    "fetch-quota-param high value (%0.1f) "
			    "out of range",
			    high);
		return ISC_R_RANGE;
	}

	obj = cfg_tuple_get(map, "discount");
	discount = (double)cfg_obj_asfixedpoint(obj) / 100.0;
	if (discount < 0.0 || discount > 1.0) {
		cfg_obj_log(obj, ISC_LOG_ERROR,
			    "fetch-quota-param discount value (%0.1f) "
			    "out of range",
			    discount);
		return ISC_R_RANGE;
	}

	return ISC_R_SUCCESS;
}

/*
 * Check allow-recursion and allow-recursion-on acls, and also log a
 * warning if they're inconsistent with the "recursion" option.
 */
static isc_result_t
check_recursionacls(cfg_aclconfctx_t *actx, const cfg_obj_t *voptions,
		    const char *viewname, const cfg_obj_t *config,
		    isc_mem_t *mctx) {
	const cfg_obj_t *options, *aclobj, *obj = NULL;
	dns_acl_t *acl = NULL;
	isc_result_t result = ISC_R_SUCCESS, tresult;
	bool recursion;
	const char *forview = " for view ";
	int i = 0;

	static const char *acls[] = { "allow-recursion", "allow-recursion-on",
				      NULL };

	if (voptions != NULL) {
		cfg_map_get(voptions, "recursion", &obj);
	}
	if (obj == NULL && config != NULL) {
		options = NULL;
		cfg_map_get(config, "options", &options);
		if (options != NULL) {
			cfg_map_get(options, "recursion", &obj);
		}
	}
	if (obj == NULL) {
		recursion = true;
	} else {
		recursion = cfg_obj_asboolean(obj);
	}

	if (viewname == NULL) {
		viewname = "";
		forview = "";
	}

	for (i = 0; acls[i] != NULL; i++) {
		aclobj = options = NULL;
		acl = NULL;

		if (voptions != NULL) {
			cfg_map_get(voptions, acls[i], &aclobj);
		}
		if (config != NULL && aclobj == NULL) {
			options = NULL;
			cfg_map_get(config, "options", &options);
			if (options != NULL) {
				cfg_map_get(options, acls[i], &aclobj);
			}
		}
		if (aclobj == NULL) {
			continue;
		}

		tresult = cfg_acl_fromconfig(aclobj, config, actx, mctx, 0,
					     &acl);

		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}

		if (acl == NULL) {
			continue;
		}

		if (!recursion && !dns_acl_isnone(acl)) {
			cfg_obj_log(aclobj, ISC_LOG_WARNING,
				    "both \"recursion no;\" and "
				    "\"%s\" active%s%s",
				    acls[i], forview, viewname);
		}

		if (acl != NULL) {
			dns_acl_detach(&acl);
		}
	}

	return result;
}

typedef struct {
	const char *name;
	unsigned int scale;
	unsigned int max;
} intervaltable;

#ifdef HAVE_DNSTAP
typedef struct {
	const char *name;
	unsigned int min;
	unsigned int max;
} fstrmtable;
#endif /* ifdef HAVE_DNSTAP */

typedef enum {
	optlevel_config,
	optlevel_options,
	optlevel_view,
	optlevel_zone
} optlevel_t;

static isc_result_t
check_name(const char *str) {
	dns_fixedname_t fixed;

	dns_fixedname_init(&fixed);
	return dns_name_fromstring(dns_fixedname_name(&fixed), str,
				   dns_rootname, 0, NULL);
}

static bool
kasp_name_allowed(const cfg_listelt_t *element) {
	const char *name = cfg_obj_asstring(
		cfg_tuple_get(cfg_listelt_value(element), "name"));

	if (strcmp("none", name) == 0) {
		return false;
	}
	if (strcmp("default", name) == 0) {
		return false;
	}
	if (strcmp("insecure", name) == 0) {
		return false;
	}
	return true;
}

static const cfg_obj_t *
find_maplist(const cfg_obj_t *config, const char *listname, const char *name) {
	isc_result_t result = ISC_R_SUCCESS;
	const cfg_obj_t *maplist = NULL;

	REQUIRE(config != NULL);
	REQUIRE(name != NULL);

	result = cfg_map_get(config, listname, &maplist);
	if (result != ISC_R_SUCCESS) {
		return NULL;
	}

	CFG_LIST_FOREACH (maplist, elt) {
		const cfg_obj_t *map = cfg_listelt_value(elt);
		if (strcasecmp(cfg_obj_asstring(cfg_map_getname(map)), name) ==
		    0)
		{
			return map;
		}
	}

	return NULL;
}

static isc_result_t
check_listener(const cfg_obj_t *listener, const cfg_obj_t *config,
	       cfg_aclconfctx_t *actx, isc_mem_t *mctx) {
	isc_result_t tresult, result = ISC_R_SUCCESS;
	const cfg_obj_t *ltup = NULL;
	const cfg_obj_t *tlsobj = NULL, *httpobj = NULL;
	const cfg_obj_t *portobj = NULL;
	const cfg_obj_t *http_server = NULL;
	const cfg_obj_t *proxyobj = NULL;
	bool do_tls = false, no_tls = false;
	dns_acl_t *acl = NULL;

	ltup = cfg_tuple_get(listener, "tuple");
	RUNTIME_CHECK(ltup != NULL);

	tlsobj = cfg_tuple_get(ltup, "tls");
	if (tlsobj != NULL && cfg_obj_isstring(tlsobj)) {
		const char *tlsname = cfg_obj_asstring(tlsobj);

		if (strcasecmp(tlsname, "none") == 0) {
			no_tls = true;
		} else if (strcasecmp(tlsname, "ephemeral") == 0) {
			do_tls = true;
		} else {
			const cfg_obj_t *tlsmap = NULL;

			do_tls = true;

			tlsmap = find_maplist(config, "tls", tlsname);
			if (tlsmap == NULL) {
				cfg_obj_log(tlsobj, ISC_LOG_ERROR,
					    "tls '%s' is not defined",
					    cfg_obj_asstring(tlsobj));
				result = ISC_R_FAILURE;
			}
		}
	}

	httpobj = cfg_tuple_get(ltup, "http");
	if (httpobj != NULL && cfg_obj_isstring(httpobj)) {
		const char *httpname = cfg_obj_asstring(httpobj);

		if (!do_tls && !no_tls) {
			cfg_obj_log(httpobj, ISC_LOG_ERROR,
				    "http must specify a 'tls' "
				    "statement, 'tls ephemeral', or "
				    "'tls none'");
			result = ISC_R_FAILURE;
		}

		http_server = find_maplist(config, "http", httpname);
		if (http_server == NULL && strcasecmp(httpname, "default") != 0)
		{
			cfg_obj_log(httpobj, ISC_LOG_ERROR,
				    "http '%s' is not defined",
				    cfg_obj_asstring(httpobj));
			result = ISC_R_FAILURE;
		}
	}

	portobj = cfg_tuple_get(ltup, "port");
	if (cfg_obj_isuint32(portobj) &&
	    cfg_obj_asuint32(portobj) >= UINT16_MAX)
	{
		cfg_obj_log(portobj, ISC_LOG_ERROR,
			    "port value '%u' is out of range",

			    cfg_obj_asuint32(portobj));
		if (result == ISC_R_SUCCESS) {
			result = ISC_R_RANGE;
		}
	}

	proxyobj = cfg_tuple_get(ltup, "proxy");
	if (proxyobj != NULL && cfg_obj_isstring(proxyobj)) {
		const char *proxyval = cfg_obj_asstring(proxyobj);
		if (proxyval == NULL ||
		    (strcasecmp(proxyval, "encrypted") != 0 &&
		     strcasecmp(proxyval, "plain") != 0))
		{
			cfg_obj_log(proxyobj, ISC_LOG_ERROR,
				    "'proxy' must have one of the following "
				    "values: 'plain', 'encrypted'");

			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}

		if (proxyval != NULL &&
		    strcasecmp(proxyval, "encrypted") == 0 && !do_tls)
		{
			cfg_obj_log(proxyobj, ISC_LOG_ERROR,
				    "'proxy encrypted' can be used only when "
				    "encryption is enabled by setting 'tls' to "
				    "a defined value or to 'ephemeral'");

			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	tresult = cfg_acl_fromconfig(cfg_tuple_get(listener, "acl"), config,
				     actx, mctx, 0, &acl);
	if (result == ISC_R_SUCCESS) {
		result = tresult;
	}

	if (acl != NULL) {
		dns_acl_detach(&acl);
	}

	return result;
}

static isc_result_t
check_listeners(const cfg_obj_t *list, const cfg_obj_t *config,
		cfg_aclconfctx_t *actx, isc_mem_t *mctx) {
	isc_result_t tresult, result = ISC_R_SUCCESS;

	CFG_LIST_FOREACH (list, elt) {
		const cfg_obj_t *obj = cfg_listelt_value(elt);
		tresult = check_listener(obj, config, actx, mctx);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	return result;
}

static isc_result_t
check_port(const cfg_obj_t *options, const char *type, in_port_t *portp) {
	const cfg_obj_t *portobj = NULL;
	isc_result_t result = ISC_R_SUCCESS;

	result = cfg_map_get(options, type, &portobj);
	if (result != ISC_R_SUCCESS) {
		return ISC_R_SUCCESS;
	}

	if (cfg_obj_asuint32(portobj) >= UINT16_MAX) {
		cfg_obj_log(portobj, ISC_LOG_ERROR, "port '%u' out of range",
			    cfg_obj_asuint32(portobj));
		return ISC_R_RANGE;
	}

	SET_IF_NOT_NULL(portp, (in_port_t)cfg_obj_asuint32(portobj));
	return ISC_R_SUCCESS;
}

static isc_result_t
check_options(const cfg_obj_t *options, const cfg_obj_t *config,
	      bool check_algorithms, isc_mem_t *mctx, optlevel_t optlevel) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	unsigned int i;
	const cfg_obj_t *obj = NULL;
	const char *str = NULL;
	isc_buffer_t b;
	uint32_t lifetime = 3600;
	dns_keystorelist_t kslist;
	const char *ccalg = "siphash24";
	cfg_aclconfctx_t *actx = NULL;
	static const char *sources[] = {
		"query-source",
		"query-source-v6",
	};

	/*
	 * { "name", scale, value }
	 * (scale * value) <= UINT32_MAX
	 */
	static intervaltable intervals[] = {
		{ "interface-interval", 60, 28 * 24 * 60 },    /* 28 days */
		{ "max-transfer-idle-in", 60, 28 * 24 * 60 },  /* 28 days */
		{ "max-transfer-idle-out", 60, 28 * 24 * 60 }, /* 28 days */
		{ "max-transfer-time-in", 60, 28 * 24 * 60 },  /* 28 days */
		{ "max-transfer-time-out", 60, 28 * 24 * 60 }, /* 28 days */

		/* minimum and maximum cache and negative cache TTLs */
		{ "min-cache-ttl", 1, MAX_MIN_CACHE_TTL },   /* 90 secs */
		{ "max-cache-ttl", 1, UINT32_MAX },	     /* no limit */
		{ "min-ncache-ttl", 1, MAX_MIN_NCACHE_TTL }, /* 90 secs */
		{ "max-ncache-ttl", 1, MAX_MAX_NCACHE_TTL }, /*  7 days */
	};

	static const char *server_contact[] = { "empty-server", "empty-contact",
						"dns64-server", "dns64-contact",
						NULL };

#ifdef HAVE_DNSTAP
	static fstrmtable fstrm[] = {
		{ "fstrm-set-buffer-hint", FSTRM_IOTHR_BUFFER_HINT_MIN,
		  FSTRM_IOTHR_BUFFER_HINT_MAX },
		{ "fstrm-set-flush-timeout", FSTRM_IOTHR_FLUSH_TIMEOUT_MIN,
		  FSTRM_IOTHR_FLUSH_TIMEOUT_MAX },
		{ "fstrm-set-input-queue-size",
		  FSTRM_IOTHR_INPUT_QUEUE_SIZE_MIN,
		  FSTRM_IOTHR_INPUT_QUEUE_SIZE_MAX },
		{ "fstrm-set-output-notify-threshold",
		  FSTRM_IOTHR_QUEUE_NOTIFY_THRESHOLD_MIN, 0 },
		{ "fstrm-set-output-queue-size",
		  FSTRM_IOTHR_OUTPUT_QUEUE_SIZE_MIN,
		  FSTRM_IOTHR_OUTPUT_QUEUE_SIZE_MAX },
		{ "fstrm-set-reopen-interval", FSTRM_IOTHR_REOPEN_INTERVAL_MIN,
		  FSTRM_IOTHR_REOPEN_INTERVAL_MAX }
	};
#endif /* ifdef HAVE_DNSTAP */

	if (optlevel == optlevel_options) {
		/*
		 * Check port values, and record "port" for later use.
		 */
		tresult = check_port(options, "port", &dnsport);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
		tresult = check_port(options, "tls-port", NULL);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
		tresult = check_port(options, "http-port", NULL);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
		tresult = check_port(options, "https-port", NULL);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	if (optlevel == optlevel_options || optlevel == optlevel_view) {
		/*
		 * Warn if query-source or query-source-v6 options specify
		 * a port, and fail if they specify the DNS port.
		 */
		unsigned int none_found = false;

		for (i = 0; i < ARRAY_SIZE(sources); i++) {
			obj = NULL;
			(void)cfg_map_get(options, sources[i], &obj);
			if (obj != NULL && cfg_obj_isvoid(obj)) {
				none_found++;

				if (none_found > 1) {
					cfg_obj_log(obj, ISC_LOG_ERROR,
						    "query-source and "
						    "query-source-v6 can't be "
						    "none at the same time.");
					result = ISC_R_FAILURE;
					break;
				}
			}
		}
	}

	/*
	 * Check that fields specified in units of time other than seconds
	 * have reasonable values.
	 */
	for (i = 0; i < sizeof(intervals) / sizeof(intervals[0]); i++) {
		uint32_t val;
		obj = NULL;
		(void)cfg_map_get(options, intervals[i].name, &obj);
		if (obj == NULL) {
			continue;
		}
		if (cfg_obj_isduration(obj)) {
			val = cfg_obj_asduration(obj);
		} else {
			val = cfg_obj_asuint32(obj);
		}
		if (val > intervals[i].max) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "%s '%u' is out of range (0..%u)",
				    intervals[i].name, val, intervals[i].max);
			result = ISC_R_RANGE;
		} else if (val > (UINT32_MAX / intervals[i].scale)) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "%s '%d' is out of range",
				    intervals[i].name, val);
			result = ISC_R_RANGE;
		}
	}

	/*
	 * Check key-store.
	 */
	ISC_LIST_INIT(kslist);

	obj = NULL;
	(void)cfg_map_get(options, "key-store", &obj);
	if (obj != NULL) {
		if (optlevel != optlevel_config) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "may only be configured at the top level");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		} else if (cfg_obj_islist(obj)) {
			CFG_LIST_FOREACH (obj, element) {
				isc_result_t ret;
				const char *val = NULL;
				cfg_obj_t *kconfig = cfg_listelt_value(element);
				const cfg_obj_t *kopt = NULL;
				const cfg_obj_t *kobj = NULL;
				if (!cfg_obj_istuple(kconfig)) {
					continue;
				}
				val = cfg_obj_asstring(
					cfg_tuple_get(kconfig, "name"));
				if (strcmp(DNS_KEYSTORE_KEYDIRECTORY, val) == 0)
				{
					cfg_obj_log(obj, ISC_LOG_ERROR,
						    "name '%s' not allowed",
						    DNS_KEYSTORE_KEYDIRECTORY);
					if (result == ISC_R_SUCCESS) {
						result = ISC_R_FAILURE;
						continue;
					}
				}

				kopt = cfg_tuple_get(kconfig, "options");
				if (cfg_map_get(kopt, "directory", &kobj) ==
				    ISC_R_SUCCESS)
				{
					val = cfg_obj_asstring(kobj);
					ret = isc_file_isdirectory(val);
					switch (ret) {
					case ISC_R_SUCCESS:
						break;
					case ISC_R_FILENOTFOUND:
						cfg_obj_log(
							obj, ISC_LOG_WARNING,
							"key-store directory: "
							"'%s' does not exist",
							val);
						break;
					case ISC_R_INVALIDFILE:
						cfg_obj_log(
							obj, ISC_LOG_WARNING,
							"key-store directory: "
							"'%s' is not a "
							"directory",
							val);
						break;
					default:
						cfg_obj_log(
							obj, ISC_LOG_WARNING,
							"key-store directory: "
							"'%s' %s",
							val,
							isc_result_totext(ret));
						if (result == ISC_R_SUCCESS) {
							result = ret;
						}
					}
				}

				ret = cfg_keystore_fromconfig(kconfig, mctx,
							      &kslist, NULL);
				if (ret != ISC_R_SUCCESS) {
					if (result == ISC_R_SUCCESS) {
						result = ret;
					}
				}
			}
		}
	}

	/*
	 * Add default key-store "key-directory".
	 */
	tresult = cfg_keystore_fromconfig(NULL, mctx, &kslist, NULL);
	if (tresult != ISC_R_SUCCESS) {
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	/*
	 * Check dnssec-policy.
	 */
	obj = NULL;
	(void)cfg_map_get(options, "dnssec-policy", &obj);
	if (obj != NULL) {
		bool bad_kasp = false;
		bool bad_name = false;

		if (optlevel != optlevel_config && !cfg_obj_isstring(obj)) {
			bad_kasp = true;
		} else if (optlevel == optlevel_config) {
			dns_kasplist_t list;
			dns_kasp_t *kasp = NULL;

			ISC_LIST_INIT(list);

			if (cfg_obj_islist(obj)) {
				CFG_LIST_FOREACH (obj, element) {
					isc_result_t ret;
					cfg_obj_t *kconfig =
						cfg_listelt_value(element);

					if (!cfg_obj_istuple(kconfig)) {
						bad_kasp = true;
						continue;
					}
					if (!kasp_name_allowed(element)) {
						bad_name = true;
						continue;
					}

					ret = cfg_kasp_fromconfig(
						kconfig, NULL, check_algorithms,
						mctx, &kslist, &list, &kasp);
					if (ret != ISC_R_SUCCESS) {
						if (result == ISC_R_SUCCESS) {
							result = ret;
						}
					}

					if (kasp != NULL) {
						dns_kasp_detach(&kasp);
					}
				}
			}

			ISC_LIST_FOREACH (list, k, link) {
				ISC_LIST_UNLINK(list, k, link);
				dns_kasp_detach(&k);
			}
		}

		if (bad_kasp) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "dnssec-policy may only be configured at "
				    "the top level, please use name reference "
				    "at the zone level");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		} else if (bad_name) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "dnssec-policy name may not be 'insecure', "
				    "'none', or 'default' (which are built-in "
				    "policies)");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	/*
	 * Cleanup key-store.
	 */
	ISC_LIST_FOREACH (kslist, ks, link) {
		ISC_LIST_UNLINK(kslist, ks, link);
		dns_keystore_detach(&ks);
	}

	/*
	 * Other checks.
	 */
	obj = NULL;
	cfg_map_get(options, "max-rsa-exponent-size", &obj);
	if (obj != NULL) {
		uint32_t val;

		val = cfg_obj_asuint32(obj);
		if (val != 0 && (val < 35 || val > 4096)) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "max-rsa-exponent-size '%u' is out of "
				    "range (35..4096)",
				    val);
			result = ISC_R_RANGE;
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "preferred-glue", &obj);
	if (obj != NULL) {
		str = cfg_obj_asstring(obj);
		if (strcasecmp(str, "a") != 0 && strcasecmp(str, "aaaa") != 0 &&
		    strcasecmp(str, "none") != 0)
		{
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "preferred-glue unexpected value '%s'",
				    str);
		}
	}

	/*
	 * Set supported DNSSEC algorithms.
	 */
	obj = NULL;
	(void)cfg_map_get(options, "disable-algorithms", &obj);
	if (obj != NULL) {
		CFG_LIST_FOREACH (obj, element) {
			obj = cfg_listelt_value(element);
			tresult = disabled_algorithms(obj);
			if (tresult != ISC_R_SUCCESS) {
				result = tresult;
			}
		}
	}

	/*
	 * Set supported DS digest types.
	 */
	obj = NULL;
	(void)cfg_map_get(options, "disable-ds-digests", &obj);
	if (obj != NULL) {
		CFG_LIST_FOREACH (obj, element) {
			obj = cfg_listelt_value(element);
			tresult = disabled_ds_digests(obj);
			if (tresult != ISC_R_SUCCESS) {
				result = tresult;
			}
		}
	}

	/*
	 * Check send-report-channel. (Skip for zone level because we
	 * have an additional check in isccfg_check_zoneconf() for that.)
	 */
	if (optlevel != optlevel_zone) {
		obj = NULL;
		(void)cfg_map_get(options, "send-report-channel", &obj);
		if (obj != NULL) {
			str = cfg_obj_asstring(obj);
			tresult = check_name(str);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "'%s' is not a valid name", str);
				if (result == ISC_R_SUCCESS) {
					result = tresult;
				}
			}
		}
	}

	/*
	 * Check server/contacts for syntactic validity.
	 */
	for (i = 0; server_contact[i] != NULL; i++) {
		obj = NULL;
		(void)cfg_map_get(options, server_contact[i], &obj);
		if (obj != NULL) {
			str = cfg_obj_asstring(obj);
			if (check_name(str) != ISC_R_SUCCESS) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "%s: invalid name '%s'",
					    server_contact[i], str);
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_FAILURE;
				}
			}
		}
	}

	/*
	 * Check empty zone configuration.
	 */
	obj = NULL;
	(void)cfg_map_get(options, "disable-empty-zone", &obj);
	CFG_LIST_FOREACH (obj, element) {
		obj = cfg_listelt_value(element);
		str = cfg_obj_asstring(obj);
		if (check_name(str) != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "disable-empty-zone: invalid name '%s'",
				    str);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	/*
	 * Check that server-id is not too long.
	 * 1024 bytes should be big enough.
	 */
	obj = NULL;
	(void)cfg_map_get(options, "server-id", &obj);
	if (obj != NULL && cfg_obj_isstring(obj) &&
	    strlen(cfg_obj_asstring(obj)) > 1024U)
	{
		cfg_obj_log(obj, ISC_LOG_ERROR,
			    "'server-id' too big (>1024 bytes)");
		if (result == ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "nta-lifetime", &obj);
	if (obj != NULL) {
		lifetime = cfg_obj_asduration(obj);
		if (lifetime > 604800) { /* 7 days */
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'nta-lifetime' cannot exceed one week");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		} else if (lifetime == 0) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'nta-lifetime' may not be zero");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "nta-recheck", &obj);
	if (obj != NULL) {
		uint32_t recheck = cfg_obj_asduration(obj);
		if (recheck > 604800) { /* 7 days */
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'nta-recheck' cannot exceed one week");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}

		if (recheck > lifetime) {
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "'nta-recheck' (%d seconds) is "
				    "greater than 'nta-lifetime' "
				    "(%d seconds)",
				    recheck, lifetime);
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "cookie-algorithm", &obj);
	if (obj != NULL) {
		ccalg = cfg_obj_asstring(obj);
		if (strcasecmp(ccalg, "aes") == 0) {
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "cookie-algorithm 'aes' is obsolete and "
				    "should be removed");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "cookie-secret", &obj);
	if (obj != NULL) {
		unsigned char secret[32];

		CFG_LIST_FOREACH (obj, element) {
			unsigned int usedlength;

			obj = cfg_listelt_value(element);
			str = cfg_obj_asstring(obj);

			memset(secret, 0, sizeof(secret));
			isc_buffer_init(&b, secret, sizeof(secret));
			tresult = isc_hex_decodestring(str, &b);
			if (tresult == ISC_R_NOSPACE) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "cookie-secret: too long");
			} else if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "cookie-secret: invalid hex "
					    "string");
			}
			if (tresult != ISC_R_SUCCESS) {
				if (result == ISC_R_SUCCESS) {
					result = tresult;
				}
				continue;
			}

			usedlength = isc_buffer_usedlength(&b);
			if (strcasecmp(ccalg, "siphash24") == 0 &&
			    usedlength != ISC_SIPHASH24_KEY_LENGTH)
			{
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "SipHash-2-4 cookie-secret must be "
					    "128 bits");
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_RANGE;
				}
			}
		}
	}

#ifdef HAVE_DNSTAP
	for (i = 0; i < sizeof(fstrm) / sizeof(fstrm[0]); i++) {
		uint32_t value;

		obj = NULL;
		(void)cfg_map_get(options, fstrm[i].name, &obj);
		if (obj == NULL) {
			continue;
		}

		if (cfg_obj_isduration(obj)) {
			value = cfg_obj_asduration(obj);
		} else {
			value = cfg_obj_asuint32(obj);
		}
		if (value < fstrm[i].min ||
		    (fstrm[i].max != 0U && value > fstrm[i].max))
		{
			if (fstrm[i].max != 0U) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "%s '%u' out of range (%u..%u)",
					    fstrm[i].name, value, fstrm[i].min,
					    fstrm[i].max);
			} else {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "%s out of range (%u < %u)",
					    fstrm[i].name, value, fstrm[i].min);
			}
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}

		if (strcmp(fstrm[i].name, "fstrm-set-input-queue-size") == 0) {
			int bits = 0;
			do {
				bits += value & 0x1;
				value >>= 1;
			} while (value != 0U);
			if (bits != 1) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "%s '%u' not a power-of-2",
					    fstrm[i].name,
					    cfg_obj_asuint32(obj));
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_RANGE;
				}
			}
		}
	}

	/* Check that dnstap-ouput values are consistent */
	obj = NULL;
	(void)cfg_map_get(options, "dnstap-output", &obj);
	if (obj != NULL) {
		const cfg_obj_t *obj2;
		dns_dtmode_t dmode;

		obj2 = cfg_tuple_get(obj, "mode");
		if (obj2 == NULL) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "dnstap-output mode not found");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		} else {
			if (strcasecmp(cfg_obj_asstring(obj2), "file") == 0) {
				dmode = dns_dtmode_file;
			} else {
				dmode = dns_dtmode_unix;
			}

			obj2 = cfg_tuple_get(obj, "size");
			if (obj2 != NULL && !cfg_obj_isvoid(obj2) &&
			    dmode == dns_dtmode_unix)
			{
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "dnstap-output size "
					    "cannot be set with mode unix");
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_FAILURE;
				}
			}

			obj2 = cfg_tuple_get(obj, "versions");
			if (obj2 != NULL && !cfg_obj_isvoid(obj2) &&
			    dmode == dns_dtmode_unix)
			{
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "dnstap-output versions "
					    "cannot be set with mode unix");
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_FAILURE;
				}
			}

			obj2 = cfg_tuple_get(obj, "suffix");
			if (obj2 != NULL && !cfg_obj_isvoid(obj2) &&
			    dmode == dns_dtmode_unix)
			{
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "dnstap-output suffix "
					    "cannot be set with mode unix");
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_FAILURE;
				}
			}
		}
	}
#endif /* ifdef HAVE_DNSTAP */

	obj = NULL;
	(void)cfg_map_get(options, "lmdb-mapsize", &obj);
	if (obj != NULL) {
		uint64_t mapsize = cfg_obj_asuint64(obj);

		if (mapsize < (1ULL << 20)) { /* 1 megabyte */
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'lmdb-mapsize "
				    "%" PRId64 "' "
				    "is too small",
				    mapsize);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		} else if (mapsize > (1ULL << 40)) { /* 1 terabyte */
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'lmdb-mapsize "
				    "%" PRId64 "' "
				    "is too large",
				    mapsize);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "max-ixfr-ratio", &obj);
	if (obj != NULL && cfg_obj_ispercentage(obj)) {
		uint32_t percent = cfg_obj_aspercentage(obj);
		if (percent == 0) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'ixfr-max-ratio' must be a nonzero "
				    "percentage or 'unlimited')");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		} else if (percent > 100) {
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "'ixfr-max-ratio %d%%' exceeds 100%%",
				    percent);
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "check-names", &obj);
	if (obj != NULL && !cfg_obj_islist(obj)) {
		obj = NULL;
	}
	if (obj != NULL) {
		/* Note: SEC is defined in <sys/time.h> on some platforms. */
		enum { MAS = 1, PRI = 2, SLA = 4, SCN = 8 } values = 0;
		CFG_LIST_FOREACH (obj, el) {
			const cfg_obj_t *tuple = cfg_listelt_value(el);
			const cfg_obj_t *type = cfg_tuple_get(tuple, "type");
			const char *keyword = cfg_obj_asstring(type);
			if (strcasecmp(keyword, "primary") == 0) {
				if ((values & PRI) == PRI) {
					cfg_obj_log(obj, ISC_LOG_ERROR,
						    "'check-names primary' "
						    "duplicated");
					if (result == ISC_R_SUCCESS) {
						result = ISC_R_FAILURE;
					}
				}
				values |= PRI;
			} else if (strcasecmp(keyword, "master") == 0) {
				if ((values & MAS) == MAS) {
					cfg_obj_log(obj, ISC_LOG_ERROR,
						    "'check-names master' "
						    "duplicated");
					if (result == ISC_R_SUCCESS) {
						result = ISC_R_FAILURE;
					}
				}
				values |= MAS;
			} else if (strcasecmp(keyword, "secondary") == 0) {
				if ((values & SCN) == SCN) {
					cfg_obj_log(obj, ISC_LOG_ERROR,
						    "'check-names secondary' "
						    "duplicated");
					if (result == ISC_R_SUCCESS) {
						result = ISC_R_FAILURE;
					}
				}
				values |= SCN;
			} else if (strcasecmp(keyword, "slave") == 0) {
				if ((values & SLA) == SLA) {
					cfg_obj_log(obj, ISC_LOG_ERROR,
						    "'check-names slave' "
						    "duplicated");
					if (result == ISC_R_SUCCESS) {
						result = ISC_R_FAILURE;
					}
				}
				values |= SLA;
			}
		}

		if ((values & (PRI | MAS)) == (PRI | MAS)) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'check-names' cannot take both "
				    "'primary' and 'master'");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}

		if ((values & (SCN | SLA)) == (SCN | SLA)) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'check-names' cannot take both "
				    "'secondary' and 'slave'");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "stale-refresh-time", &obj);
	if (obj != NULL) {
		uint32_t refresh_time = cfg_obj_asduration(obj);
		if (refresh_time > 0 && refresh_time < 30) {
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "'stale-refresh-time' should either be 0 "
				    "or otherwise 30 seconds or higher");
		}
	}

	cfg_aclconfctx_create(mctx, &actx);

	obj = NULL;
	(void)cfg_map_get(options, "sig0checks-quota-exempt", &obj);
	if (obj != NULL) {
		dns_acl_t *acl = NULL;

		tresult = cfg_acl_fromconfig(obj, config, actx, mctx, 0, &acl);
		if (acl != NULL) {
			dns_acl_detach(&acl);
		}
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "listen-on", &obj);
	if (obj != NULL) {
		INSIST(config != NULL);
		tresult = check_listeners(obj, config, actx, mctx);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "listen-on-v6", &obj);
	if (obj != NULL) {
		INSIST(config != NULL);
		tresult = check_listeners(obj, config, actx, mctx);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	obj = NULL;
	(void)cfg_map_get(options, "max-query-restarts", &obj);
	if (obj != NULL) {
		uint32_t restarts = cfg_obj_asuint32(obj);
		if (restarts == 0 || restarts > 255) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'max-query-restarts' is out of "
				    "range 1..255)");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}
	}

	if (actx != NULL) {
		cfg_aclconfctx_detach(&actx);
	}

	return result;
}

/*
 * Check "remote-servers" style list.
 */
static isc_result_t
check_remoteserverlist(const cfg_obj_t *cctx, const char *list,
		       isc_symtab_t *symtab, isc_mem_t *mctx) {
	isc_symvalue_t symvalue;
	isc_result_t result = ISC_R_SUCCESS, tresult;
	const cfg_obj_t *obj = NULL;

	result = cfg_map_get(cctx, list, &obj);
	if (result != ISC_R_SUCCESS) {
		return ISC_R_SUCCESS;
	}

	CFG_LIST_FOREACH (obj, elt) {
		char *tmp = NULL;
		const char *name = NULL;

		obj = cfg_listelt_value(elt);
		name = cfg_obj_asstring(cfg_tuple_get(obj, "name"));

		tmp = isc_mem_strdup(mctx, name);
		symvalue.as_cpointer = obj;
		tresult = isc_symtab_define(symtab, tmp, 1, symvalue,
					    isc_symexists_reject);
		if (tresult == ISC_R_EXISTS) {
			const char *file = NULL;
			unsigned int line;

			RUNTIME_CHECK(
				isc_symtab_lookup(symtab, tmp, 1, &symvalue) ==
				ISC_R_SUCCESS);
			file = cfg_obj_file(symvalue.as_cpointer);
			line = cfg_obj_line(symvalue.as_cpointer);

			if (file == NULL) {
				file = "<unknown file>";
			}
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "%s list '%s' is duplicated: "
				    "also defined at %s:%u",
				    list, name, file, line);
			isc_mem_free(mctx, tmp);
			result = tresult;
			break;
		}
	}
	return result;
}

/*
 * Check remote-server lists for duplicates.
 */
static isc_result_t
check_remoteserverlists(const cfg_obj_t *cctx, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	isc_symtab_t *symtab = NULL;

	isc_symtab_create(mctx, freekey, mctx, false, &symtab);

	tresult = check_remoteserverlist(cctx, "remote-servers", symtab, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}
	/* parental-agents, primaries, masters are treated as synonyms */
	tresult = check_remoteserverlist(cctx, "parental-agents", symtab, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}
	tresult = check_remoteserverlist(cctx, "primaries", symtab, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}
	tresult = check_remoteserverlist(cctx, "masters", symtab, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}
	isc_symtab_destroy(&symtab);
	return result;
}

#if HAVE_LIBNGHTTP2
static isc_result_t
check_httpserver(const cfg_obj_t *http, isc_symtab_t *symtab) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	const char *name = cfg_obj_asstring(cfg_map_getname(http));
	const cfg_obj_t *eps = NULL;
	isc_symvalue_t symvalue;

	if (strcasecmp(name, "default") == 0) {
		cfg_obj_log(http, ISC_LOG_ERROR,
			    "'http' name cannot be '%s' (which is a "
			    "built-in configuration)",
			    name);
		result = ISC_R_FAILURE;
	} else {
		/* Check for duplicates */
		symvalue.as_cpointer = http;
		result = isc_symtab_define(symtab, name, 1, symvalue,
					   isc_symexists_reject);
		if (result == ISC_R_EXISTS) {
			const char *file = NULL;
			unsigned int line;

			tresult = isc_symtab_lookup(symtab, name, 1, &symvalue);
			RUNTIME_CHECK(tresult == ISC_R_SUCCESS);

			line = cfg_obj_line(symvalue.as_cpointer);
			file = cfg_obj_file(symvalue.as_cpointer);
			if (file == NULL) {
				file = "<unknown file>";
			}

			cfg_obj_log(http, ISC_LOG_ERROR,
				    "http '%s' is duplicated: "
				    "also defined at %s:%u",
				    name, file, line);
		}
	}

	/* Check endpoints are valid */
	tresult = cfg_map_get(http, "endpoints", &eps);
	if (tresult == ISC_R_SUCCESS) {
		CFG_LIST_FOREACH (eps, elt) {
			const cfg_obj_t *ep = cfg_listelt_value(elt);
			const char *path = cfg_obj_asstring(ep);
			if (!isc_nm_http_path_isvalid(path)) {
				cfg_obj_log(eps, ISC_LOG_ERROR,
					    "endpoint '%s' is not a "
					    "valid absolute HTTP path",
					    path);
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_FAILURE;
				}
			}
		}
	}

	return result;
}

static isc_result_t
check_httpservers(const cfg_obj_t *config, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	const cfg_obj_t *obj = NULL;
	isc_symtab_t *symtab = NULL;

	isc_symtab_create(mctx, NULL, NULL, false, &symtab);

	result = cfg_map_get(config, "http", &obj);
	if (result != ISC_R_SUCCESS) {
		result = ISC_R_SUCCESS;
		goto done;
	}

	CFG_LIST_FOREACH (obj, elt) {
		obj = cfg_listelt_value(elt);
		tresult = check_httpserver(obj, symtab);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

done:
	isc_symtab_destroy(&symtab);
	return result;
}
#endif /* HAVE_LIBNGHTTP2 */

static isc_result_t
check_tls_defintion(const cfg_obj_t *tlsobj, const char *name,
		    isc_symtab_t *symtab) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	const cfg_obj_t *tls_proto_list = NULL, *tls_key = NULL,
			*tls_cert = NULL, *tls_ciphers = NULL,
			*tls_cipher_suites = NULL;
	uint32_t tls_protos = 0;
	isc_symvalue_t symvalue;

	if (strcasecmp(name, "ephemeral") == 0 || strcasecmp(name, "none") == 0)
	{
		cfg_obj_log(tlsobj, ISC_LOG_ERROR,
			    "tls clause name '%s' is reserved for internal use",
			    name);
		result = ISC_R_FAILURE;
	} else {
		/* Check for duplicates */
		symvalue.as_cpointer = tlsobj;
		result = isc_symtab_define(symtab, name, 1, symvalue,
					   isc_symexists_reject);
		if (result == ISC_R_EXISTS) {
			const char *file = NULL;
			unsigned int line;

			tresult = isc_symtab_lookup(symtab, name, 1, &symvalue);
			RUNTIME_CHECK(tresult == ISC_R_SUCCESS);

			line = cfg_obj_line(symvalue.as_cpointer);
			file = cfg_obj_file(symvalue.as_cpointer);
			if (file == NULL) {
				file = "<unknown file>";
			}

			cfg_obj_log(tlsobj, ISC_LOG_ERROR,
				    "tls clause '%s' is duplicated: "
				    "also defined at %s:%u",
				    name, file, line);
		}
	}

	(void)cfg_map_get(tlsobj, "key-file", &tls_key);
	(void)cfg_map_get(tlsobj, "cert-file", &tls_cert);
	if ((tls_key == NULL && tls_cert != NULL) ||
	    (tls_cert == NULL && tls_key != NULL))
	{
		cfg_obj_log(tlsobj, ISC_LOG_ERROR,
			    "tls '%s': 'cert-file' and 'key-file' must "
			    "both be specified, or both omitted",
			    name);
		result = ISC_R_FAILURE;
	}

	/* Check protocols are valid */
	tresult = cfg_map_get(tlsobj, "protocols", &tls_proto_list);
	if (tresult == ISC_R_SUCCESS) {
		INSIST(tls_proto_list != NULL);
		CFG_LIST_FOREACH (tls_proto_list, proto) {
			const cfg_obj_t *tls_proto_obj =
				cfg_listelt_value(proto);
			const char *tls_sver = cfg_obj_asstring(tls_proto_obj);
			const isc_tls_protocol_version_t ver =
				isc_tls_protocol_name_to_version(tls_sver);

			if (ver == ISC_TLS_PROTO_VER_UNDEFINED) {
				cfg_obj_log(tls_proto_obj, ISC_LOG_ERROR,
					    "'%s' is not a valid "
					    "TLS protocol version",
					    tls_sver);
				result = ISC_R_FAILURE;
				continue;
			} else if (!isc_tls_protocol_supported(ver)) {
				cfg_obj_log(tls_proto_obj, ISC_LOG_ERROR,
					    "'%s' is not "
					    "supported by the "
					    "cryptographic library version in "
					    "use (%s)",
					    tls_sver, OPENSSL_VERSION_TEXT);
				result = ISC_R_FAILURE;
			}

			if ((tls_protos & ver) != 0) {
				cfg_obj_log(tls_proto_obj, ISC_LOG_WARNING,
					    "'%s' is specified more than once "
					    "in '%s'",
					    tls_sver, name);
				result = ISC_R_FAILURE;
			}

			tls_protos |= ver;
		}

		if (tls_protos == 0) {
			cfg_obj_log(tlsobj, ISC_LOG_ERROR,
				    "tls '%s' does not contain any valid "
				    "TLS protocol versions definitions",
				    name);
			result = ISC_R_FAILURE;
		}
	}

	/* Check cipher list string is valid */
	tresult = cfg_map_get(tlsobj, "ciphers", &tls_ciphers);
	if (tresult == ISC_R_SUCCESS) {
		const char *ciphers = cfg_obj_asstring(tls_ciphers);
		if (!isc_tls_cipherlist_valid(ciphers)) {
			cfg_obj_log(tls_ciphers, ISC_LOG_ERROR,
				    "'ciphers' in the 'tls' clause '%s' is "
				    "not a "
				    "valid cipher list string",
				    name);
			result = ISC_R_FAILURE;
		}
	}

	/* Check if the cipher suites string is valid */
	tresult = cfg_map_get(tlsobj, "cipher-suites", &tls_cipher_suites);
	if (tresult == ISC_R_SUCCESS) {
		const char *cipher_suites = cfg_obj_asstring(tls_cipher_suites);
		if (!isc_tls_cipher_suites_valid(cipher_suites)) {
			cfg_obj_log(
				tls_cipher_suites, ISC_LOG_ERROR,
				"'cipher-suites' in the 'tls' clause '%s' is "
				"not a valid cipher suites string",
				name);
			result = ISC_R_FAILURE;
		}
	}

	return result;
}

static isc_result_t
check_tls_definitions(const cfg_obj_t *config, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	const cfg_obj_t *obj = NULL;
	isc_symtab_t *symtab = NULL;

	result = cfg_map_get(config, "tls", &obj);
	if (result != ISC_R_SUCCESS) {
		result = ISC_R_SUCCESS;
		return result;
	}

	isc_symtab_create(mctx, NULL, NULL, false, &symtab);

	CFG_LIST_FOREACH (obj, elt) {
		const char *name;
		obj = cfg_listelt_value(elt);
		name = cfg_obj_asstring(cfg_map_getname(obj));
		tresult = check_tls_defintion(obj, name, symtab);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	isc_symtab_destroy(&symtab);

	return result;
}

static isc_result_t
get_remotes(const cfg_obj_t *cctx, const char *list, const char *name,
	    const cfg_obj_t **ret) {
	isc_result_t result = ISC_R_SUCCESS;
	const cfg_obj_t *obj = NULL;

	result = cfg_map_get(cctx, list, &obj);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	CFG_LIST_FOREACH (obj, elt) {
		const char *listname = NULL;

		obj = cfg_listelt_value(elt);
		listname = cfg_obj_asstring(cfg_tuple_get(obj, "name"));

		if (strcasecmp(listname, name) == 0) {
			*ret = obj;
			return ISC_R_SUCCESS;
		}
	}

	return ISC_R_NOTFOUND;
}

static isc_result_t
get_remoteservers_def(const char *name, const cfg_obj_t *cctx,
		      const cfg_obj_t **ret) {
	isc_result_t result;

	result = get_remotes(cctx, "remote-servers", name, ret);
	if (result == ISC_R_SUCCESS) {
		return result;
	}
	result = get_remotes(cctx, "primaries", name, ret);
	if (result == ISC_R_SUCCESS) {
		return result;
	}
	result = get_remotes(cctx, "parental-agents", name, ret);
	if (result == ISC_R_SUCCESS) {
		return result;
	}
	return get_remotes(cctx, "masters", name, ret);
}

static isc_result_t
validate_remotes(const cfg_obj_t *obj, const cfg_obj_t *config,
		 uint32_t *countp, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	uint32_t count = 0;
	isc_symtab_t *symtab = NULL;
	isc_symvalue_t symvalue;
	const cfg_listelt_t *element = NULL;
	cfg_listelt_t **stack = NULL;
	uint32_t stackcount = 0, pushed = 0;
	const cfg_obj_t *listobj;

	REQUIRE(countp != NULL);
	isc_symtab_create(mctx, NULL, NULL, false, &symtab);

newlist:
	listobj = cfg_tuple_get(obj, "addresses");
	element = cfg_list_first(listobj);
resume:
	for (; element != NULL; element = cfg_list_next(element)) {
		const char *listname;
		const cfg_obj_t *addr;
		const cfg_obj_t *key;
		const cfg_obj_t *tls;

		addr = cfg_tuple_get(cfg_listelt_value(element),
				     "remoteselement");
		key = cfg_tuple_get(cfg_listelt_value(element), "key");
		tls = cfg_tuple_get(cfg_listelt_value(element), "tls");

		if (cfg_obj_issockaddr(addr)) {
			count++;
			if (cfg_obj_isstring(key)) {
				const char *str = cfg_obj_asstring(key);
				dns_fixedname_t fname;
				dns_name_t *nm = dns_fixedname_initname(&fname);
				tresult = dns_name_fromstring(
					nm, str, dns_rootname, 0, NULL);
				if (tresult != ISC_R_SUCCESS) {
					cfg_obj_log(key, ISC_LOG_ERROR,
						    "'%s' is not a valid name",
						    str);
					if (result == ISC_R_SUCCESS) {
						result = tresult;
					}
				}
			}
			if (cfg_obj_isstring(tls)) {
				const char *str = cfg_obj_asstring(tls);
				dns_fixedname_t fname;
				dns_name_t *nm = dns_fixedname_initname(&fname);
				tresult = dns_name_fromstring(
					nm, str, dns_rootname, 0, NULL);
				if (tresult != ISC_R_SUCCESS) {
					cfg_obj_log(tls, ISC_LOG_ERROR,
						    "'%s' is not a valid name",
						    str);
					if (result == ISC_R_SUCCESS) {
						result = tresult;
					}
				}

				if (strcasecmp(str, "ephemeral") != 0) {
					const cfg_obj_t *tlsmap = NULL;

					tlsmap = find_maplist(config, "tls",
							      str);
					if (tlsmap == NULL) {
						cfg_obj_log(
							tls, ISC_LOG_ERROR,
							"tls '%s' is not "
							"defined",
							cfg_obj_asstring(tls));
						result = ISC_R_FAILURE;
					}
				}
			}
			continue;
		}
		if (!cfg_obj_isvoid(key)) {
			cfg_obj_log(key, ISC_LOG_ERROR, "unexpected token '%s'",
				    cfg_obj_asstring(key));
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
		if (!cfg_obj_isvoid(tls)) {
			cfg_obj_log(key, ISC_LOG_ERROR, "unexpected token '%s'",
				    cfg_obj_asstring(tls));
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
		listname = cfg_obj_asstring(addr);
		symvalue.as_cpointer = addr;
		tresult = isc_symtab_define(symtab, listname, 1, symvalue,
					    isc_symexists_reject);
		if (tresult == ISC_R_EXISTS) {
			continue;
		}
		tresult = get_remoteservers_def(listname, config, &obj);
		if (tresult != ISC_R_SUCCESS) {
			if (result == ISC_R_SUCCESS) {
				result = tresult;
			}
			cfg_obj_log(addr, ISC_LOG_ERROR,
				    "unable to find remote-servers list '%s'",
				    listname);
			continue;
		}
		/* Grow stack? */
		if (stackcount == pushed) {
			stack = isc_mem_creget(mctx, stack, stackcount,
					       stackcount + 16,
					       sizeof(stack[0]));
			stackcount += 16;
		}
		stack[pushed++] = UNCONST(cfg_list_next(element));
		goto newlist;
	}
	if (pushed != 0) {
		element = stack[--pushed];
		goto resume;
	}
	if (stack != NULL) {
		isc_mem_cput(mctx, stack, stackcount, sizeof(*stack));
	}
	isc_symtab_destroy(&symtab);
	*countp = count;
	return result;
}

static isc_result_t
check_update_policy(const cfg_obj_t *policy) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	dns_fixedname_t fixed_id, fixed_name;
	dns_name_t *id = NULL, *name = NULL;
	const char *str = NULL;
	isc_textregion_t r;
	dns_rdatatype_t type;

	/* Check for "update-policy local;" */
	if (cfg_obj_isstring(policy) &&
	    strcmp("local", cfg_obj_asstring(policy)) == 0)
	{
		return ISC_R_SUCCESS;
	}

	/* Now check the grant policy */
	CFG_LIST_FOREACH (policy, element) {
		const cfg_obj_t *stmt = cfg_listelt_value(element);
		const cfg_obj_t *identity = cfg_tuple_get(stmt, "identity");
		const cfg_obj_t *matchtype = cfg_tuple_get(stmt, "matchtype");
		const cfg_obj_t *dname = cfg_tuple_get(stmt, "name");
		const cfg_obj_t *typelist = cfg_tuple_get(stmt, "types");
		dns_ssumatchtype_t mtype;

		id = dns_fixedname_initname(&fixed_id);
		name = dns_fixedname_initname(&fixed_name);

		tresult = dns_ssu_mtypefromstring(cfg_obj_asstring(matchtype),
						  &mtype);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(identity, ISC_LOG_ERROR,
				    "has a bad match-type");
		}

		str = cfg_obj_asstring(identity);
		tresult = dns_name_fromstring(id, str, dns_rootname, 1, NULL);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(identity, ISC_LOG_ERROR,
				    "'%s' is not a valid name", str);
			result = tresult;
		}

		/*
		 * There is no name field for subzone and dname is void
		 */
		if (mtype == dns_ssumatchtype_subdomain &&
		    cfg_obj_isvoid(dname))
		{
			str = "."; /* Use "." as a replacement. */
		} else {
			str = cfg_obj_asstring(dname);
		}
		if (tresult == ISC_R_SUCCESS) {
			tresult = dns_name_fromstring(name, str, dns_rootname,
						      0, NULL);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(dname, ISC_LOG_ERROR,
					    "'%s' is not a valid name", str);
				result = tresult;
			}
		}

		if (tresult == ISC_R_SUCCESS &&
		    mtype == dns_ssumatchtype_wildcard &&
		    !dns_name_iswildcard(name))
		{
			cfg_obj_log(identity, ISC_LOG_ERROR,
				    "'%s' is not a wildcard", str);
			result = ISC_R_FAILURE;
		}

		/*
		 * For some match types, the name should be a placeholder
		 * value, either "." or the same as identity.
		 */
		switch (mtype) {
		case dns_ssumatchtype_self:
		case dns_ssumatchtype_selfsub:
		case dns_ssumatchtype_selfwild:
			if (tresult == ISC_R_SUCCESS &&
			    (!dns_name_equal(id, name) &&
			     !dns_name_equal(dns_rootname, name)))
			{
				cfg_obj_log(identity, ISC_LOG_ERROR,
					    "identity and name fields are not "
					    "the same");
				result = ISC_R_FAILURE;
			}
			break;
		case dns_ssumatchtype_selfkrb5:
		case dns_ssumatchtype_selfms:
		case dns_ssumatchtype_selfsubkrb5:
		case dns_ssumatchtype_selfsubms:
		case dns_ssumatchtype_tcpself:
		case dns_ssumatchtype_6to4self:
			if (tresult == ISC_R_SUCCESS &&
			    !dns_name_equal(dns_rootname, name))
			{
				cfg_obj_log(identity, ISC_LOG_ERROR,
					    "name field not set to "
					    "placeholder value '.'");
				result = ISC_R_FAILURE;
			}
			break;
		case dns_ssumatchtype_name:
		case dns_ssumatchtype_subdomain: /* also zonesub */
		case dns_ssumatchtype_subdomainms:
		case dns_ssumatchtype_subdomainselfmsrhs:
		case dns_ssumatchtype_subdomainkrb5:
		case dns_ssumatchtype_subdomainselfkrb5rhs:
		case dns_ssumatchtype_wildcard:
		case dns_ssumatchtype_external:
		case dns_ssumatchtype_local:
			if (tresult == ISC_R_SUCCESS) {
				r.base = UNCONST(str);
				r.length = strlen(str);
				tresult = dns_rdatatype_fromtext(&type, &r);
			}
			if (tresult == ISC_R_SUCCESS) {
				cfg_obj_log(identity, ISC_LOG_ERROR,
					    "missing name field type '%s' "
					    "found",
					    str);
				result = ISC_R_FAILURE;
				break;
			}
			break;
		default:
			UNREACHABLE();
		}

		CFG_LIST_FOREACH (typelist, element2) {
			const cfg_obj_t *typeobj;
			const char *bracket;

			typeobj = cfg_listelt_value(element2);
			r.base = UNCONST(cfg_obj_asstring(typeobj));

			bracket = strchr(r.base, '(' /*)*/);
			if (bracket != NULL) {
				char *end = NULL;
				unsigned long max;

				r.length = bracket - r.base;
				max = strtoul(bracket + 1, &end, 10);
				if (max > 0xffff || end[0] != /*(*/ ')' ||
				    end[1] != 0)
				{
					cfg_obj_log(typeobj, ISC_LOG_ERROR,
						    "'%s' is not a valid count",
						    bracket);
					result = DNS_R_SYNTAX;
				}
			} else {
				r.length = strlen(r.base);
			}

			tresult = dns_rdatatype_fromtext(&type, &r);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(typeobj, ISC_LOG_ERROR,
					    "'%.*s' is not a valid type",
					    (int)r.length, r.base);
				result = tresult;
			}
		}
	}
	return result;
}

typedef struct {
	const char *name;
	unsigned int allowed;
} optionstable;

static isc_result_t
check_nonzero(const cfg_obj_t *options) {
	isc_result_t result = ISC_R_SUCCESS;
	const cfg_obj_t *obj = NULL;
	unsigned int i;

	static const char *nonzero[] = { "max-retry-time", "min-retry-time",
					 "max-refresh-time",
					 "min-refresh-time" };
	/*
	 * Check if value is zero.
	 */
	for (i = 0; i < sizeof(nonzero) / sizeof(nonzero[0]); i++) {
		obj = NULL;
		if (cfg_map_get(options, nonzero[i], &obj) == ISC_R_SUCCESS &&
		    cfg_obj_asuint32(obj) == 0)
		{
			cfg_obj_log(obj, ISC_LOG_ERROR, "'%s' must not be zero",
				    nonzero[i]);
			result = ISC_R_FAILURE;
		}
	}
	return result;
}

/*%
 * Check whether NOTIFY configuration at the zone level is acceptable for a
 * mirror zone.  Return true if it is; return false otherwise.
 */
static bool
check_mirror_zone_notify(const cfg_obj_t *zoptions, const char *znamestr) {
	bool notify_configuration_ok = true;
	const cfg_obj_t *obj = NULL;

	(void)cfg_map_get(zoptions, "notify", &obj);
	if (obj == NULL) {
		/*
		 * "notify" not set at zone level.  This is fine.
		 */
		return true;
	}

	if (cfg_obj_isboolean(obj)) {
		if (cfg_obj_asboolean(obj)) {
			/*
			 * "notify yes;" set at zone level.  This is an error.
			 */
			notify_configuration_ok = false;
		}
	} else {
		const char *notifystr = cfg_obj_asstring(obj);
		if (strcasecmp(notifystr, "explicit") != 0) {
			/*
			 * Something else than "notify explicit;" set at zone
			 * level.  This is an error.
			 */
			notify_configuration_ok = false;
		}
	}

	if (!notify_configuration_ok) {
		cfg_obj_log(zoptions, ISC_LOG_ERROR,
			    "zone '%s': mirror zones can only be used with "
			    "'notify no;' or 'notify explicit;'",
			    znamestr);
	}

	return notify_configuration_ok;
}

/*%
 * Try to determine whether recursion is available in a view without resorting
 * to extraordinary measures: just check the "recursion" and "allow-recursion"
 * settings.  The point is to prevent accidental mirror zone misuse rather than
 * to enforce some sort of policy.  Recursion is assumed to be allowed by
 * default if it is not explicitly disabled.
 */
static bool
check_recursion(const cfg_obj_t *config, const cfg_obj_t *voptions,
		const cfg_obj_t *goptions, cfg_aclconfctx_t *actx,
		isc_mem_t *mctx) {
	dns_acl_t *acl = NULL;
	const cfg_obj_t *obj;
	isc_result_t result = ISC_R_SUCCESS;
	bool retval = true;

	/*
	 * Check the "recursion" option first.
	 */
	obj = NULL;
	result = ISC_R_NOTFOUND;
	if (voptions != NULL) {
		result = cfg_map_get(voptions, "recursion", &obj);
	}
	if (result != ISC_R_SUCCESS && goptions != NULL) {
		result = cfg_map_get(goptions, "recursion", &obj);
	}
	if (result == ISC_R_SUCCESS && !cfg_obj_asboolean(obj)) {
		retval = false;
		goto cleanup;
	}

	/*
	 * If recursion is not disabled by the "recursion" option, check
	 * whether it is disabled by the "allow-recursion" ACL.
	 */
	obj = NULL;
	result = ISC_R_NOTFOUND;
	if (voptions != NULL) {
		result = cfg_map_get(voptions, "allow-recursion", &obj);
	}
	if (result != ISC_R_SUCCESS && goptions != NULL) {
		result = cfg_map_get(goptions, "allow-recursion", &obj);
	}
	if (result == ISC_R_SUCCESS) {
		result = cfg_acl_fromconfig(obj, config, actx, mctx, 0, &acl);
		if (result != ISC_R_SUCCESS) {
			goto cleanup;
		}
		retval = !dns_acl_isnone(acl);
	}

cleanup:
	if (acl != NULL) {
		dns_acl_detach(&acl);
	}

	return retval;
}

static isc_result_t
check_keydir(const cfg_obj_t *config, const cfg_obj_t *zconfig,
	     dns_name_t *zname, const char *name, const char *keydir,
	     isc_symtab_t *keydirs, isc_mem_t *mctx) {
	const char *dir = keydir;
	isc_result_t ret, result = ISC_R_SUCCESS;
	bool do_cleanup = false;
	bool done = false;
	bool keystore = false;
	const cfg_obj_t *kasps = NULL;
	dns_kasp_t *kasp = NULL;
	dns_kasplist_t kasplist;
	const cfg_obj_t *keystores = NULL;
	dns_keystorelist_t kslist;

	/* If no dnssec-policy or key-store, use the dir (key-directory) */
	(void)cfg_map_get(config, "dnssec-policy", &kasps);
	(void)cfg_map_get(config, "key-store", &keystores);
	if (kasps == NULL || keystores == NULL) {
		goto check;
	}

	ISC_LIST_INIT(kasplist);
	ISC_LIST_INIT(kslist);
	do_cleanup = true;

	/*
	 * Build the keystore list.
	 */
	CFG_LIST_FOREACH (keystores, element) {
		cfg_obj_t *kcfg = cfg_listelt_value(element);
		(void)cfg_keystore_fromconfig(kcfg, mctx, &kslist, NULL);
	}
	(void)cfg_keystore_fromconfig(NULL, mctx, &kslist, NULL);

	/*
	 * Look for the dnssec-policy by name, which is the dnssec-policy
	 * for the zone in question.
	 */
	CFG_LIST_FOREACH (kasps, element) {
		cfg_obj_t *kconfig = cfg_listelt_value(element);
		const cfg_obj_t *kaspobj = NULL;

		if (!cfg_obj_istuple(kconfig)) {
			continue;
		}

		kaspobj = cfg_tuple_get(kconfig, "name");
		if (strcmp(name, cfg_obj_asstring(kaspobj)) != 0) {
			continue;
		}

		ret = cfg_kasp_fromconfig(kconfig, NULL, false, mctx, &kslist,
					  &kasplist, &kasp);
		if (ret != ISC_R_SUCCESS) {
			kasp = NULL;
		}

		break;
	}
	if (kasp == NULL) {
		goto check;
	}

	/* Check key-stores of keys */
	dns_kasp_freeze(kasp);
	ISC_LIST_FOREACH (dns_kasp_keys(kasp), kkey, link) {
		dns_keystore_t *kks = dns_kasp_key_keystore(kkey);
		dir = dns_keystore_directory(kks, keydir);
		keystore = (kks != NULL && strcmp(DNS_KEYSTORE_KEYDIRECTORY,
						  dns_keystore_name(kks)) != 0);

		ret = keydirexist(zconfig,
				  keystore ? "key-store directory"
					   : "key-directory",
				  zname, dir, name, keydirs, mctx);
		if (ret != ISC_R_SUCCESS) {
			result = ret;
		}
	}
	dns_kasp_thaw(kasp);
	done = true;

check:
	if (!done) {
		ret = keydirexist(zconfig, "key-directory", zname, dir, name,
				  keydirs, mctx);
		if (ret != ISC_R_SUCCESS) {
			result = ret;
		}
	}

	if (do_cleanup) {
		if (kasp != NULL) {
			dns_kasp_detach(&kasp);
		}
		ISC_LIST_FOREACH (kasplist, k, link) {
			ISC_LIST_UNLINK(kasplist, k, link);
			dns_kasp_detach(&k);
		}
		ISC_LIST_FOREACH (kslist, ks, link) {
			ISC_LIST_UNLINK(kslist, ks, link);
			dns_keystore_detach(&ks);
		}
	}

	return result;
}

/*
 * Try to find a zone option in one of up to four levels of options:
 * for example, the zone, template, view, and global option blocks.
 * (Fewer levels can be specified for options that aren't defined at
 * all four levels.)
 */
static isc_result_t
get_zoneopt(const cfg_obj_t *opts1, const cfg_obj_t *opts2,
	    const cfg_obj_t *opts3, const cfg_obj_t *opts4, const char *name,
	    const cfg_obj_t **objp) {
	isc_result_t result = ISC_R_NOTFOUND;

	REQUIRE(*objp == NULL);

	if (opts1 != NULL) {
		result = cfg_map_get(opts1, name, objp);
	}
	if (*objp == NULL && opts2 != NULL) {
		result = cfg_map_get(opts2, name, objp);
	}
	if (*objp == NULL && opts3 != NULL) {
		result = cfg_map_get(opts3, name, objp);
	}
	if (*objp == NULL && opts4 != NULL) {
		result = cfg_map_get(opts4, name, objp);
	}

	return result;
}

isc_result_t
isccfg_check_zoneconf(const cfg_obj_t *zconfig, const cfg_obj_t *voptions,
		      const cfg_obj_t *config, isc_symtab_t *symtab,
		      isc_symtab_t *files, isc_symtab_t *keydirs,
		      isc_symtab_t *inview, const char *viewname,
		      dns_rdataclass_t defclass, cfg_aclconfctx_t *actx,
		      isc_mem_t *mctx) {
	const char *znamestr = NULL;
	const char *typestr = NULL;
	const char *target = NULL;
	const char *tmplname = NULL;
	int ztype;
	const cfg_obj_t *zoptions = NULL, *toptions = NULL, *goptions = NULL;
	const cfg_obj_t *obj = NULL, *kasp = NULL;
	const cfg_obj_t *templates = NULL, *inviewobj = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	unsigned int i = 0;
	dns_rdataclass_t zclass;
	dns_fixedname_t fixedname;
	dns_name_t *zname = NULL; /* NULL if parsing of zone name fails. */
	isc_buffer_t b;
	bool root = false;
	bool rfc1918 = false;
	bool ula = false;
	bool dlz;
	bool ddns = false;
	bool has_dnssecpolicy = false;
	bool kasp_inlinesigning = false;
	bool inline_signing = false;
	const void *clauses = NULL;
	const char *option = NULL;
	const char *kaspname = NULL;
	const char *dir = NULL;
	static const char *acls[] = {
		"allow-notify",
		"allow-transfer",
		"allow-update",
		"allow-update-forwarding",
	};

	znamestr = cfg_obj_asstring(cfg_tuple_get(zconfig, "name"));

	zoptions = cfg_tuple_get(zconfig, "options");

	if (config != NULL) {
		cfg_map_get(config, "options", &goptions);
	}

	/* If the zone specifies a template, find it too */
	(void)cfg_map_get(config, "template", &templates);
	(void)cfg_map_get(zoptions, "template", &obj);
	if (obj != NULL) {
		tmplname = cfg_obj_asstring(obj);

		CFG_LIST_FOREACH (templates, e) {
			const cfg_obj_t *t = cfg_tuple_get(cfg_listelt_value(e),
							   "name");
			if (strcasecmp(cfg_obj_asstring(t), tmplname) == 0) {
				toptions = cfg_tuple_get(cfg_listelt_value(e),
							 "options");
				break;
			}
		}

		if (toptions == NULL) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "zone '%s': template '%s' not found",
				    znamestr, tmplname);
			return ISC_R_FAILURE;
		}
	}

	(void)cfg_map_get(zoptions, "in-view", &inviewobj);
	if (inviewobj != NULL) {
		target = cfg_obj_asstring(inviewobj);
		ztype = CFG_ZONE_INVIEW;
	} else {
		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL, "type", &obj);
		if (obj == NULL) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "zone '%s': type not present", znamestr);
			return ISC_R_FAILURE;
		}

		typestr = cfg_obj_asstring(obj);
		if (strcasecmp(typestr, "master") == 0 ||
		    strcasecmp(typestr, "primary") == 0)
		{
			ztype = CFG_ZONE_PRIMARY;
		} else if (strcasecmp(typestr, "slave") == 0 ||
			   strcasecmp(typestr, "secondary") == 0)
		{
			ztype = CFG_ZONE_SECONDARY;
		} else if (strcasecmp(typestr, "mirror") == 0) {
			ztype = CFG_ZONE_MIRROR;
		} else if (strcasecmp(typestr, "stub") == 0) {
			ztype = CFG_ZONE_STUB;
		} else if (strcasecmp(typestr, "static-stub") == 0) {
			ztype = CFG_ZONE_STATICSTUB;
		} else if (strcasecmp(typestr, "forward") == 0) {
			ztype = CFG_ZONE_FORWARD;
		} else if (strcasecmp(typestr, "hint") == 0) {
			ztype = CFG_ZONE_HINT;
		} else if (strcasecmp(typestr, "redirect") == 0) {
			ztype = CFG_ZONE_REDIRECT;
		} else {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "zone '%s': invalid type %s", znamestr,
				    typestr);
			return ISC_R_FAILURE;
		}

		if (ztype == CFG_ZONE_REDIRECT && strcmp(znamestr, ".") != 0) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "redirect zones must be called \".\"");
			return ISC_R_FAILURE;
		}
	}

	obj = cfg_tuple_get(zconfig, "class");
	if (cfg_obj_isstring(obj)) {
		isc_textregion_t r;

		r.base = UNCONST(cfg_obj_asstring(obj));
		r.length = strlen(r.base);
		result = dns_rdataclass_fromtext(&zclass, &r);
		if (result != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "zone '%s': invalid class %s", znamestr,
				    r.base);
			return ISC_R_FAILURE;
		}
		if (zclass != defclass) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "zone '%s': class '%s' does not "
				    "match view/default class",
				    znamestr, r.base);
			return ISC_R_FAILURE;
		}
	} else {
		zclass = defclass;
	}

	/*
	 * Look for an already existing zone.
	 * We need to make this canonical as isc_symtab_define()
	 * deals with strings.
	 */
	dns_fixedname_init(&fixedname);
	isc_buffer_constinit(&b, znamestr, strlen(znamestr));
	isc_buffer_add(&b, strlen(znamestr));
	tresult = dns_name_fromtext(dns_fixedname_name(&fixedname), &b,
				    dns_rootname, DNS_NAME_DOWNCASE);
	if (tresult != ISC_R_SUCCESS) {
		cfg_obj_log(zconfig, ISC_LOG_ERROR,
			    "zone '%s': is not a valid name", znamestr);
		result = ISC_R_FAILURE;
	} else if (symtab != NULL && inview != NULL) {
		char namebuf[DNS_NAME_FORMATSIZE];
		char classbuf[DNS_RDATACLASS_FORMATSIZE];
		char *key = NULL;
		const char *vname = NULL;
		size_t len = 0;
		int n;

		zname = dns_fixedname_name(&fixedname);
		dns_name_format(zname, namebuf, sizeof(namebuf));
		dns_rdataclass_format(zclass, classbuf, sizeof(classbuf));

		tresult = exists(
			zconfig, namebuf,
			ztype == CFG_ZONE_HINT	     ? 1
			: ztype == CFG_ZONE_REDIRECT ? 2
						     : 3,
			symtab,
			"zone '%s': already exists previous definition: %s:%u",
			mctx);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
		if (dns_name_equal(zname, dns_rootname)) {
			root = true;
		} else if (dns_name_isrfc1918(zname)) {
			rfc1918 = true;
		} else if (dns_name_isula(zname)) {
			ula = true;
		}
		vname = (ztype == CFG_ZONE_INVIEW) ? target
			: (viewname != NULL)	   ? viewname
						   : "_default";
		len = strlen(namebuf) + strlen(classbuf) + strlen(vname) + 3;
		key = isc_mem_get(mctx, len);
		n = snprintf(key, len, "%s/%s/%s", namebuf, classbuf, vname);
		RUNTIME_CHECK(n > 0 && (size_t)n < len);
		switch (ztype) {
		case CFG_ZONE_INVIEW:
			tresult = isc_symtab_lookup(inview, key, 1, NULL);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(inviewobj, ISC_LOG_ERROR,
					    "'in-view' zone '%s' "
					    "does not exist in view '%s', "
					    "or view '%s' is not yet defined",
					    znamestr, target, target);
				if (result == ISC_R_SUCCESS) {
					result = tresult;
				}
			}
			break;

		case CFG_ZONE_FORWARD:
		case CFG_ZONE_REDIRECT:
			break;

		case CFG_ZONE_PRIMARY:
		case CFG_ZONE_SECONDARY:
		case CFG_ZONE_MIRROR:
		case CFG_ZONE_HINT:
		case CFG_ZONE_STUB:
		case CFG_ZONE_STATICSTUB: {
			char *tmp = isc_mem_strdup(mctx, key);
			isc_symvalue_t symvalue;
			symvalue.as_cpointer = NULL;
			tresult = isc_symtab_define(inview, tmp, 1, symvalue,
						    isc_symexists_replace);
			RUNTIME_CHECK(tresult == ISC_R_SUCCESS);
		} break;

		default:
			UNREACHABLE();
		}
		isc_mem_put(mctx, key, len);
	}

	if (ztype == CFG_ZONE_INVIEW) {
		const cfg_obj_t *fwd = NULL;
		unsigned int maxopts = 1;

		(void)cfg_map_get(zoptions, "forward", &fwd);
		if (fwd != NULL) {
			maxopts++;
		}
		fwd = NULL;
		(void)cfg_map_get(zoptions, "forwarders", &fwd);
		if (fwd != NULL) {
			maxopts++;
		}
		if (cfg_map_count(zoptions) > maxopts) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "zone '%s': 'in-view' used "
				    "with incompatible zone options",
				    znamestr);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
		return result;
	}

	/*
	 * Check if value is zero.
	 */
	if (check_nonzero(zoptions) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	/*
	 * Check if a dnssec-policy is set.
	 */
	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, voptions, goptions,
			  "dnssec-policy", &obj);
	if (obj != NULL) {
		kaspname = cfg_obj_asstring(obj);
		if (strcmp(kaspname, "default") == 0) {
			has_dnssecpolicy = true;
			kasp_inlinesigning = true;
		} else if (strcmp(kaspname, "insecure") == 0) {
			has_dnssecpolicy = true;
			kasp_inlinesigning = true;
		} else if (strcmp(kaspname, "none") == 0) {
			has_dnssecpolicy = false;
			kasp_inlinesigning = false;
		} else {
			const cfg_obj_t *kasps = NULL;
			(void)cfg_map_get(config, "dnssec-policy", &kasps);
			CFG_LIST_FOREACH (kasps, element) {
				const cfg_obj_t *kobj = cfg_tuple_get(
					cfg_listelt_value(element), "name");
				if (strcmp(kaspname, cfg_obj_asstring(kobj)) ==
				    0)
				{
					const cfg_obj_t *inlinesigning = NULL;
					const cfg_obj_t *kopt = cfg_tuple_get(
						cfg_listelt_value(element),
						"options");
					if (cfg_map_get(kopt, "inline-signing",
							&inlinesigning) ==
					    ISC_R_SUCCESS)
					{
						kasp_inlinesigning =
							cfg_obj_asboolean(
								inlinesigning);
					} else {
						/* By default true */
						kasp_inlinesigning = true;
					}

					has_dnssecpolicy = true;
					break;
				}
			}

			if (!has_dnssecpolicy) {
				cfg_obj_log(zconfig, ISC_LOG_ERROR,
					    "zone '%s': option "
					    "'dnssec-policy %s' has no "
					    "matching dnssec-policy config",
					    znamestr, kaspname);
				if (result == ISC_R_SUCCESS) {
					result = ISC_R_FAILURE;
				}
			}
		}
		if (has_dnssecpolicy) {
			kasp = obj;
		}
	}

	/*
	 * Reject zones with both dnssec-policy and max-zone-ttl
	 * */
	if (has_dnssecpolicy) {
		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, voptions, goptions,
				  "max-zone-ttl", &obj);
		if (obj != NULL) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "zone '%s': option 'max-zone-ttl' "
				    "cannot be used together with "
				    "'dnssec-policy'",
				    znamestr);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	/*
	 * Check validity of the zone options.
	 */
	option = cfg_map_firstclause(&cfg_type_zoneopts, &clauses, &i);
	while (option != NULL) {
		obj = NULL;
		bool topt = false;
		(void)cfg_map_get(zoptions, option, &obj);
		if (obj == NULL && toptions != NULL) {
			(void)cfg_map_get(toptions, option, &obj);
			topt = true;
		}
		if (obj != NULL && !cfg_clause_validforzone(option, ztype)) {
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "option '%s' is not allowed "
				    "in '%s' zone '%s'%s%s%s",
				    option, typestr, znamestr,
				    topt ? " (referencing template '" : "",
				    topt ? tmplname : "", topt ? "')" : "");
			result = ISC_R_FAILURE;
		}
		option = cfg_map_nextclause(&cfg_type_zoneopts, &clauses, &i);
	}

	/*
	 * Check that ACLs expand correctly.
	 */
	for (i = 0; i < ARRAY_SIZE(acls); i++) {
		tresult = checkacl(acls[i], actx, zconfig, voptions, config,
				   mctx);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	/*
	 * Only a limited subset of all possible "notify" settings can be used
	 * at the zone level for mirror zones.
	 */
	if (ztype == CFG_ZONE_MIRROR &&
	    !check_mirror_zone_notify(zoptions, znamestr))
	{
		result = ISC_R_FAILURE;
	}

	/*
	 * Primary, secondary, and mirror zones may have an "also-notify"
	 * field, but shouldn't if notify is disabled.
	 */
	if (ztype == CFG_ZONE_PRIMARY || ztype == CFG_ZONE_SECONDARY ||
	    ztype == CFG_ZONE_MIRROR)
	{
		bool donotify = true;

		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, voptions, goptions,
				  "notify", &obj);
		if (obj != NULL) {
			if (cfg_obj_isboolean(obj)) {
				donotify = cfg_obj_asboolean(obj);
			} else {
				const char *str = cfg_obj_asstring(obj);
				if (ztype != CFG_ZONE_PRIMARY &&
				    (strcasecmp(str, "master-only") == 0 ||
				     strcasecmp(str, "primary-only") == 0))
				{
					donotify = false;
				}
			}
		}

		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL, "also-notify",
				  &obj);
		if (obj != NULL && !donotify) {
			cfg_obj_log(zoptions, ISC_LOG_WARNING,
				    "zone '%s': 'also-notify' set but "
				    "'notify' is disabled",
				    znamestr);
		}
		if (obj == NULL) {
			(void)get_zoneopt(voptions, goptions, NULL, NULL,
					  "also-notify", &obj);
		}
		if (obj != NULL && donotify) {
			uint32_t count;
			tresult = validate_remotes(obj, config, &count, mctx);
			if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS)
			{
				result = tresult;
			}
		}
	}

	/*
	 * Secondary, mirror, and stub zones must have a "primaries" field,
	 * with one exception: when mirroring the root zone, a default,
	 * built-in primary server list is used in the absence of one
	 * explicitly specified.
	 */
	if (ztype == CFG_ZONE_SECONDARY || ztype == CFG_ZONE_STUB ||
	    (ztype == CFG_ZONE_MIRROR && zname != NULL &&
	     !dns_name_equal(zname, dns_rootname)))
	{
		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL, "primaries",
				  &obj);
		if (obj == NULL) {
			/* If "primaries" was unset, check for "masters" */
			(void)get_zoneopt(zoptions, toptions, NULL, NULL,
					  "masters", &obj);
		} else {
			const cfg_obj_t *obj2 = NULL;

			/* ...bug if it was set, "masters" must not be. */
			(void)get_zoneopt(zoptions, toptions, NULL, NULL,
					  "masters", &obj2);
			if (obj2 != NULL) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "'primaries' and 'masters' cannot "
					    "both be used in the same zone");
				result = ISC_R_FAILURE;
			}
		}
		if (obj == NULL) {
			cfg_obj_log(zoptions, ISC_LOG_ERROR,
				    "zone '%s': missing 'primaries' entry",
				    znamestr);
			result = ISC_R_FAILURE;
		} else {
			uint32_t count;
			tresult = validate_remotes(obj, config, &count, mctx);
			if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS)
			{
				result = tresult;
			}
			if (tresult == ISC_R_SUCCESS && count == 0) {
				cfg_obj_log(zoptions, ISC_LOG_ERROR,
					    "zone '%s': "
					    "empty 'primaries' entry",
					    znamestr);
				result = ISC_R_FAILURE;
			}
		}
	}

	/*
	 * Primary and secondary zones that have a "parental-agents" field,
	 * must have a corresponding "parental-agents" clause.
	 */
	if (ztype == CFG_ZONE_PRIMARY || ztype == CFG_ZONE_SECONDARY) {
		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL,
				  "parental-agents", &obj);
		if (obj != NULL) {
			uint32_t count;
			tresult = validate_remotes(obj, config, &count, mctx);
			if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS)
			{
				result = tresult;
			}
			if (tresult == ISC_R_SUCCESS && count == 0) {
				cfg_obj_log(zoptions, ISC_LOG_ERROR,
					    "zone '%s': "
					    "empty 'parental-agents' entry",
					    znamestr);
				result = ISC_R_FAILURE;
			}
		}
	}

	/*
	 * Configuring a mirror zone and disabling recursion at the same time
	 * contradicts the purpose of the former.
	 */
	if (ztype == CFG_ZONE_MIRROR &&
	    !check_recursion(config, voptions, goptions, actx, mctx))
	{
		cfg_obj_log(zoptions, ISC_LOG_ERROR,
			    "zone '%s': mirror zones cannot be used if "
			    "recursion is disabled",
			    znamestr);
		result = ISC_R_FAILURE;
	}

	/*
	 * Primary zones can't have both "allow-update" and "update-policy".
	 */
	if (ztype == CFG_ZONE_PRIMARY || ztype == CFG_ZONE_SECONDARY) {
		bool signing = false;
		const cfg_obj_t *au = NULL, *up = NULL;

		(void)get_zoneopt(zoptions, toptions, NULL, NULL,
				  "allow-update", &au);
		(void)get_zoneopt(zoptions, toptions, NULL, NULL,
				  "update-policy", &up);

		if (au != NULL && up != NULL) {
			cfg_obj_log(au, ISC_LOG_ERROR,
				    "zone '%s': 'allow-update' is ignored "
				    "when 'update-policy' is present",
				    znamestr);
			result = ISC_R_FAILURE;
		} else if (up != NULL) {
			tresult = check_update_policy(up);
			if (tresult != ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}

		/*
		 * To determine whether dnssec-policy is allowed,
		 * we should also check for allow-update at the
		 * view and options levels.
		 */
		if (au == NULL) {
			(void)get_zoneopt(voptions, goptions, NULL, NULL,
					  "allow-update", &au);
		}

		if (up != NULL) {
			ddns = true;
		} else if (au != NULL) {
			dns_acl_t *acl = NULL;
			tresult = cfg_acl_fromconfig(au, config, actx, mctx, 0,
						     &acl);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(au, ISC_LOG_ERROR,
					    "acl expansion failed: %s",
					    isc_result_totext(result));
				result = ISC_R_FAILURE;
			} else if (acl != NULL) {
				if (!dns_acl_isnone(acl)) {
					ddns = true;
				}
				dns_acl_detach(&acl);
			}
		}

		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL,
				  "inline-signing", &obj);
		if (obj != NULL) {
			inline_signing = signing = cfg_obj_asboolean(obj);
		} else if (has_dnssecpolicy) {
			signing = kasp_inlinesigning;
		}

		if (has_dnssecpolicy) {
			if (!ddns && !signing) {
				cfg_obj_log(kasp, ISC_LOG_ERROR,
					    "'inline-signing yes;' must also "
					    "be configured explicitly for "
					    "zones using dnssec-policy%s. See "
					    "https://kb.isc.org/docs/"
					    "dnssec-policy-requires-dynamic-"
					    "dns-or-inline-signing",
					    (ztype == CFG_ZONE_PRIMARY)
						    ? " without a configured "
						      "'allow-update' or "
						      "'update-policy'"
						    : "");
				result = ISC_R_FAILURE;
			}
		}

		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL,
				  "sig-signing-type", &obj);
		if (obj != NULL) {
			uint32_t type = cfg_obj_asuint32(obj);
			if (type < 0xff00U || type > 0xffffU) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "sig-signing-type: %u out of "
					    "range [%u..%u]",
					    type, 0xff00U, 0xffffU);
				result = ISC_R_FAILURE;
			}
		}

		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL,
				  "dnssec-loadkeys-interval", &obj);
		if (obj != NULL && ztype == CFG_ZONE_SECONDARY && !signing) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "dnssec-loadkeys-interval: requires "
				    "inline-signing when used in secondary "
				    "zone");
			result = ISC_R_FAILURE;
		}
	}

	/*
	 * Check that forwarding is reasonable.
	 */
	obj = NULL;
	if (root) {
		(void)get_zoneopt(voptions, goptions, NULL, NULL, "forwarders",
				  &obj);
	}
	if (check_forward(config, zoptions, obj) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	/*
	 * Check that a RFC 1918 / ULA reverse zone is not forward first
	 * unless explicitly configured to be so.
	 */
	if (ztype == CFG_ZONE_FORWARD && (rfc1918 || ula)) {
		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL, "forward",
				  &obj);
		if (obj == NULL) {
			/*
			 * Forward mode not explicitly configured
			 * at the zone or template level.
			 */
			(void)get_zoneopt(voptions, goptions, NULL, NULL,
					  "forward", &obj);
			if (obj == NULL ||
			    strcasecmp(cfg_obj_asstring(obj), "first") == 0)
			{
				cfg_obj_log(zconfig, ISC_LOG_WARNING,
					    "inherited 'forward first;' for "
					    "%s zone '%s' - did you want "
					    "'forward only;'?",
					    rfc1918 ? "rfc1918" : "ula",
					    znamestr);
			}
		}
	}

	/*
	 * Check validity of static stub server addresses.
	 */
	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, NULL, NULL, "server-addresses",
			  &obj);
	if (ztype == CFG_ZONE_STATICSTUB && obj != NULL) {
		CFG_LIST_FOREACH (obj, element) {
			isc_sockaddr_t sa;
			isc_netaddr_t na;
			obj = cfg_listelt_value(element);
			sa = *cfg_obj_assockaddr(obj);

			isc_netaddr_fromsockaddr(&na, &sa);
			if (isc_netaddr_getzone(&na) != 0) {
				result = ISC_R_FAILURE;
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "scoped address is not allowed "
					    "for static stub "
					    "server-addresses");
			}
		}
	}

	/*
	 * Check validity of static stub server names.
	 */
	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, NULL, NULL, "server-names", &obj);
	if (zname != NULL && ztype == CFG_ZONE_STATICSTUB && obj != NULL) {
		CFG_LIST_FOREACH (obj, element) {
			const char *snamestr = NULL;
			dns_fixedname_t fixed_sname;
			isc_buffer_t b2;
			dns_name_t *sname = NULL;

			obj = cfg_listelt_value(element);
			snamestr = cfg_obj_asstring(obj);

			isc_buffer_constinit(&b2, snamestr, strlen(snamestr));
			isc_buffer_add(&b2, strlen(snamestr));
			sname = dns_fixedname_initname(&fixed_sname);
			tresult = dns_name_fromtext(sname, &b2, dns_rootname,
						    0);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(zconfig, ISC_LOG_ERROR,
					    "server-name '%s' is not a valid "
					    "name",
					    snamestr);
				result = ISC_R_FAILURE;
			} else if (dns_name_issubdomain(sname, zname)) {
				cfg_obj_log(zconfig, ISC_LOG_ERROR,
					    "server-name '%s' must not be a "
					    "subdomain of zone name '%s'",
					    snamestr, znamestr);
				result = ISC_R_FAILURE;
			}
		}
	}

	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, NULL, NULL, "send-report-channel",
			  &obj);
	if (obj != NULL) {
		const char *str = cfg_obj_asstring(obj);
		dns_fixedname_t fad;
		dns_name_t *ad = dns_fixedname_initname(&fad);

		tresult = dns_name_fromstring(ad, str, dns_rootname, 0, NULL);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'%s' is not a valid name", str);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		} else if (dns_name_issubdomain(ad, zname)) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "send-report-channel '%s' cannot "
				    "be at or below the zone name '%s'",
				    str, znamestr);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	/*
	 * Warn if key-directory doesn't exist
	 */
	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, voptions, goptions,
			  "key-directory", &obj);
	if (obj != NULL) {
		dir = cfg_obj_asstring(obj);

		tresult = isc_file_isdirectory(dir);
		switch (tresult) {
		case ISC_R_SUCCESS:
			break;
		case ISC_R_FILENOTFOUND:
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "key-directory: '%s' does not exist", dir);
			break;
		case ISC_R_INVALIDFILE:
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "key-directory: '%s' is not a directory",
				    dir);
			break;
		default:
			cfg_obj_log(obj, ISC_LOG_WARNING,
				    "key-directory: '%s' %s", dir,
				    isc_result_totext(tresult));
			if (result == ISC_R_SUCCESS) {
				result = tresult;
			}
		}
	}

	/*
	 * Make sure there is no other zone with the same key directory (from
	 * (key-directory or key-store/directory) and a different dnssec-policy.
	 */
	if (zname != NULL && keydirs != NULL) {
		if (has_dnssecpolicy) {
			tresult = check_keydir(config, zconfig, zname, kaspname,
					       dir, keydirs, mctx);
		} else {
			tresult = keydirexist(zconfig, "key-directory", zname,
					      dir, kaspname, keydirs, mctx);
		}
		if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	/*
	 * "log-report-channel" cannot be set for the root zone.
	 */
	if (ztype == CFG_ZONE_PRIMARY || ztype == CFG_ZONE_SECONDARY) {
		obj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL,
				  "log-report-channel", &obj);
		if (obj != NULL && cfg_obj_asboolean(obj) &&
		    dns_name_equal(zname, dns_rootname))
		{
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "'log-report-channel' cannot be set in "
				    "the root zone");
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	/*
	 * Check various options.
	 */
	tresult = check_options(zoptions, config, false, mctx, optlevel_zone);
	if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS) {
		result = tresult;
	}

	/*
	 * If the zone type is rbt then primary/hint zones require file
	 * clauses. If inline-signing is used, then secondary zones require a
	 * file clause as well.
	 */
	obj = NULL;
	dlz = false;
	(void)get_zoneopt(zoptions, toptions, NULL, NULL, "dlz", &obj);
	if (obj != NULL) {
		dlz = true;
	}

	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, NULL, NULL, "database", &obj);
	if (dlz && obj != NULL) {
		cfg_obj_log(zconfig, ISC_LOG_ERROR,
			    "zone '%s': cannot specify both 'dlz' "
			    "and 'database'",
			    znamestr);
		if (result == ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	} else if (!dlz && (obj == NULL ||
			    strcmp(ZONEDB_DEFAULT, cfg_obj_asstring(obj)) == 0))
	{
		const cfg_obj_t *fileobj = NULL;
		(void)get_zoneopt(zoptions, toptions, NULL, NULL, "file",
				  &fileobj);
		if (fileobj == NULL &&
		    (ztype == CFG_ZONE_PRIMARY || ztype == CFG_ZONE_HINT ||
		     (ztype == CFG_ZONE_SECONDARY && inline_signing)))
		{
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "zone '%s': missing 'file' entry",
				    znamestr);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		} else if (fileobj != NULL && files != NULL &&
			   (ztype == CFG_ZONE_SECONDARY ||
			    ztype == CFG_ZONE_MIRROR || ddns ||
			    has_dnssecpolicy))
		{
			tresult = fileexist(fileobj, files, true);
			if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS)
			{
				result = tresult;
			}
		} else if (fileobj != NULL && files != NULL &&
			   (ztype == CFG_ZONE_PRIMARY ||
			    ztype == CFG_ZONE_HINT))
		{
			tresult = fileexist(fileobj, files, false);
			if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS)
			{
				result = tresult;
			}
		}
	}

	/*
	 * Check that masterfile-format and masterfile-style are
	 * consistent.
	 */
	obj = NULL;
	tresult = get_zoneopt(zoptions, toptions, voptions, goptions,
			      "masterfile-format", &obj);
	if (tresult == ISC_R_SUCCESS &&
	    strcasecmp(cfg_obj_asstring(obj), "raw") == 0)
	{
		obj = NULL;
		tresult = get_zoneopt(zoptions, toptions, voptions, goptions,
				      "masterfile-style", &obj);
		if (tresult == ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "zone '%s': 'masterfile-style' "
				    "can only be used with "
				    "'masterfile-format text'",
				    znamestr);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}

	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, voptions, goptions,
			  "max-journal-size", &obj);
	if (obj != NULL && cfg_obj_isuint64(obj)) {
		uint64_t value = cfg_obj_asuint64(obj);
		if (value > DNS_JOURNAL_SIZE_MAX) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'max-journal-size %" PRId64 "' "
				    "is too large",
				    value);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}
	}

	obj = NULL;
	(void)get_zoneopt(zoptions, toptions, voptions, goptions,
			  "min-transfer-rate-in", &obj);
	if (obj != NULL) {
		uint32_t traffic_bytes =
			cfg_obj_asuint32(cfg_tuple_get(obj, "traffic_bytes"));
		uint32_t time_minutes =
			cfg_obj_asuint32(cfg_tuple_get(obj, "time_minutes"));
		if (traffic_bytes == 0) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "zone '%s': 'min-transfer-rate-in' bytes "
				    "value cannot be '0'",
				    znamestr);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}
		/* Max. 28 days (in minutes). */
		const unsigned int time_minutes_max = 28 * 24 * 60;
		if (time_minutes < 1 || time_minutes > time_minutes_max) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "zone '%s': 'min-transfer-rate-in' minutes "
				    "value is out of range (1..%u)",
				    znamestr, time_minutes_max);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_RANGE;
			}
		}
	}

	return result;
}

typedef struct keyalgorithms {
	const char *name;
	uint16_t size;
} algorithmtable;

isc_result_t
isccfg_check_key(const cfg_obj_t *key) {
	const cfg_obj_t *algobj = NULL;
	const cfg_obj_t *secretobj = NULL;
	const char *keyname = cfg_obj_asstring(cfg_map_getname(key));
	const char *algorithm;
	int i;
	size_t len = 0;
	isc_result_t result = ISC_R_SUCCESS;
	isc_buffer_t buf;
	unsigned char secretbuf[1024];
	static const algorithmtable algorithms[] = {
		{ "hmac-md5", 128 },
		{ "hmac-md5.sig-alg.reg.int", 0 },
		{ "hmac-md5.sig-alg.reg.int.", 0 },
		{ "hmac-sha1", 160 },
		{ "hmac-sha224", 224 },
		{ "hmac-sha256", 256 },
		{ "hmac-sha384", 384 },
		{ "hmac-sha512", 512 },
		{ NULL, 0 }
	};

	(void)cfg_map_get(key, "algorithm", &algobj);
	(void)cfg_map_get(key, "secret", &secretobj);
	if (secretobj == NULL || algobj == NULL) {
		cfg_obj_log(key, ISC_LOG_ERROR,
			    "key '%s' must have both 'secret' and "
			    "'algorithm' defined",
			    keyname);
		return ISC_R_FAILURE;
	}

	isc_buffer_init(&buf, secretbuf, sizeof(secretbuf));
	result = isc_base64_decodestring(cfg_obj_asstring(secretobj), &buf);
	if (result != ISC_R_SUCCESS) {
		cfg_obj_log(secretobj, ISC_LOG_ERROR, "bad secret '%s'",
			    isc_result_totext(result));
		return result;
	}

	algorithm = cfg_obj_asstring(algobj);
	for (i = 0; algorithms[i].name != NULL; i++) {
		len = strlen(algorithms[i].name);
		if (strncasecmp(algorithms[i].name, algorithm, len) == 0 &&
		    (algorithm[len] == '\0' ||
		     (algorithms[i].size != 0 && algorithm[len] == '-')))
		{
			break;
		}
	}
	if (algorithms[i].name == NULL) {
		cfg_obj_log(algobj, ISC_LOG_ERROR, "unknown algorithm '%s'",
			    algorithm);
		return ISC_R_NOTFOUND;
	}
	if (algorithm[len] == '-') {
		uint16_t digestbits;
		result = isc_parse_uint16(&digestbits, algorithm + len + 1, 10);
		if (result == ISC_R_SUCCESS || result == ISC_R_RANGE) {
			if (result == ISC_R_RANGE ||
			    digestbits > algorithms[i].size)
			{
				cfg_obj_log(algobj, ISC_LOG_ERROR,
					    "key '%s' digest-bits too large "
					    "[%u..%u]",
					    keyname, algorithms[i].size / 2,
					    algorithms[i].size);
				return ISC_R_RANGE;
			}
			if ((digestbits % 8) != 0) {
				cfg_obj_log(algobj, ISC_LOG_ERROR,
					    "key '%s' digest-bits not multiple"
					    " of 8",
					    keyname);
				return ISC_R_RANGE;
			}
			/*
			 * Recommended minima for hmac algorithms.
			 */
			if (digestbits < (algorithms[i].size / 2U) ||
			    (digestbits < 80U))
			{
				cfg_obj_log(algobj, ISC_LOG_WARNING,
					    "key '%s' digest-bits too small "
					    "[<%u]",
					    keyname, algorithms[i].size / 2);
			}
		} else {
			cfg_obj_log(algobj, ISC_LOG_ERROR,
				    "key '%s': unable to parse digest-bits",
				    keyname);
			return result;
		}
	}
	return ISC_R_SUCCESS;
}

typedef enum symtab_file_type {
	READ_ONLY = 1,
	WRITEABLE = 2,
} symtab_file_type_t;

static isc_result_t
fileexist(const cfg_obj_t *obj, isc_symtab_t *symtab, bool writeable) {
	isc_result_t result_ro, result_w;
	isc_symvalue_t symvalue_ro, symvalue_w;
	unsigned int line;
	const char *file;

	/*
	 * Since symtab doesn't let us query the file type, we need to query
	 * twice. Once per type.
	 */
	result_ro = isc_symtab_lookup(symtab, cfg_obj_asstring(obj), READ_ONLY,
				      &symvalue_ro);
	result_w = isc_symtab_lookup(symtab, cfg_obj_asstring(obj), WRITEABLE,
				     &symvalue_w);

	bool found_read_only = result_ro == ISC_R_SUCCESS;
	bool found_writable = result_w == ISC_R_SUCCESS;

	/*
	 * If either the new file, the old file or both files are writeable,
	 * bail out.
	 */
	if ((writeable && found_read_only) || found_writable) {
		isc_symvalue_t symvalue = (writeable && found_read_only)
						  ? symvalue_ro
						  : symvalue_w;

		file = cfg_obj_file(symvalue.as_cpointer);
		line = cfg_obj_line(symvalue.as_cpointer);
		cfg_obj_log(obj, ISC_LOG_ERROR,
			    "writeable file '%s': already in use: "
			    "%s:%u",
			    cfg_obj_asstring(obj), file, line);
		return ISC_R_EXISTS;
	} else if (found_read_only) {
		/* If a read only file is already in the hashtable, we have
		 * nothing to do */
		return ISC_R_SUCCESS;
	} else {
		/* If the file was not present already in the hashmap, add it */
		isc_symvalue_t symvalue =
			(isc_symvalue_t){ .as_cpointer = obj };
		symtab_file_type_t type = writeable ? WRITEABLE : READ_ONLY;
		isc_result_t result =
			isc_symtab_define(symtab, cfg_obj_asstring(obj), type,
					  symvalue, isc_symexists_reject);
		return result;
	}
}

static isc_result_t
keydirexist(const cfg_obj_t *zcfg, const char *optname, dns_name_t *zname,
	    const char *dirname, const char *kaspnamestr, isc_symtab_t *symtab,
	    isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_symvalue_t symvalue;
	char *symkey;
	char keydirbuf[DNS_NAME_FORMATSIZE + 128];
	char *keydir = keydirbuf;
	size_t len = sizeof(keydirbuf);
	size_t n;

	if (kaspnamestr == NULL || strcmp(kaspnamestr, "none") == 0) {
		return ISC_R_SUCCESS;
	}

	dns_name_format(zname, keydirbuf, sizeof(keydirbuf));
	len -= strlen(keydir);
	keydir += strlen(keydir);
	n = snprintf(keydir, len, "/%s", (dirname == NULL) ? "." : dirname);
	if (n > len) {
		cfg_obj_log(zcfg, ISC_LOG_WARNING,
			    "%s '%s' truncated because too long, may cause "
			    "false positives in key directory in use checks",
			    optname, (dirname == NULL) ? "." : dirname);
	}
	keydir = keydirbuf;

	result = isc_symtab_lookup(symtab, keydir, 1, &symvalue);
	if (result == ISC_R_SUCCESS) {
		const cfg_obj_t *kasp = NULL;
		const cfg_obj_t *exist = symvalue.as_cpointer;
		const char *file = cfg_obj_file(exist);
		unsigned int line = cfg_obj_line(exist);

		/*
		 * Having the same key-directory for the same zone is fine
		 * iff the zone is using the same policy, or has no policy.
		 */
		(void)cfg_map_get(cfg_tuple_get(exist, "options"),
				  "dnssec-policy", &kasp);
		if (kasp == NULL ||
		    strcmp(cfg_obj_asstring(kasp), "none") == 0 ||
		    strcmp(cfg_obj_asstring(kasp), kaspnamestr) == 0)
		{
			return ISC_R_SUCCESS;
		}

		cfg_obj_log(zcfg, ISC_LOG_ERROR,
			    "%s '%s' already in use by zone %s with "
			    "policy %s: %s:%u",
			    optname, keydir,
			    cfg_obj_asstring(cfg_tuple_get(exist, "name")),
			    cfg_obj_asstring(kasp), file, line);
		return ISC_R_EXISTS;
	}

	/*
	 * Add the new zone plus key-directory.
	 */
	symkey = isc_mem_strdup(mctx, keydir);
	symvalue.as_cpointer = zcfg;
	result = isc_symtab_define(symtab, symkey, 1, symvalue,
				   isc_symexists_reject);
	RUNTIME_CHECK(result == ISC_R_SUCCESS);
	return result;
}

/*
 * Check key list for duplicates key names and that the key names
 * are valid domain names as these keys are used for TSIG.
 *
 * Check the key contents for validity.
 */
static isc_result_t
check_keylist(const cfg_obj_t *keys, isc_symtab_t *symtab, isc_mem_t *mctx) {
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_fixedname_t fname;
	dns_name_t *name = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;

	name = dns_fixedname_initname(&fname);
	CFG_LIST_FOREACH (keys, element) {
		const cfg_obj_t *key = cfg_listelt_value(element);
		const char *keyid = cfg_obj_asstring(cfg_map_getname(key));
		isc_symvalue_t symvalue;
		isc_buffer_t b;
		char *keyname;

		isc_buffer_constinit(&b, keyid, strlen(keyid));
		isc_buffer_add(&b, strlen(keyid));
		tresult = dns_name_fromtext(name, &b, dns_rootname, 0);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(key, ISC_LOG_ERROR,
				    "key '%s': bad key name", keyid);
			result = tresult;
			continue;
		}
		tresult = isccfg_check_key(key);
		if (tresult != ISC_R_SUCCESS) {
			return tresult;
		}

		dns_name_format(name, namebuf, sizeof(namebuf));
		keyname = isc_mem_strdup(mctx, namebuf);
		symvalue.as_cpointer = key;
		tresult = isc_symtab_define(symtab, keyname, 1, symvalue,
					    isc_symexists_reject);
		if (tresult == ISC_R_EXISTS) {
			const char *file;
			unsigned int line;

			RUNTIME_CHECK(isc_symtab_lookup(symtab, keyname, 1,
							&symvalue) ==
				      ISC_R_SUCCESS);
			file = cfg_obj_file(symvalue.as_cpointer);
			line = cfg_obj_line(symvalue.as_cpointer);

			if (file == NULL) {
				file = "<unknown file>";
			}
			cfg_obj_log(key, ISC_LOG_ERROR,
				    "key '%s': already exists "
				    "previous definition: %s:%u",
				    keyid, file, line);
			isc_mem_free(mctx, keyname);
			result = tresult;
		} else if (tresult != ISC_R_SUCCESS) {
			isc_mem_free(mctx, keyname);
			return tresult;
		}
	}
	return result;
}

/*
 * RNDC keys are not normalised unlike TSIG keys.
 *
 * 	"foo." is different to "foo".
 */
static bool
rndckey_exists(const cfg_obj_t *keylist, const char *keyname) {
	if (keylist == NULL) {
		return false;
	}

	CFG_LIST_FOREACH (keylist, element) {
		const cfg_obj_t *obj = cfg_listelt_value(element);
		const char *str = cfg_obj_asstring(cfg_map_getname(obj));
		if (!strcasecmp(str, keyname)) {
			return true;
		}
	}
	return false;
}

static struct {
	const char *v4;
	const char *v6;
} sources[] = { { "transfer-source", "transfer-source-v6" },
		{ "notify-source", "notify-source-v6" },
		{ "parental-source", "parental-source-v6" },
		{ "query-source", "query-source-v6" },
		{ NULL, NULL } };

static struct {
	const char *name;
	isc_result_t (*set)(dns_peer_t *peer, bool newval);
} bools[] = {
	{ "bogus", dns_peer_setbogus },
	{ "edns", dns_peer_setsupportedns },
	{ "provide-ixfr", dns_peer_setprovideixfr },
	{ "request-expire", dns_peer_setrequestexpire },
	{ "request-ixfr", dns_peer_setrequestixfr },
	{ "request-nsid", dns_peer_setrequestnsid },
	{ "request-zoneversion", dns_peer_setrequestzoneversion },
	{ "send-cookie", dns_peer_setsendcookie },
	{ "tcp-keepalive", dns_peer_settcpkeepalive },
	{ "tcp-only", dns_peer_setforcetcp },
};

static struct {
	const char *name;
	isc_result_t (*set)(dns_peer_t *peer, uint32_t newval);
} uint32s[] = {
	{ "request-ixfr-max-diffs", dns_peer_setrequestixfrmaxdiffs },
};

static isc_result_t
check_servers(const cfg_obj_t *config, const cfg_obj_t *voptions,
	      isc_symtab_t *symtab, isc_mem_t *mctx) {
	dns_fixedname_t fname;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	const cfg_obj_t *servers = NULL;
	const cfg_obj_t *obj = NULL;
	const cfg_obj_t *keys = NULL;
	char buf[ISC_NETADDR_FORMATSIZE];
	char namebuf[DNS_NAME_FORMATSIZE];
	const char *xfr = NULL;
	const char *keyval = NULL;
	isc_buffer_t b;
	int source;
	dns_name_t *keyname = NULL;
	servers = NULL;
	if (voptions != NULL) {
		(void)cfg_map_get(voptions, "server", &servers);
	}
	if (servers == NULL) {
		(void)cfg_map_get(config, "server", &servers);
	}
	if (servers == NULL) {
		return ISC_R_SUCCESS;
	}

	CFG_LIST_FOREACH (servers, e1) {
		const cfg_obj_t *v1 = cfg_listelt_value(e1);
		isc_netaddr_t n1;
		unsigned int p1;
		dns_peer_t *peer = NULL;

		cfg_obj_asnetprefix(cfg_map_getname(v1), &n1, &p1);

		/*
		 * Check that unused bits are zero.
		 */
		tresult = isc_netaddr_prefixok(&n1, p1);
		if (tresult != ISC_R_SUCCESS) {
			INSIST(tresult == ISC_R_FAILURE);
			isc_netaddr_format(&n1, buf, sizeof(buf));
			cfg_obj_log(v1, ISC_LOG_ERROR,
				    "server '%s/%u': invalid prefix "
				    "(extra bits specified)",
				    buf, p1);
			result = tresult;
		}
		source = 0;
		do {
			/*
			 * For a v6 server we can't specify a v4 source,
			 * and vice versa.
			 */
			obj = NULL;
			if (n1.family == AF_INET) {
				xfr = sources[source].v6;
			} else {
				xfr = sources[source].v4;
			}
			(void)cfg_map_get(v1, xfr, &obj);
			if (obj != NULL) {
				isc_netaddr_format(&n1, buf, sizeof(buf));
				cfg_obj_log(v1, ISC_LOG_ERROR,
					    "server '%s/%u': %s not legal", buf,
					    p1, xfr);
				result = ISC_R_FAILURE;
			}

			/*
			 * Check that we aren't using the DNS
			 * listener port (i.e. 53, or whatever was set
			 * as "port" in options) as a source port.
			 */
			obj = NULL;
			if (n1.family == AF_INET) {
				xfr = sources[source].v4;
			} else {
				xfr = sources[source].v6;
			}
			(void)cfg_map_get(v1, xfr, &obj);
			if (obj != NULL) {
				if (cfg_obj_issockaddr(obj)) {
					const isc_sockaddr_t *sa =
						cfg_obj_assockaddr(obj);
					in_port_t port =
						isc_sockaddr_getport(sa);
					if (port == dnsport) {
						cfg_obj_log(obj, ISC_LOG_ERROR,
							    "'%s' cannot "
							    "specify the "
							    "DNS listener port "
							    "(%d)",
							    xfr, port);
						result = ISC_R_FAILURE;
					}
				} else {
					cfg_obj_log(obj, ISC_LOG_ERROR,
						    "'none' is not a legal "
						    "'%s' parameter in a "
						    "server block",
						    xfr);
					result = ISC_R_FAILURE;
				}
			}
		} while (sources[++source].v4 != NULL);

		const cfg_listelt_t *e2 = e1;
		while ((e2 = cfg_list_next(e2)) != NULL) {
			unsigned int p2;
			isc_netaddr_t n2;
			const cfg_obj_t *v2 = cfg_listelt_value(e2);
			cfg_obj_asnetprefix(cfg_map_getname(v2), &n2, &p2);

			if (p1 == p2 && isc_netaddr_equal(&n1, &n2)) {
				const char *file = cfg_obj_file(v1);
				unsigned int line = cfg_obj_line(v1);

				if (file == NULL) {
					file = "<unknown file>";
				}

				isc_netaddr_format(&n2, buf, sizeof(buf));
				cfg_obj_log(v2, ISC_LOG_ERROR,
					    "server '%s/%u': already exists "
					    "previous definition: %s:%u",
					    buf, p2, file, line);
				result = ISC_R_FAILURE;
			}
		}

		keys = NULL;
		cfg_map_get(v1, "keys", &keys);
		if (keys != NULL) {
			/*
			 * Normalize key name.
			 */
			keyval = cfg_obj_asstring(keys);
			isc_buffer_constinit(&b, keyval, strlen(keyval));
			isc_buffer_add(&b, strlen(keyval));
			keyname = dns_fixedname_initname(&fname);
			tresult = dns_name_fromtext(keyname, &b, dns_rootname,
						    0);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(keys, ISC_LOG_ERROR,
					    "bad key name '%s'", keyval);
				result = ISC_R_FAILURE;
				continue;
			}
			dns_name_format(keyname, namebuf, sizeof(namebuf));
			tresult = isc_symtab_lookup(symtab, namebuf, 1, NULL);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(keys, ISC_LOG_ERROR,
					    "unknown key '%s'", keyval);
				result = ISC_R_FAILURE;
			}
		}
		(void)dns_peer_newprefix(mctx, &n1, p1, &peer);
		for (size_t i = 0; i < ARRAY_SIZE(bools); i++) {
			const cfg_obj_t *opt = NULL;
			cfg_map_get(v1, bools[i].name, &opt);
			if (opt != NULL) {
				tresult = (bools[i].set)(
					peer, cfg_obj_asboolean(opt));
				if (tresult != ISC_R_SUCCESS) {
					cfg_obj_log(opt, ISC_LOG_ERROR,
						    "setting server option "
						    "'%s' failed: %s",
						    bools[i].name,
						    isc_result_totext(tresult));
					result = ISC_R_FAILURE;
				}
			}
		}
		for (size_t i = 0; i < ARRAY_SIZE(uint32s); i++) {
			const cfg_obj_t *opt = NULL;
			cfg_map_get(v1, uint32s[i].name, &opt);
			if (opt != NULL) {
				tresult = (uint32s[i].set)(
					peer, cfg_obj_asuint32(opt));
				if (tresult != ISC_R_SUCCESS) {
					cfg_obj_log(opt, ISC_LOG_ERROR,
						    "setting server option "
						    "'%s' failed: %s",
						    uint32s[i].name,
						    isc_result_totext(tresult));
					result = ISC_R_FAILURE;
				}
			}
		}
		dns_peer_detach(&peer);
	}
	return result;
}

#define ROOT_KSK_STATIC	 0x01
#define ROOT_KSK_MANAGED 0x02
#define ROOT_KSK_ANY	 0x03
#define ROOT_KSK_2010	 0x04
#define ROOT_KSK_2017	 0x08

static isc_result_t
check_trust_anchor(const cfg_obj_t *key, unsigned int *flagsp) {
	bool managed = true;
	const char *str = NULL, *namestr = NULL;
	dns_fixedname_t fkeyname;
	dns_name_t *keyname = NULL;
	isc_buffer_t b;
	isc_region_t r;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	uint32_t rdata1, rdata2, rdata3;
	unsigned char data[4096];
	const char *atstr = NULL;
	enum {
		INIT_DNSKEY,
		STATIC_DNSKEY,
		INIT_DS,
		STATIC_DS,
	} anchortype;

	/*
	 * The 2010 and 2017 IANA root keys - these are used below
	 * to check the contents of trusted, initial and
	 * static trust anchor configurations.
	 */
	static const unsigned char root_ksk_2010[] = {
		0x03, 0x01, 0x00, 0x01, 0xa8, 0x00, 0x20, 0xa9, 0x55, 0x66,
		0xba, 0x42, 0xe8, 0x86, 0xbb, 0x80, 0x4c, 0xda, 0x84, 0xe4,
		0x7e, 0xf5, 0x6d, 0xbd, 0x7a, 0xec, 0x61, 0x26, 0x15, 0x55,
		0x2c, 0xec, 0x90, 0x6d, 0x21, 0x16, 0xd0, 0xef, 0x20, 0x70,
		0x28, 0xc5, 0x15, 0x54, 0x14, 0x4d, 0xfe, 0xaf, 0xe7, 0xc7,
		0xcb, 0x8f, 0x00, 0x5d, 0xd1, 0x82, 0x34, 0x13, 0x3a, 0xc0,
		0x71, 0x0a, 0x81, 0x18, 0x2c, 0xe1, 0xfd, 0x14, 0xad, 0x22,
		0x83, 0xbc, 0x83, 0x43, 0x5f, 0x9d, 0xf2, 0xf6, 0x31, 0x32,
		0x51, 0x93, 0x1a, 0x17, 0x6d, 0xf0, 0xda, 0x51, 0xe5, 0x4f,
		0x42, 0xe6, 0x04, 0x86, 0x0d, 0xfb, 0x35, 0x95, 0x80, 0x25,
		0x0f, 0x55, 0x9c, 0xc5, 0x43, 0xc4, 0xff, 0xd5, 0x1c, 0xbe,
		0x3d, 0xe8, 0xcf, 0xd0, 0x67, 0x19, 0x23, 0x7f, 0x9f, 0xc4,
		0x7e, 0xe7, 0x29, 0xda, 0x06, 0x83, 0x5f, 0xa4, 0x52, 0xe8,
		0x25, 0xe9, 0xa1, 0x8e, 0xbc, 0x2e, 0xcb, 0xcf, 0x56, 0x34,
		0x74, 0x65, 0x2c, 0x33, 0xcf, 0x56, 0xa9, 0x03, 0x3b, 0xcd,
		0xf5, 0xd9, 0x73, 0x12, 0x17, 0x97, 0xec, 0x80, 0x89, 0x04,
		0x1b, 0x6e, 0x03, 0xa1, 0xb7, 0x2d, 0x0a, 0x73, 0x5b, 0x98,
		0x4e, 0x03, 0x68, 0x73, 0x09, 0x33, 0x23, 0x24, 0xf2, 0x7c,
		0x2d, 0xba, 0x85, 0xe9, 0xdb, 0x15, 0xe8, 0x3a, 0x01, 0x43,
		0x38, 0x2e, 0x97, 0x4b, 0x06, 0x21, 0xc1, 0x8e, 0x62, 0x5e,
		0xce, 0xc9, 0x07, 0x57, 0x7d, 0x9e, 0x7b, 0xad, 0xe9, 0x52,
		0x41, 0xa8, 0x1e, 0xbb, 0xe8, 0xa9, 0x01, 0xd4, 0xd3, 0x27,
		0x6e, 0x40, 0xb1, 0x14, 0xc0, 0xa2, 0xe6, 0xfc, 0x38, 0xd1,
		0x9c, 0x2e, 0x6a, 0xab, 0x02, 0x64, 0x4b, 0x28, 0x13, 0xf5,
		0x75, 0xfc, 0x21, 0x60, 0x1e, 0x0d, 0xee, 0x49, 0xcd, 0x9e,
		0xe9, 0x6a, 0x43, 0x10, 0x3e, 0x52, 0x4d, 0x62, 0x87, 0x3d
	};
	static const unsigned char root_ksk_2017[] = {
		0x03, 0x01, 0x00, 0x01, 0xac, 0xff, 0xb4, 0x09, 0xbc, 0xc9,
		0x39, 0xf8, 0x31, 0xf7, 0xa1, 0xe5, 0xec, 0x88, 0xf7, 0xa5,
		0x92, 0x55, 0xec, 0x53, 0x04, 0x0b, 0xe4, 0x32, 0x02, 0x73,
		0x90, 0xa4, 0xce, 0x89, 0x6d, 0x6f, 0x90, 0x86, 0xf3, 0xc5,
		0xe1, 0x77, 0xfb, 0xfe, 0x11, 0x81, 0x63, 0xaa, 0xec, 0x7a,
		0xf1, 0x46, 0x2c, 0x47, 0x94, 0x59, 0x44, 0xc4, 0xe2, 0xc0,
		0x26, 0xbe, 0x5e, 0x98, 0xbb, 0xcd, 0xed, 0x25, 0x97, 0x82,
		0x72, 0xe1, 0xe3, 0xe0, 0x79, 0xc5, 0x09, 0x4d, 0x57, 0x3f,
		0x0e, 0x83, 0xc9, 0x2f, 0x02, 0xb3, 0x2d, 0x35, 0x13, 0xb1,
		0x55, 0x0b, 0x82, 0x69, 0x29, 0xc8, 0x0d, 0xd0, 0xf9, 0x2c,
		0xac, 0x96, 0x6d, 0x17, 0x76, 0x9f, 0xd5, 0x86, 0x7b, 0x64,
		0x7c, 0x3f, 0x38, 0x02, 0x9a, 0xbd, 0xc4, 0x81, 0x52, 0xeb,
		0x8f, 0x20, 0x71, 0x59, 0xec, 0xc5, 0xd2, 0x32, 0xc7, 0xc1,
		0x53, 0x7c, 0x79, 0xf4, 0xb7, 0xac, 0x28, 0xff, 0x11, 0x68,
		0x2f, 0x21, 0x68, 0x1b, 0xf6, 0xd6, 0xab, 0xa5, 0x55, 0x03,
		0x2b, 0xf6, 0xf9, 0xf0, 0x36, 0xbe, 0xb2, 0xaa, 0xa5, 0xb3,
		0x77, 0x8d, 0x6e, 0xeb, 0xfb, 0xa6, 0xbf, 0x9e, 0xa1, 0x91,
		0xbe, 0x4a, 0xb0, 0xca, 0xea, 0x75, 0x9e, 0x2f, 0x77, 0x3a,
		0x1f, 0x90, 0x29, 0xc7, 0x3e, 0xcb, 0x8d, 0x57, 0x35, 0xb9,
		0x32, 0x1d, 0xb0, 0x85, 0xf1, 0xb8, 0xe2, 0xd8, 0x03, 0x8f,
		0xe2, 0x94, 0x19, 0x92, 0x54, 0x8c, 0xee, 0x0d, 0x67, 0xdd,
		0x45, 0x47, 0xe1, 0x1d, 0xd6, 0x3a, 0xf9, 0xc9, 0xfc, 0x1c,
		0x54, 0x66, 0xfb, 0x68, 0x4c, 0xf0, 0x09, 0xd7, 0x19, 0x7c,
		0x2c, 0xf7, 0x9e, 0x79, 0x2a, 0xb5, 0x01, 0xe6, 0xa8, 0xa1,
		0xca, 0x51, 0x9a, 0xf2, 0xcb, 0x9b, 0x5f, 0x63, 0x67, 0xe9,
		0x4c, 0x0d, 0x47, 0x50, 0x24, 0x51, 0x35, 0x7b, 0xe1, 0xb5
	};
	static const unsigned char root_ds_1_2017[] = {
		0xae, 0x1e, 0xa5, 0xb9, 0x74, 0xd4, 0xc8, 0x58, 0xb7, 0x40,
		0xbd, 0x03, 0xe3, 0xce, 0xd7, 0xeb, 0xfc, 0xbd, 0x17, 0x24
	};
	static const unsigned char root_ds_2_2017[] = {
		0xe0, 0x6d, 0x44, 0xb8, 0x0b, 0x8f, 0x1d, 0x39,
		0xa9, 0x5c, 0x0b, 0x0d, 0x7c, 0x65, 0xd0, 0x84,
		0x58, 0xe8, 0x80, 0x40, 0x9b, 0xbc, 0x68, 0x34,
		0x57, 0x10, 0x42, 0x37, 0xc7, 0xf8, 0xec, 0x8D
	};

	/* if DNSKEY, flags; if DS, key tag */
	rdata1 = cfg_obj_asuint32(cfg_tuple_get(key, "rdata1"));

	/* if DNSKEY, protocol; if DS, algorithm */
	rdata2 = cfg_obj_asuint32(cfg_tuple_get(key, "rdata2"));

	/* if DNSKEY, algorithm; if DS, digest type */
	rdata3 = cfg_obj_asuint32(cfg_tuple_get(key, "rdata3"));

	namestr = cfg_obj_asstring(cfg_tuple_get(key, "name"));

	keyname = dns_fixedname_initname(&fkeyname);
	isc_buffer_constinit(&b, namestr, strlen(namestr));
	isc_buffer_add(&b, strlen(namestr));
	result = dns_name_fromtext(keyname, &b, dns_rootname, 0);
	if (result != ISC_R_SUCCESS) {
		cfg_obj_log(key, ISC_LOG_WARNING, "bad key name: %s\n",
			    isc_result_totext(result));
		result = ISC_R_FAILURE;
	}

	atstr = cfg_obj_asstring(cfg_tuple_get(key, "anchortype"));
	if (strcasecmp(atstr, "static-key") == 0) {
		managed = false;
		anchortype = STATIC_DNSKEY;
	} else if (strcasecmp(atstr, "static-ds") == 0) {
		managed = false;
		anchortype = STATIC_DS;
	} else if (strcasecmp(atstr, "initial-key") == 0) {
		anchortype = INIT_DNSKEY;
	} else if (strcasecmp(atstr, "initial-ds") == 0) {
		anchortype = INIT_DS;
	} else {
		cfg_obj_log(key, ISC_LOG_ERROR,
			    "key '%s': "
			    "invalid initialization method '%s'",
			    namestr, atstr);
		result = ISC_R_FAILURE;
		/*
		 * We can't interpret the trust anchor, so
		 * we skip all other checks.
		 */
		goto cleanup;
	}

	switch (anchortype) {
	case INIT_DNSKEY:
	case STATIC_DNSKEY:
		if (rdata1 > 0xffff) {
			cfg_obj_log(key, ISC_LOG_ERROR, "flags too big: %u",
				    rdata1);
			result = ISC_R_RANGE;
		}
		if (rdata1 & DNS_KEYFLAG_REVOKE) {
			cfg_obj_log(key, ISC_LOG_WARNING,
				    "key flags revoke bit set");
		}
		if (rdata2 > 0xff) {
			cfg_obj_log(key, ISC_LOG_ERROR, "protocol too big: %u",
				    rdata2);
			result = ISC_R_RANGE;
		}
		if (rdata3 > 0xff) {
			cfg_obj_log(key, ISC_LOG_ERROR,
				    "algorithm too big: %u\n", rdata3);
			result = ISC_R_RANGE;
		}

		isc_buffer_init(&b, data, sizeof(data));

		str = cfg_obj_asstring(cfg_tuple_get(key, "data"));
		tresult = isc_base64_decodestring(str, &b);

		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(key, ISC_LOG_ERROR, "%s",
				    isc_result_totext(tresult));
			result = ISC_R_FAILURE;
		} else {
			isc_buffer_usedregion(&b, &r);

			if ((rdata3 == DST_ALG_RSASHA1) && r.length > 1 &&
			    r.base[0] == 1 && r.base[1] == 3)
			{
				cfg_obj_log(key, ISC_LOG_WARNING,
					    "%s '%s' has a weak exponent",
					    atstr, namestr);
			}
		}

		if (result == ISC_R_SUCCESS &&
		    dns_name_equal(keyname, dns_rootname))
		{
			/*
			 * Flag any use of a root key, regardless of content.
			 */
			*flagsp |= (managed ? ROOT_KSK_MANAGED
					    : ROOT_KSK_STATIC);

			if (rdata1 == 257 && rdata2 == 3 && rdata3 == 8 &&
			    (isc_buffer_usedlength(&b) ==
			     sizeof(root_ksk_2010)) &&
			    memcmp(data, root_ksk_2010,
				   sizeof(root_ksk_2010)) == 0)
			{
				*flagsp |= ROOT_KSK_2010;
			}

			if (rdata1 == 257 && rdata2 == 3 && rdata3 == 8 &&
			    (isc_buffer_usedlength(&b) ==
			     sizeof(root_ksk_2017)) &&
			    memcmp(data, root_ksk_2017,
				   sizeof(root_ksk_2017)) == 0)
			{
				*flagsp |= ROOT_KSK_2017;
			}
		}
		break;

	case INIT_DS:
	case STATIC_DS:
		if (rdata1 > 0xffff) {
			cfg_obj_log(key, ISC_LOG_ERROR, "key tag too big: %u",
				    rdata1);
			result = ISC_R_RANGE;
		}
		if (rdata2 > 0xff) {
			cfg_obj_log(key, ISC_LOG_ERROR,
				    "algorithm too big: %u\n", rdata2);
			result = ISC_R_RANGE;
		}
		if (rdata3 > 0xff) {
			cfg_obj_log(key, ISC_LOG_ERROR,
				    "digest type too big: %u", rdata3);
			result = ISC_R_RANGE;
		}

		isc_buffer_init(&b, data, sizeof(data));

		str = cfg_obj_asstring(cfg_tuple_get(key, "data"));
		tresult = isc_hex_decodestring(str, &b);

		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(key, ISC_LOG_ERROR, "%s",
				    isc_result_totext(tresult));
			result = ISC_R_FAILURE;
		}
		if (result == ISC_R_SUCCESS &&
		    dns_name_equal(keyname, dns_rootname))
		{
			/*
			 * Flag any use of a root key, regardless of content.
			 */
			*flagsp |= (managed ? ROOT_KSK_MANAGED
					    : ROOT_KSK_STATIC);

			if (rdata1 == 20326 && rdata2 == 8 && rdata3 == 1 &&
			    (isc_buffer_usedlength(&b) ==
			     sizeof(root_ds_1_2017)) &&
			    memcmp(data, root_ds_1_2017,
				   sizeof(root_ds_1_2017)) == 0)
			{
				*flagsp |= ROOT_KSK_2017;
			}

			if (rdata1 == 20326 && rdata2 == 8 && rdata3 == 2 &&
			    (isc_buffer_usedlength(&b) ==
			     sizeof(root_ds_2_2017)) &&
			    memcmp(data, root_ds_2_2017,
				   sizeof(root_ds_2_2017)) == 0)
			{
				*flagsp |= ROOT_KSK_2017;
			}
		}
		break;
	}

cleanup:
	return result;
}

static isc_result_t
record_static_keys(isc_symtab_t *symtab, isc_mem_t *mctx,
		   const cfg_obj_t *keylist, bool autovalidation) {
	isc_result_t result, ret = ISC_R_SUCCESS;
	dns_fixedname_t fixed;
	dns_name_t *name = NULL;
	char namebuf[DNS_NAME_FORMATSIZE], *p = NULL;

	name = dns_fixedname_initname(&fixed);

	CFG_LIST_FOREACH (keylist, elt) {
		const char *initmethod;
		const cfg_obj_t *init = NULL;
		const cfg_obj_t *obj = cfg_listelt_value(elt);
		const char *str = cfg_obj_asstring(cfg_tuple_get(obj, "name"));
		isc_symvalue_t symvalue;

		result = dns_name_fromstring(name, str, dns_rootname, 0, NULL);
		if (result != ISC_R_SUCCESS) {
			continue;
		}

		init = cfg_tuple_get(obj, "anchortype");
		if (!cfg_obj_isvoid(init)) {
			initmethod = cfg_obj_asstring(init);
			if (strcasecmp(initmethod, "initial-key") == 0) {
				/* initializing key, skip it */
				continue;
			}
			if (strcasecmp(initmethod, "initial-ds") == 0) {
				/* initializing key, skip it */
				continue;
			}
		}

		dns_name_format(name, namebuf, sizeof(namebuf));
		symvalue.as_cpointer = obj;
		p = isc_mem_strdup(mctx, namebuf);
		result = isc_symtab_define(symtab, p, 1, symvalue,
					   isc_symexists_reject);
		if (result == ISC_R_EXISTS) {
			isc_mem_free(mctx, p);
		} else if (result != ISC_R_SUCCESS) {
			isc_mem_free(mctx, p);
			ret = result;
			continue;
		}

		if (autovalidation && dns_name_equal(name, dns_rootname)) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "static trust anchor for root zone "
				    "cannot be used with "
				    "'dnssec-validation auto'.");
			ret = ISC_R_FAILURE;
			continue;
		}
	}

	return ret;
}

static isc_result_t
check_initializing_keys(isc_symtab_t *symtab, const cfg_obj_t *keylist) {
	isc_result_t result, ret = ISC_R_SUCCESS;
	dns_fixedname_t fixed;
	dns_name_t *name = NULL;
	char namebuf[DNS_NAME_FORMATSIZE];

	name = dns_fixedname_initname(&fixed);

	CFG_LIST_FOREACH (keylist, elt) {
		const cfg_obj_t *obj = cfg_listelt_value(elt);
		const cfg_obj_t *init = NULL;
		const char *str;
		isc_symvalue_t symvalue;

		init = cfg_tuple_get(obj, "anchortype");
		if (cfg_obj_isvoid(init) ||
		    strcasecmp(cfg_obj_asstring(init), "static-key") == 0 ||
		    strcasecmp(cfg_obj_asstring(init), "static-ds") == 0)
		{
			/* static key, skip it */
			continue;
		}

		str = cfg_obj_asstring(cfg_tuple_get(obj, "name"));
		result = dns_name_fromstring(name, str, dns_rootname, 0, NULL);
		if (result != ISC_R_SUCCESS) {
			continue;
		}

		dns_name_format(name, namebuf, sizeof(namebuf));
		result = isc_symtab_lookup(symtab, namebuf, 1, &symvalue);
		if (result == ISC_R_SUCCESS) {
			const char *file = cfg_obj_file(symvalue.as_cpointer);
			unsigned int line = cfg_obj_line(symvalue.as_cpointer);
			if (file == NULL) {
				file = "<unknown file>";
			}
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "static and initializing keys "
				    "cannot be used for the "
				    "same domain. "
				    "static key defined at "
				    "%s:%u",
				    file, line);

			ret = ISC_R_FAILURE;
		}
	}

	return ret;
}

static isc_result_t
record_ds_keys(isc_symtab_t *symtab, isc_mem_t *mctx,
	       const cfg_obj_t *keylist) {
	isc_result_t result, ret = ISC_R_SUCCESS;
	dns_fixedname_t fixed;
	dns_name_t *name = NULL;
	char namebuf[DNS_NAME_FORMATSIZE], *p = NULL;

	name = dns_fixedname_initname(&fixed);

	CFG_LIST_FOREACH (keylist, elt) {
		const char *initmethod;
		const cfg_obj_t *init = NULL;
		const cfg_obj_t *obj = cfg_listelt_value(elt);
		const char *str = cfg_obj_asstring(cfg_tuple_get(obj, "name"));
		isc_symvalue_t symvalue;

		result = dns_name_fromstring(name, str, dns_rootname, 0, NULL);
		if (result != ISC_R_SUCCESS) {
			continue;
		}

		init = cfg_tuple_get(obj, "anchortype");
		if (!cfg_obj_isvoid(init)) {
			initmethod = cfg_obj_asstring(init);
			if (strcasecmp(initmethod, "initial-key") == 0 ||
			    strcasecmp(initmethod, "static-key") == 0)
			{
				/* Key-style key, skip it */
				continue;
			}
		}

		dns_name_format(name, namebuf, sizeof(namebuf));
		symvalue.as_cpointer = obj;
		p = isc_mem_strdup(mctx, namebuf);
		result = isc_symtab_define(symtab, p, 1, symvalue,
					   isc_symexists_reject);
		if (result == ISC_R_EXISTS) {
			isc_mem_free(mctx, p);
		}
	}

	return ret;
}

/*
 * Check for conflicts between static and initialiizing keys.
 */
static isc_result_t
check_ta_conflicts(const cfg_obj_t *global_ta, const cfg_obj_t *view_ta,
		   bool autovalidation, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	const cfg_obj_t *keylist = NULL;
	isc_symtab_t *statictab = NULL, *dstab = NULL;

	isc_symtab_create(mctx, freekey, mctx, false, &statictab);
	isc_symtab_create(mctx, freekey, mctx, false, &dstab);

	/*
	 * First we record all the static keys (trust-anchors configured with
	 * "static-key"), and all the DS-style trust anchors.
	 */
	CFG_LIST_FOREACH (global_ta, elt) {
		keylist = cfg_listelt_value(elt);
		tresult = record_static_keys(statictab, mctx, keylist,
					     autovalidation);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}

		tresult = record_ds_keys(dstab, mctx, keylist);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	CFG_LIST_FOREACH (view_ta, elt) {
		keylist = cfg_listelt_value(elt);
		tresult = record_static_keys(statictab, mctx, keylist,
					     autovalidation);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}

		tresult = record_ds_keys(dstab, mctx, keylist);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	/*
	 * Next, ensure that there's no conflict between the
	 * static keys and the trust-anchors configured with "initial-key".
	 */
	CFG_LIST_FOREACH (global_ta, elt) {
		keylist = cfg_listelt_value(elt);
		tresult = check_initializing_keys(statictab, keylist);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	CFG_LIST_FOREACH (view_ta, elt) {
		keylist = cfg_listelt_value(elt);
		tresult = check_initializing_keys(statictab, keylist);
		if (result == ISC_R_SUCCESS) {
			result = tresult;
		}
	}

	if (statictab != NULL) {
		isc_symtab_destroy(&statictab);
	}
	if (dstab != NULL) {
		isc_symtab_destroy(&dstab);
	}
	return result;
}

typedef enum { special_zonetype_rpz, special_zonetype_catz } special_zonetype_t;

static isc_result_t
check_rpz_catz(const char *rpz_catz, const cfg_obj_t *rpz_obj,
	       const char *viewname, isc_symtab_t *symtab,
	       special_zonetype_t specialzonetype) {
	const cfg_obj_t *obj = NULL, *nameobj = NULL, *zoneobj = NULL;
	const char *zonename = NULL, *zonetype = NULL;
	const char *forview = " for view ";
	isc_symvalue_t value;
	isc_result_t result = ISC_R_SUCCESS, tresult;
	dns_fixedname_t fixed;
	dns_name_t *name = NULL;
	char namebuf[DNS_NAME_FORMATSIZE];
	unsigned int num_zones = 0;

	if (viewname == NULL) {
		viewname = "";
		forview = "";
	}
	result = ISC_R_SUCCESS;

	name = dns_fixedname_initname(&fixed);
	obj = cfg_tuple_get(rpz_obj, "zone list");

	CFG_LIST_FOREACH (obj, element) {
		obj = cfg_listelt_value(element);
		nameobj = cfg_tuple_get(obj, "zone name");
		zonename = cfg_obj_asstring(nameobj);
		zonetype = "";

		if (specialzonetype == special_zonetype_rpz) {
			if (++num_zones > 64) {
				cfg_obj_log(nameobj, ISC_LOG_ERROR,
					    "more than 64 response policy "
					    "zones in view '%s'",
					    viewname);
				return ISC_R_FAILURE;
			}
		}

		tresult = dns_name_fromstring(name, zonename, dns_rootname, 0,
					      NULL);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(nameobj, ISC_LOG_ERROR,
				    "bad domain name '%s'", zonename);
			if (result == ISC_R_SUCCESS) {
				result = tresult;
			}
			continue;
		}
		dns_name_format(name, namebuf, sizeof(namebuf));
		tresult = isc_symtab_lookup(symtab, namebuf, 3, &value);
		if (tresult == ISC_R_SUCCESS) {
			obj = NULL;
			zoneobj = value.as_cpointer;
			if (zoneobj != NULL && cfg_obj_istuple(zoneobj)) {
				zoneobj = cfg_tuple_get(zoneobj, "options");
			}
			if (zoneobj != NULL && cfg_obj_ismap(zoneobj)) {
				(void)cfg_map_get(zoneobj, "type", &obj);
			}
			if (obj != NULL) {
				zonetype = cfg_obj_asstring(obj);
			}
		}
		if (strcasecmp(zonetype, "primary") != 0 &&
		    strcasecmp(zonetype, "master") != 0 &&
		    strcasecmp(zonetype, "secondary") != 0 &&
		    strcasecmp(zonetype, "slave") != 0)
		{
			cfg_obj_log(nameobj, ISC_LOG_ERROR,
				    "%s '%s'%s%s is not a primary or secondary "
				    "zone",
				    rpz_catz, zonename, forview, viewname);
			if (result == ISC_R_SUCCESS) {
				result = ISC_R_FAILURE;
			}
		}
	}
	return result;
}

static isc_result_t
check_rpz(const cfg_obj_t *rpz_obj) {
	const cfg_obj_t *obj = NULL, *nameobj = NULL, *edeobj = NULL;
	const char *zonename = NULL;
	isc_result_t result = ISC_R_SUCCESS, tresult;
	dns_fixedname_t fixed;
	dns_name_t *name = dns_fixedname_initname(&fixed);

	obj = cfg_tuple_get(rpz_obj, "zone list");

	CFG_LIST_FOREACH (obj, element) {
		obj = cfg_listelt_value(element);
		nameobj = cfg_tuple_get(obj, "zone name");
		zonename = cfg_obj_asstring(nameobj);

		tresult = dns_name_fromstring(name, zonename, dns_rootname, 0,
					      NULL);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR, "bad domain name '%s'",
				    zonename);
			if (result == ISC_R_SUCCESS) {
				result = tresult;
				continue;
			}
		}

		edeobj = cfg_tuple_get(obj, "ede");
		if (edeobj != NULL && cfg_obj_isstring(edeobj)) {
			const char *str = cfg_obj_asstring(edeobj);

			if (dns_rpz_str2ede(str) == UINT16_MAX) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "unsupported EDE type '%s'", str);
				result = ISC_R_FAILURE;
			}
		}
	}

	return result;
}

static isc_result_t
check_catz(const cfg_obj_t *catz_obj, const char *viewname, isc_mem_t *mctx) {
	const cfg_obj_t *obj = NULL, *nameobj = NULL, *primariesobj = NULL;
	const char *zonename = NULL;
	const char *forview = " for view ";
	isc_result_t result = ISC_R_SUCCESS, tresult;
	isc_symtab_t *symtab = NULL;
	dns_fixedname_t fixed;
	dns_name_t *name = dns_fixedname_initname(&fixed);

	if (viewname == NULL) {
		viewname = "";
		forview = "";
	}

	isc_symtab_create(mctx, freekey, mctx, false, &symtab);

	obj = cfg_tuple_get(catz_obj, "zone list");

	CFG_LIST_FOREACH (obj, element) {
		char namebuf[DNS_NAME_FORMATSIZE];

		obj = cfg_listelt_value(element);
		nameobj = cfg_tuple_get(obj, "zone name");
		zonename = cfg_obj_asstring(nameobj);

		tresult = dns_name_fromstring(name, zonename, dns_rootname, 0,
					      NULL);
		if (tresult != ISC_R_SUCCESS) {
			cfg_obj_log(obj, ISC_LOG_ERROR, "bad domain name '%s'",
				    zonename);
			if (result == ISC_R_SUCCESS) {
				result = tresult;
				continue;
			}
		}

		dns_name_format(name, namebuf, sizeof(namebuf));
		tresult = exists(nameobj, namebuf, 1, symtab,
				 "catalog zone '%s': already added here %s:%u",
				 mctx);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
			continue;
		}

		primariesobj = cfg_tuple_get(obj, "default-primaries");
		if (primariesobj != NULL && cfg_obj_istuple(primariesobj)) {
			primariesobj = cfg_tuple_get(obj, "default-masters");
			if (primariesobj != NULL &&
			    cfg_obj_istuple(primariesobj))
			{
				cfg_obj_log(nameobj, ISC_LOG_ERROR,
					    "catalog zone '%s'%s%s: "
					    "'default-primaries' and "
					    "'default-masters' can not be both "
					    "defined",
					    zonename, forview, viewname);
				result = ISC_R_FAILURE;
				break;
			}
		}
	}

	if (symtab != NULL) {
		isc_symtab_destroy(&symtab);
	}

	return result;
}

/*%
 * Data structure used for the 'callback_data' argument to check_one_plugin().
 */
struct check_one_plugin_data {
	isc_mem_t *mctx;
	cfg_aclconfctx_t *actx;
	isc_result_t *check_result;
};

/*%
 * A callback for the cfg_pluginlist_foreach() call in check_viewconf() below.
 * Since the point is to check configuration of all plugins even when
 * processing some of them fails, always return ISC_R_SUCCESS and indicate any
 * check failures through the 'check_result' variable passed in via the
 * 'callback_data' structure.
 */
static isc_result_t
check_one_plugin(const cfg_obj_t *config, const cfg_obj_t *obj,
		 const char *plugin_path, const char *parameters,
		 void *callback_data) {
	struct check_one_plugin_data *data = callback_data;
	char full_path[PATH_MAX];
	isc_result_t result = ISC_R_SUCCESS;

	result = ns_plugin_expandpath(plugin_path, full_path,
				      sizeof(full_path));
	if (result != ISC_R_SUCCESS) {
		cfg_obj_log(obj, ISC_LOG_ERROR,
			    "%s: plugin check failed: "
			    "unable to get full plugin path: %s",
			    plugin_path, isc_result_totext(result));
		return result;
	}

	result = ns_plugin_check(full_path, parameters, config,
				 cfg_obj_file(obj), cfg_obj_line(obj),
				 data->mctx, data->actx);
	if (result != ISC_R_SUCCESS) {
		cfg_obj_log(obj, ISC_LOG_ERROR, "%s: plugin check failed: %s",
			    full_path, isc_result_totext(result));
		*data->check_result = result;
	}

	return ISC_R_SUCCESS;
}

static isc_result_t
check_dnstap(const cfg_obj_t *voptions, const cfg_obj_t *config) {
#ifdef HAVE_DNSTAP
	const cfg_obj_t *options = NULL;
	const cfg_obj_t *obj = NULL;

	if (config != NULL) {
		(void)cfg_map_get(config, "options", &options);
	}
	if (options != NULL) {
		(void)cfg_map_get(options, "dnstap-output", &obj);
	}
	if (obj == NULL) {
		if (voptions != NULL) {
			(void)cfg_map_get(voptions, "dnstap", &obj);
		}
		if (options != NULL && obj == NULL) {
			(void)cfg_map_get(options, "dnstap", &obj);
		}
		if (obj != NULL) {
			cfg_obj_log(obj, ISC_LOG_ERROR,
				    "'dnstap-output' must be set if 'dnstap' "
				    "is set");
			return ISC_R_FAILURE;
		}
	}
	return ISC_R_SUCCESS;
#else  /* ifdef HAVE_DNSTAP */
	UNUSED(voptions);
	UNUSED(config);

	return ISC_R_SUCCESS;
#endif /* ifdef HAVE_DNSTAP */
}

static isc_result_t
check_viewconf(const cfg_obj_t *config, const cfg_obj_t *voptions,
	       const char *viewname, dns_rdataclass_t vclass,
	       isc_symtab_t *files, isc_symtab_t *keydirs, unsigned int flags,
	       isc_symtab_t *inview, isc_mem_t *mctx) {
	const cfg_obj_t *zones = NULL;
	const cfg_obj_t *view_ta = NULL, *global_ta = NULL;
	const cfg_obj_t *check_keys[2] = { NULL, NULL };
	const cfg_obj_t *keys = NULL;
	isc_symtab_t *symtab = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult = ISC_R_SUCCESS;
	cfg_aclconfctx_t *actx = NULL;
	const cfg_obj_t *obj = NULL;
	const cfg_obj_t *options = NULL;
	const cfg_obj_t *opts = NULL;
	const cfg_obj_t *plugin_list = NULL;
	bool autovalidation = false;
	unsigned int dflags = 0;
	int i;
	bool check_plugins = (flags & BIND_CHECK_PLUGINS) != 0;
	bool check_algorithms = (flags & BIND_CHECK_ALGORITHMS) != 0;

	/*
	 * Get global options block
	 */
	(void)cfg_map_get(config, "options", &options);

	/*
	 * The most relevant options for this view
	 */
	if (voptions != NULL) {
		opts = voptions;
	} else {
		opts = options;
	}

	/*
	 * Check that all zone statements are syntactically correct and
	 * there are no duplicate zones.
	 */
	isc_symtab_create(mctx, freekey, mctx, false, &symtab);

	cfg_aclconfctx_create(mctx, &actx);

	if (voptions != NULL) {
		(void)cfg_map_get(voptions, "zone", &zones);
	} else {
		(void)cfg_map_get(config, "zone", &zones);
	}

	CFG_LIST_FOREACH (zones, element) {
		const cfg_obj_t *zone = cfg_listelt_value(element);

		tresult = isccfg_check_zoneconf(zone, voptions, config, symtab,
						files, keydirs, inview,
						viewname, vclass, actx, mctx);
		if (tresult != ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	}

	/*
	 * Check that the response-policy and catalog-zones options
	 * refer to zones that exist.
	 */
	if (opts != NULL) {
		obj = NULL;
		if ((cfg_map_get(opts, "response-policy", &obj) ==
		     ISC_R_SUCCESS) &&
		    (check_rpz_catz("response-policy zone", obj, viewname,
				    symtab,
				    special_zonetype_rpz) != ISC_R_SUCCESS))
		{
			result = ISC_R_FAILURE;
		}

		obj = NULL;
		if ((cfg_map_get(opts, "catalog-zones", &obj) ==
		     ISC_R_SUCCESS) &&
		    (check_rpz_catz("catalog zone", obj, viewname, symtab,

				    special_zonetype_catz) != ISC_R_SUCCESS))
		{
			result = ISC_R_FAILURE;
		}
	}

	/*
	 * Check response-policy configuration.
	 */
	if (opts != NULL) {
		obj = NULL;
		if ((cfg_map_get(opts, "response-policy", &obj) ==
		     ISC_R_SUCCESS) &&
		    (check_rpz(obj) != ISC_R_SUCCESS))
		{
			result = ISC_R_FAILURE;
		}
	}

	/*
	 * Check catalog-zones configuration.
	 */
	if (opts != NULL) {
		obj = NULL;
		if ((cfg_map_get(opts, "catalog-zones", &obj) ==
		     ISC_R_SUCCESS) &&
		    (check_catz(obj, viewname, mctx) != ISC_R_SUCCESS))
		{
			result = ISC_R_FAILURE;
		}
	}

	isc_symtab_destroy(&symtab);

	/*
	 * Check that forwarding is reasonable.
	 */
	if (opts != NULL && check_forward(config, opts, NULL) != ISC_R_SUCCESS)
	{
		result = ISC_R_FAILURE;
	}

	/*
	 * Check non-zero options at the global and view levels.
	 */
	if (options != NULL && check_nonzero(options) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}
	if (voptions != NULL && check_nonzero(voptions) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	/*
	 * Check that dual-stack-servers is reasonable.
	 */
	if (opts != NULL && check_dual_stack(opts) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	/*
	 * Check that rrset-order is reasonable.
	 */
	if (opts != NULL && check_order(opts) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	/*
	 * Check that all key statements are syntactically correct and
	 * there are no duplicate keys.
	 */
	isc_symtab_create(mctx, freekey, mctx, false, &symtab);

	(void)cfg_map_get(config, "key", &keys);
	tresult = check_keylist(keys, symtab, mctx);
	if (tresult == ISC_R_EXISTS) {
		result = ISC_R_FAILURE;
	} else if (tresult != ISC_R_SUCCESS) {
		result = tresult;
		goto cleanup;
	}

	if (voptions != NULL) {
		keys = NULL;
		(void)cfg_map_get(voptions, "key", &keys);
		tresult = check_keylist(keys, symtab, mctx);
		if (tresult == ISC_R_EXISTS) {
			result = ISC_R_FAILURE;
		} else if (tresult != ISC_R_SUCCESS) {
			result = tresult;
			goto cleanup;
		}
	}

	/*
	 * Global servers can refer to keys in views.
	 */
	if (check_servers(config, voptions, symtab, mctx) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	isc_symtab_destroy(&symtab);

	/*
	 * Load all DNSSEC keys.
	 */
	if (voptions != NULL) {
		(void)cfg_map_get(voptions, "trust-anchors", &view_ta);
	}
	(void)cfg_map_get(config, "trust-anchors", &global_ta);

	check_keys[0] = view_ta;
	check_keys[1] = global_ta;
	for (i = 0; i < 2; i++) {
		if (check_keys[i] != NULL) {
			unsigned int taflags = 0;

			CFG_LIST_FOREACH (check_keys[i], element) {
				const cfg_obj_t *keylist =
					cfg_listelt_value(element);
				CFG_LIST_FOREACH (keylist, element2) {
					obj = cfg_listelt_value(element2);
					tresult = check_trust_anchor(obj,
								     &taflags);
					if (tresult != ISC_R_SUCCESS) {
						result = tresult;
					}
				}
			}

			if ((taflags & ROOT_KSK_STATIC) != 0) {
				cfg_obj_log(check_keys[i], ISC_LOG_WARNING,
					    "static entry for the root "
					    "zone WILL FAIL after key "
					    "rollover - use trust-anchors "
					    "with initial-key "
					    "or initial-ds instead.");
			}

			if ((taflags & ROOT_KSK_2010) != 0 &&
			    (taflags & ROOT_KSK_2017) == 0)
			{
				cfg_obj_log(check_keys[i], ISC_LOG_WARNING,
					    "initial-key entry for the root "
					    "zone uses the 2010 key without "
					    "the updated 2017 key");
			}

			dflags |= taflags;
		}
	}

	if ((dflags & ROOT_KSK_ANY) == ROOT_KSK_ANY) {
		keys = (view_ta != NULL) ? view_ta : global_ta;
		cfg_obj_log(keys, ISC_LOG_WARNING,
			    "both initial and static entries for the "
			    "root zone are present");
	}

	obj = NULL;
	if (voptions != NULL) {
		(void)cfg_map_get(voptions, "dnssec-validation", &obj);
	}
	if (obj == NULL && options != NULL) {
		(void)cfg_map_get(options, "dnssec-validation", &obj);
	}
	if (obj != NULL) {
		if (!cfg_obj_isboolean(obj)) {
			autovalidation = true;
		} else if (cfg_obj_asboolean(obj)) {
			if (global_ta == NULL && view_ta == NULL) {
				cfg_obj_log(obj, ISC_LOG_ERROR,
					    "the 'dnssec-validation yes' "
					    "option requires configured "
					    "'trust-anchors'; consider using "
					    "'dnssec-validation auto'.");
				result = ISC_R_FAILURE;
			}
		}
	}

	tresult = check_ta_conflicts(global_ta, view_ta, autovalidation, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	/*
	 * Check options.
	 */
	if (voptions != NULL) {
		tresult = check_options(voptions, NULL, check_algorithms, mctx,
					optlevel_view);
	} else {
		tresult = check_options(config, config, check_algorithms, mctx,
					optlevel_config);
	}
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	tresult = check_dnstap(voptions, config);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	tresult = check_viewacls(actx, voptions, config, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	tresult = check_recursionacls(actx, voptions, viewname, config, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	tresult = check_dns64(actx, voptions, config, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	tresult = check_ratelimit(actx, voptions, config, mctx);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	tresult = check_fetchlimit(voptions, config);
	if (tresult != ISC_R_SUCCESS) {
		result = tresult;
	}

	/*
	 * Load plugins.
	 */
	if (check_plugins) {
		if (voptions != NULL) {
			(void)cfg_map_get(voptions, "plugin", &plugin_list);
		} else {
			(void)cfg_map_get(config, "plugin", &plugin_list);
		}
	}

	{
		struct check_one_plugin_data check_one_plugin_data = {
			.mctx = mctx,
			.actx = actx,
			.check_result = &tresult,
		};

		(void)cfg_pluginlist_foreach(config, plugin_list,
					     check_one_plugin,
					     &check_one_plugin_data);
		if (tresult != ISC_R_SUCCESS) {
			result = tresult;
		}
	}

cleanup:
	if (symtab != NULL) {
		isc_symtab_destroy(&symtab);
	}
	if (actx != NULL) {
		cfg_aclconfctx_detach(&actx);
	}

	return result;
}

static const char *default_channels[] = { "default_syslog", "default_stderr",
					  "default_debug", "null", NULL };

static isc_result_t
check_logging(const cfg_obj_t *config, isc_mem_t *mctx) {
	const cfg_obj_t *categories = NULL;
	const cfg_obj_t *category = NULL;
	const cfg_obj_t *channels = NULL;
	const cfg_obj_t *channel = NULL;
	const char *channelname = NULL;
	const char *catname = NULL;
	const cfg_obj_t *fileobj = NULL;
	const cfg_obj_t *syslogobj = NULL;
	const cfg_obj_t *nullobj = NULL;
	const cfg_obj_t *stderrobj = NULL;
	const cfg_obj_t *logobj = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult;
	isc_symtab_t *symtab = NULL;
	isc_symvalue_t symvalue;
	int i;

	(void)cfg_map_get(config, "logging", &logobj);
	if (logobj == NULL) {
		return ISC_R_SUCCESS;
	}

	isc_symtab_create(mctx, NULL, NULL, false, &symtab);

	symvalue.as_cpointer = NULL;
	for (i = 0; default_channels[i] != NULL; i++) {
		tresult = isc_symtab_define(symtab, default_channels[i], 1,
					    symvalue, isc_symexists_replace);
		RUNTIME_CHECK(tresult == ISC_R_SUCCESS);
	}

	cfg_map_get(logobj, "channel", &channels);

	CFG_LIST_FOREACH (channels, element) {
		channel = cfg_listelt_value(element);
		channelname = cfg_obj_asstring(cfg_map_getname(channel));
		fileobj = syslogobj = nullobj = stderrobj = NULL;
		(void)cfg_map_get(channel, "file", &fileobj);
		(void)cfg_map_get(channel, "syslog", &syslogobj);
		(void)cfg_map_get(channel, "null", &nullobj);
		(void)cfg_map_get(channel, "stderr", &stderrobj);
		i = 0;
		if (fileobj != NULL) {
			i++;
		}
		if (syslogobj != NULL) {
			i++;
		}
		if (nullobj != NULL) {
			i++;
		}
		if (stderrobj != NULL) {
			i++;
		}
		if (i != 1) {
			cfg_obj_log(channel, ISC_LOG_ERROR,
				    "channel '%s': exactly one of file, "
				    "syslog, "
				    "null, and stderr must be present",
				    channelname);
			result = ISC_R_FAILURE;
		}
		tresult = isc_symtab_define(symtab, channelname, 1, symvalue,
					    isc_symexists_replace);
		RUNTIME_CHECK(tresult == ISC_R_SUCCESS);
	}

	cfg_map_get(logobj, "category", &categories);

	CFG_LIST_FOREACH (categories, element) {
		category = cfg_listelt_value(element);
		catname = cfg_obj_asstring(cfg_tuple_get(category, "name"));
		if (isc_log_categorybyname(catname) == ISC_LOGCATEGORY_INVALID)
		{
			cfg_obj_log(category, ISC_LOG_ERROR,
				    "undefined category: '%s'", catname);
			result = ISC_R_FAILURE;
		}
		channels = cfg_tuple_get(category, "destinations");
		CFG_LIST_FOREACH (channels, delement) {
			channel = cfg_listelt_value(delement);
			channelname = cfg_obj_asstring(channel);
			tresult = isc_symtab_lookup(symtab, channelname, 1,
						    &symvalue);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(channel, ISC_LOG_ERROR,
					    "undefined channel: '%s'",
					    channelname);
				result = tresult;
			}
		}
	}
	isc_symtab_destroy(&symtab);
	return result;
}

static isc_result_t
check_controlskeys(const cfg_obj_t *control, const cfg_obj_t *keylist) {
	isc_result_t result = ISC_R_SUCCESS;
	const cfg_obj_t *control_keylist;
	const cfg_obj_t *key = NULL;
	const char *keyval = NULL;

	control_keylist = cfg_tuple_get(control, "keys");
	if (cfg_obj_isvoid(control_keylist)) {
		return ISC_R_SUCCESS;
	}

	CFG_LIST_FOREACH (control_keylist, element) {
		key = cfg_listelt_value(element);
		keyval = cfg_obj_asstring(key);

		if (!rndckey_exists(keylist, keyval)) {
			cfg_obj_log(key, ISC_LOG_ERROR, "unknown key '%s'",
				    keyval);
			result = ISC_R_NOTFOUND;
		}
	}
	return result;
}

static isc_result_t
check_controls(const cfg_obj_t *config, isc_mem_t *mctx) {
	isc_result_t result = ISC_R_SUCCESS, tresult;
	cfg_aclconfctx_t *actx = NULL;
	const cfg_obj_t *allow = NULL;
	const cfg_obj_t *control = NULL;
	const cfg_obj_t *controls = NULL;
	const cfg_obj_t *controlslist = NULL;
	const cfg_obj_t *inetcontrols = NULL;
	const cfg_obj_t *unixcontrols = NULL;
	const cfg_obj_t *keylist = NULL;
	const cfg_obj_t *obj = NULL;
	const char *path = NULL;
	dns_acl_t *acl = NULL;
	isc_symtab_t *symtab = NULL;

	(void)cfg_map_get(config, "controls", &controlslist);
	if (controlslist == NULL) {
		return ISC_R_SUCCESS;
	}

	(void)cfg_map_get(config, "key", &keylist);

	cfg_aclconfctx_create(mctx, &actx);

	isc_symtab_create(mctx, freekey, mctx, true, &symtab);

	/*
	 * INET: Check allow clause.
	 * UNIX: Not supported.
	 */
	CFG_LIST_FOREACH (controlslist, element) {
		controls = cfg_listelt_value(element);
		unixcontrols = NULL;
		inetcontrols = NULL;
		(void)cfg_map_get(controls, "unix", &unixcontrols);
		(void)cfg_map_get(controls, "inet", &inetcontrols);
		CFG_LIST_FOREACH (inetcontrols, element2) {
			char socktext[ISC_SOCKADDR_FORMATSIZE];
			isc_sockaddr_t addr;

			control = cfg_listelt_value(element2);
			allow = cfg_tuple_get(control, "allow");
			tresult = cfg_acl_fromconfig(allow, config, actx, mctx,
						     0, &acl);
			if (acl != NULL) {
				dns_acl_detach(&acl);
			}
			if (tresult != ISC_R_SUCCESS) {
				result = tresult;
			}
			tresult = check_controlskeys(control, keylist);
			if (tresult != ISC_R_SUCCESS) {
				result = tresult;
			}
			obj = cfg_tuple_get(control, "address");
			addr = *cfg_obj_assockaddr(obj);
			if (isc_sockaddr_getport(&addr) == 0) {
				isc_sockaddr_setport(&addr, NAMED_CONTROL_PORT);
			}
			isc_sockaddr_format(&addr, socktext, sizeof(socktext));
			tresult = exists(
				obj, socktext, 1, symtab,
				"inet control socket '%s': already defined, "
				"previous definition: %s:%u",
				mctx);
			if (tresult != ISC_R_SUCCESS) {
				result = tresult;
			}
		}
		CFG_LIST_FOREACH (unixcontrols, element2) {
			control = cfg_listelt_value(element2);
			path = cfg_obj_asstring(cfg_tuple_get(control, "path"));
			cfg_obj_log(control, ISC_LOG_ERROR,
				    "unix control '%s': not supported", path);
			result = ISC_R_FAMILYNOSUPPORT;
		}
	}

	cfg_aclconfctx_detach(&actx);
	if (symtab != NULL) {
		isc_symtab_destroy(&symtab);
	}
	return result;
}

isc_result_t
isccfg_check_namedconf(const cfg_obj_t *config, unsigned int flags,
		       isc_mem_t *mctx) {
	const cfg_obj_t *options = NULL;
	const cfg_obj_t *views = NULL;
	const cfg_obj_t *acls = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t tresult = ISC_R_SUCCESS;
	isc_symtab_t *symtab = NULL;
	isc_symtab_t *files = NULL;
	isc_symtab_t *keydirs = NULL;
	isc_symtab_t *inview = NULL;
	bool check_algorithms = (flags & BIND_CHECK_ALGORITHMS) != 0;

	static const char *builtin[] = { "localhost", "localnets", "any",
					 "none" };

	(void)cfg_map_get(config, "options", &options);

	if (options != NULL &&
	    check_options(options, config, check_algorithms, mctx,
			  optlevel_options) != ISC_R_SUCCESS)
	{
		result = ISC_R_FAILURE;
	}

	if (check_logging(config, mctx) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	if (check_controls(config, mctx) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	if (check_remoteserverlists(config, mctx) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

#if HAVE_LIBNGHTTP2
	if (check_httpservers(config, mctx) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}
#endif /* HAVE_LIBNGHTTP2 */

	if (check_tls_definitions(config, mctx) != ISC_R_SUCCESS) {
		result = ISC_R_FAILURE;
	}

	(void)cfg_map_get(config, "view", &views);

	if (views != NULL && options != NULL) {
		if (check_dual_stack(options) != ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;

			/*
			 * Use case insensitive comparison as not all file
			 * systems are case sensitive. This will prevent people
			 * using FOO.DB and foo.db on case sensitive file
			 * systems but that shouldn't be a major issue.
			 */
		}
	}

	/*
	 * Use case insensitive comparison as not all file systems are
	 * case sensitive. This will prevent people using FOO.DB and foo.db
	 * on case sensitive file systems but that shouldn't be a major issue.
	 */
	isc_symtab_create(mctx, NULL, NULL, false, &files);
	isc_symtab_create(mctx, freekey, mctx, false, &keydirs);
	isc_symtab_create(mctx, freekey, mctx, true, &inview);

	if (views == NULL) {
		tresult = check_viewconf(config, NULL, NULL, dns_rdataclass_in,
					 files, keydirs, flags, inview, mctx);
		if (result == ISC_R_SUCCESS && tresult != ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	} else {
		const cfg_obj_t *zones = NULL;
		const cfg_obj_t *plugins = NULL;

		(void)cfg_map_get(config, "zone", &zones);
		if (zones != NULL) {
			cfg_obj_log(zones, ISC_LOG_ERROR,
				    "when using 'view' statements, "
				    "all zones must be in views");
			result = ISC_R_FAILURE;
		}

		(void)cfg_map_get(config, "plugin", &plugins);
		if (plugins != NULL) {
			cfg_obj_log(plugins, ISC_LOG_ERROR,
				    "when using 'view' statements, "
				    "all plugins must be defined in views");
			result = ISC_R_FAILURE;
		}
	}

	isc_symtab_create(mctx, NULL, NULL, true, &symtab);

	CFG_LIST_FOREACH (views, velement) {
		const cfg_obj_t *view = cfg_listelt_value(velement);
		const cfg_obj_t *vname = cfg_tuple_get(view, "name");
		const cfg_obj_t *voptions = cfg_tuple_get(view, "options");
		const cfg_obj_t *vclassobj = cfg_tuple_get(view, "class");
		dns_rdataclass_t vclass = dns_rdataclass_in;
		const char *key = cfg_obj_asstring(vname);
		isc_symvalue_t symvalue;
		unsigned int symtype;

		tresult = ISC_R_SUCCESS;
		if (cfg_obj_isstring(vclassobj)) {
			isc_textregion_t r;

			r.base = UNCONST(cfg_obj_asstring(vclassobj));
			r.length = strlen(r.base);
			tresult = dns_rdataclass_fromtext(&vclass, &r);
			if (tresult != ISC_R_SUCCESS) {
				cfg_obj_log(vclassobj, ISC_LOG_ERROR,
					    "view '%s': invalid class %s",
					    cfg_obj_asstring(vname), r.base);
			}
		}
		symtype = vclass + 1;
		if (tresult == ISC_R_SUCCESS && symtab != NULL) {
			symvalue.as_cpointer = view;
			tresult = isc_symtab_define(symtab, key, symtype,
						    symvalue,
						    isc_symexists_reject);
			if (tresult == ISC_R_EXISTS) {
				const char *file;
				unsigned int line;
				RUNTIME_CHECK(isc_symtab_lookup(symtab, key,
								symtype,
								&symvalue) ==
					      ISC_R_SUCCESS);
				file = cfg_obj_file(symvalue.as_cpointer);
				line = cfg_obj_line(symvalue.as_cpointer);
				cfg_obj_log(view, ISC_LOG_ERROR,
					    "view '%s': already exists "
					    "previous definition: %s:%u",
					    key, file, line);
				result = tresult;
			} else if ((strcasecmp(key, "_bind") == 0 &&
				    vclass == dns_rdataclass_ch) ||
				   (strcasecmp(key, "_default") == 0 &&
				    vclass == dns_rdataclass_in))
			{
				cfg_obj_log(view, ISC_LOG_ERROR,
					    "attempt to redefine builtin view "
					    "'%s'",
					    key);
				result = ISC_R_EXISTS;
			}
		}
		if (tresult == ISC_R_SUCCESS) {
			tresult = check_viewconf(config, voptions, key, vclass,
						 files, keydirs, flags, inview,
						 mctx);
		}
		if (tresult != ISC_R_SUCCESS) {
			result = ISC_R_FAILURE;
		}
	}

	cfg_map_get(config, "acl", &acls);

	if (acls != NULL) {
		const char *aclname = NULL;

		CFG_LIST_FOREACH (acls, elt) {
			const cfg_obj_t *acl = cfg_listelt_value(elt);
			unsigned int line = cfg_obj_line(acl);
			unsigned int i;

			aclname = cfg_obj_asstring(cfg_tuple_get(acl, "name"));
			for (i = 0; i < sizeof(builtin) / sizeof(builtin[0]);
			     i++)
			{
				if (strcasecmp(aclname, builtin[i]) == 0) {
					{
						cfg_obj_log(acl, ISC_LOG_ERROR,
							    "attempt to "
							    "redefine "
							    "builtin acl '%s'",
							    aclname);
						result = ISC_R_FAILURE;
						break;
					}
				}
			}

			for (const cfg_listelt_t *elt2 = cfg_list_next(elt);
			     elt2 != NULL; elt2 = cfg_list_next(elt2))
			{
				const cfg_obj_t *acl2 = cfg_listelt_value(elt2);
				const char *name;
				name = cfg_obj_asstring(
					cfg_tuple_get(acl2, "name"));
				if (strcasecmp(aclname, name) == 0) {
					const char *file = cfg_obj_file(acl);

					if (file == NULL) {
						file = "<unknown file>";
					}

					cfg_obj_log(acl2, ISC_LOG_ERROR,
						    "attempt to redefine "
						    "acl '%s' previous "
						    "definition: %s:%u",
						    name, file, line);
					result = ISC_R_FAILURE;
				}
			}
		}
	}

	if (symtab != NULL) {
		isc_symtab_destroy(&symtab);
	}
	if (inview != NULL) {
		isc_symtab_destroy(&inview);
	}
	if (files != NULL) {
		isc_symtab_destroy(&files);
	}
	if (keydirs != NULL) {
		isc_symtab_destroy(&keydirs);
	}

	return result;
}
