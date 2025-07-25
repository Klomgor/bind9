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

#include <isc/buffer.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/result.h>
#include <isc/stats.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/acl.h>
#include <dns/db.h>
#include <dns/fixedname.h>
#include <dns/ipkeylist.h>
#include <dns/journal.h>
#include <dns/kasp.h>
#include <dns/masterdump.h>
#include <dns/name.h>
#include <dns/nsec3.h>
#include <dns/rdata.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatatype.h>
#include <dns/sdlz.h>
#include <dns/ssu.h>
#include <dns/stats.h>
#include <dns/tsig.h>
#include <dns/view.h>
#include <dns/zone.h>

#include <ns/client.h>

#include <named/config.h>
#include <named/globals.h>
#include <named/log.h>
#include <named/server.h>
#include <named/zoneconf.h>

/* ACLs associated with zone */
typedef enum {
	allow_notify,
	allow_query,
	allow_query_on,
	allow_transfer,
	allow_update,
	allow_update_forwarding
} acl_type_t;

#define CHECK(x)                             \
	do {                                 \
		result = (x);                \
		if (result != ISC_R_SUCCESS) \
			goto cleanup;        \
	} while (0)

/*%
 * Convenience function for configuring a single zone ACL.
 */
static isc_result_t
configure_zone_acl(const cfg_obj_t *zconfig, const cfg_obj_t *vconfig,
		   const cfg_obj_t *config, acl_type_t acltype,
		   cfg_aclconfctx_t *actx, dns_zone_t *zone,
		   void (*setzacl)(dns_zone_t *, dns_acl_t *),
		   void (*clearzacl)(dns_zone_t *)) {
	isc_result_t result;
	const cfg_obj_t *maps[6] = { 0 };
	const cfg_obj_t *aclobj = NULL;
	int i = 0;
	dns_acl_t **aclp = NULL, *acl = NULL;
	const char *aclname;
	dns_view_t *view = NULL;

	view = dns_zone_getview(zone);

	switch (acltype) {
	case allow_notify:
		if (view != NULL) {
			aclp = &view->notifyacl;
		}
		aclname = "allow-notify";
		break;
	case allow_query:
		if (view != NULL) {
			aclp = &view->queryacl;
		}
		aclname = "allow-query";
		break;
	case allow_query_on:
		if (view != NULL) {
			aclp = &view->queryonacl;
		}
		aclname = "allow-query-on";
		break;
	case allow_transfer:
		if (view != NULL) {
			aclp = &view->transferacl;
		}
		aclname = "allow-transfer";
		break;
	case allow_update:
		if (view != NULL) {
			aclp = &view->updateacl;
		}
		aclname = "allow-update";
		break;
	case allow_update_forwarding:
		if (view != NULL) {
			aclp = &view->upfwdacl;
		}
		aclname = "allow-update-forwarding";
		break;
	default:
		UNREACHABLE();
	}

	/* First check to see if ACL is defined within the zone */
	if (zconfig != NULL) {
		maps[i] = cfg_tuple_get(zconfig, "options");
		(void)named_config_get(maps, aclname, &aclobj);
		if (aclobj != NULL) {
			aclp = NULL;
			goto parse_acl;
		}
	}

	if (config != NULL && maps[i] != NULL) {
		const cfg_obj_t *toptions = named_zone_templateopts(config,
								    maps[i]);
		if (toptions != NULL) {
			maps[i++] = toptions;
		}
	}

	/* Failing that, see if there's a default ACL already in the view */
	if (aclp != NULL && *aclp != NULL) {
		(*setzacl)(zone, *aclp);
		return ISC_R_SUCCESS;
	}

	/* Check for default ACLs that haven't been parsed yet */
	if (vconfig != NULL) {
		const cfg_obj_t *options = cfg_tuple_get(vconfig, "options");
		if (options != NULL) {
			maps[i++] = options;
		}
	}
	if (config != NULL) {
		const cfg_obj_t *options = NULL;
		(void)cfg_map_get(config, "options", &options);
		if (options != NULL) {
			maps[i++] = options;
		}
	}
	maps[i++] = named_g_defaultoptions;
	maps[i] = NULL;

	(void)named_config_get(maps, aclname, &aclobj);
	if (aclobj == NULL) {
		(*clearzacl)(zone);
		return ISC_R_SUCCESS;
	}

parse_acl:
	result = cfg_acl_fromconfig(aclobj, config, actx, named_g_mctx, 0,
				    &acl);
	if (result != ISC_R_SUCCESS) {
		return result;
	}
	(*setzacl)(zone, acl);

	/* Set the view default now */
	if (aclp != NULL) {
		dns_acl_attach(acl, aclp);
	}

	dns_acl_detach(&acl);
	return ISC_R_SUCCESS;
}

/*%
 * Parse the zone update-policy statement.
 */
static isc_result_t
configure_zone_ssutable(const cfg_obj_t *zconfig, const cfg_obj_t *tconfig,
			dns_zone_t *zone, const char *zname) {
	const cfg_obj_t *updatepolicy = NULL;
	dns_ssutable_t *table = NULL;
	isc_mem_t *mctx = dns_zone_getmctx(zone);
	bool autoddns = false;
	isc_result_t result = ISC_R_SUCCESS;
	char debug[1024];
	isc_buffer_t dbuf;

	isc_buffer_init(&dbuf, debug, sizeof(debug));
	isc_buffer_setmctx(&dbuf, mctx);

	(void)named_config_findopt(zconfig, tconfig, "update-policy",
				   &updatepolicy);
	if (updatepolicy == NULL) {
		dns_zone_setssutable(zone, NULL);
		return ISC_R_SUCCESS;
	}

	if (cfg_obj_isstring(updatepolicy) &&
	    strcmp("local", cfg_obj_asstring(updatepolicy)) == 0)
	{
		autoddns = true;
		updatepolicy = NULL;
	}

	dns_ssutable_create(mctx, &table);

	CFG_LIST_FOREACH (updatepolicy, element) {
		const cfg_obj_t *stmt = cfg_listelt_value(element);
		const cfg_obj_t *mode = cfg_tuple_get(stmt, "mode");
		const cfg_obj_t *identity = cfg_tuple_get(stmt, "identity");
		const cfg_obj_t *matchtype = cfg_tuple_get(stmt, "matchtype");
		const cfg_obj_t *dname = cfg_tuple_get(stmt, "name");
		const cfg_obj_t *typelist = cfg_tuple_get(stmt, "types");
		const char *str;
		bool grant = false;
		bool usezone = false;
		dns_ssumatchtype_t mtype = dns_ssumatchtype_name;
		dns_fixedname_t fname, fident;
		isc_buffer_t b;
		dns_ssuruletype_t *types;
		unsigned int i, n;
		char namebuf[DNS_NAME_FORMATSIZE];

		isc_buffer_clear(&dbuf);
		str = cfg_obj_asstring(mode);
		if (strcasecmp(str, "grant") == 0) {
			grant = true;
		} else if (strcasecmp(str, "deny") == 0) {
			grant = false;
		} else {
			UNREACHABLE();
		}
		isc_buffer_putstr(&dbuf, str);

		dns_fixedname_init(&fident);
		str = cfg_obj_asstring(identity);
		isc_buffer_constinit(&b, str, strlen(str));
		isc_buffer_add(&b, strlen(str));
		result = dns_name_fromtext(dns_fixedname_name(&fident), &b,
					   dns_rootname, 0);
		if (result != ISC_R_SUCCESS) {
			cfg_obj_log(identity, ISC_LOG_ERROR,
				    "'%s' is not a valid name", str);
			goto cleanup;
		}
		dns_name_format(dns_fixedname_name(&fident), namebuf,
				sizeof(namebuf));
		isc_buffer_putstr(&dbuf, " ");
		isc_buffer_putstr(&dbuf, namebuf);

		str = cfg_obj_asstring(matchtype);
		CHECK(dns_ssu_mtypefromstring(str, &mtype));
		if (mtype == dns_ssumatchtype_subdomain &&
		    strcasecmp(str, "zonesub") == 0)
		{
			usezone = true;
		}
		isc_buffer_putstr(&dbuf, " ");
		isc_buffer_putstr(&dbuf, str);

		dns_fixedname_init(&fname);
		if (usezone) {
			dns_name_copy(dns_zone_getorigin(zone),
				      dns_fixedname_name(&fname));
		} else {
			str = cfg_obj_asstring(dname);
			isc_buffer_constinit(&b, str, strlen(str));
			isc_buffer_add(&b, strlen(str));
			result = dns_name_fromtext(dns_fixedname_name(&fname),
						   &b, dns_rootname, 0);
			if (result != ISC_R_SUCCESS) {
				cfg_obj_log(identity, ISC_LOG_ERROR,
					    "'%s' is not a valid name", str);
				goto cleanup;
			}
			dns_name_format(dns_fixedname_name(&fname), namebuf,
					sizeof(namebuf));
			isc_buffer_putstr(&dbuf, " ");
			isc_buffer_putstr(&dbuf, namebuf);
		}

		n = named_config_listcount(typelist);
		if (n == 0) {
			types = NULL;
		} else {
			types = isc_mem_cget(mctx, n, sizeof(*types));
		}

		i = 0;
		CFG_LIST_FOREACH (typelist, element2) {
			const cfg_obj_t *typeobj;
			const char *bracket;
			isc_textregion_t r;
			unsigned long max = 0;

			INSIST(i < n);

			typeobj = cfg_listelt_value(element2);
			str = cfg_obj_asstring(typeobj);
			r.base = UNCONST(str);
			isc_buffer_putstr(&dbuf, " ");
			isc_buffer_putstr(&dbuf, str);

			bracket = strchr(str, '(' /*)*/);
			if (bracket != NULL) {
				char *end = NULL;
				r.length = bracket - str;
				max = strtoul(bracket + 1, &end, 10);
				if (max > 0xffff || end[0] != /*(*/ ')' ||
				    end[1] != 0)
				{
					cfg_obj_log(identity, ISC_LOG_ERROR,
						    "'%s' is not a valid count",
						    bracket);
					isc_mem_cput(mctx, types, n,
						     sizeof(*types));
					goto cleanup;
				}
			} else {
				r.length = strlen(str);
			}
			types[i].max = max;

			result = dns_rdatatype_fromtext(&types[i++].type, &r);
			if (result != ISC_R_SUCCESS) {
				cfg_obj_log(identity, ISC_LOG_ERROR,
					    "'%.*s' is not a valid type",
					    (int)r.length, str);
				isc_mem_cput(mctx, types, n, sizeof(*types));
				goto cleanup;
			}
		}
		INSIST(i == n);

		isc_buffer_putuint8(&dbuf, '\0');
		dns_ssutable_addrule(table, grant, dns_fixedname_name(&fident),
				     mtype, dns_fixedname_name(&fname), n,
				     types, isc_buffer_base(&dbuf));
		if (types != NULL) {
			isc_mem_cput(mctx, types, n, sizeof(*types));
		}
	}

	/*
	 * If "update-policy local;" and a session key exists,
	 * then use the default policy, which is equivalent to:
	 * update-policy { grant <session-keyname> zonesub any; };
	 */
	if (autoddns) {
		dns_ssuruletype_t any = { dns_rdatatype_any, 0 };

		if (named_g_server->session_keyname == NULL) {
			isc_log_write(NAMED_LOGCATEGORY_GENERAL,
				      NAMED_LOGMODULE_SERVER, ISC_LOG_ERROR,
				      "failed to enable auto DDNS policy "
				      "for zone %s: session key not found",
				      zname);
			result = ISC_R_NOTFOUND;
			goto cleanup;
		}

		dns_ssutable_addrule(
			table, true, named_g_server->session_keyname,
			dns_ssumatchtype_local, dns_zone_getorigin(zone), 1,
			&any, "local");
	}

	dns_zone_setssutable(zone, table);

cleanup:
	isc_buffer_clearmctx(&dbuf);
	dns_ssutable_detach(&table);
	return result;
}

/*
 * This is the TTL used for internally generated RRsets for static-stub zones.
 * The value doesn't matter because the mapping is static, but needs to be
 * defined for the sake of implementation.
 */
#define STATICSTUB_SERVER_TTL 86400

/*%
 * Configure an apex NS with glues for a static-stub zone.
 * For example, for the zone named "example.com", the following RRs will be
 * added to the zone DB:
 * example.com. NS example.com.
 * example.com. A 192.0.2.1
 * example.com. AAAA 2001:db8::1
 */
static isc_result_t
configure_staticstub_serveraddrs(const cfg_obj_t *zconfig, dns_zone_t *zone,
				 dns_rdatalist_t *rdatalist_ns,
				 dns_rdatalist_t *rdatalist_a,
				 dns_rdatalist_t *rdatalist_aaaa) {
	isc_mem_t *mctx = dns_zone_getmctx(zone);
	isc_region_t region, sregion;
	dns_rdata_t *rdata = NULL;
	isc_result_t result = ISC_R_SUCCESS;

	CFG_LIST_FOREACH (zconfig, element) {
		const isc_sockaddr_t *sa;
		isc_netaddr_t na;
		const cfg_obj_t *address = cfg_listelt_value(element);
		dns_rdatalist_t *rdatalist;

		sa = cfg_obj_assockaddr(address);
		if (isc_sockaddr_getport(sa) != 0) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "port is not configurable for "
				    "static stub server-addresses");
			return ISC_R_FAILURE;
		}
		isc_netaddr_fromsockaddr(&na, sa);
		if (isc_netaddr_getzone(&na) != 0) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "scoped address is not allowed "
				    "for static stub "
				    "server-addresses");
			return ISC_R_FAILURE;
		}

		switch (na.family) {
		case AF_INET:
			region.length = sizeof(na.type.in);
			rdatalist = rdatalist_a;
			break;
		default:
			INSIST(na.family == AF_INET6);
			region.length = sizeof(na.type.in6);
			rdatalist = rdatalist_aaaa;
			break;
		}

		rdata = isc_mem_get(mctx, sizeof(*rdata) + region.length);
		region.base = (unsigned char *)(rdata + 1);
		memmove(region.base, &na.type, region.length);
		dns_rdata_init(rdata);
		dns_rdata_fromregion(rdata, dns_zone_getclass(zone),
				     rdatalist->type, &region);
		ISC_LIST_APPEND(rdatalist->rdata, rdata, link);
	}

	/*
	 * If no address is specified (unlikely in this context, but possible),
	 * there's nothing to do anymore.
	 */
	if (ISC_LIST_EMPTY(rdatalist_a->rdata) &&
	    ISC_LIST_EMPTY(rdatalist_aaaa->rdata))
	{
		return ISC_R_SUCCESS;
	}

	/* Add to the list an apex NS with the ns name being the origin name */
	dns_name_toregion(dns_zone_getorigin(zone), &sregion);
	rdata = isc_mem_get(mctx, sizeof(*rdata) + sregion.length);
	region.length = sregion.length;
	region.base = (unsigned char *)(rdata + 1);
	memmove(region.base, sregion.base, region.length);
	dns_rdata_init(rdata);
	dns_rdata_fromregion(rdata, dns_zone_getclass(zone), dns_rdatatype_ns,
			     &region);
	ISC_LIST_APPEND(rdatalist_ns->rdata, rdata, link);

	return result;
}

/*%
 * Configure an apex NS with an out-of-zone NS names for a static-stub zone.
 * For example, for the zone named "example.com", something like the following
 * RRs will be added to the zone DB:
 * example.com. NS ns.example.net.
 */
static isc_result_t
configure_staticstub_servernames(const cfg_obj_t *zconfig, dns_zone_t *zone,
				 dns_rdatalist_t *rdatalist,
				 const char *zname) {
	isc_mem_t *mctx = dns_zone_getmctx(zone);
	dns_rdata_t *rdata = NULL;
	isc_region_t sregion, region;
	isc_result_t result = ISC_R_SUCCESS;

	CFG_LIST_FOREACH (zconfig, element) {
		const cfg_obj_t *obj = NULL;
		const char *str = NULL;
		dns_fixedname_t fixed_name;
		dns_name_t *nsname = NULL;
		isc_buffer_t b;

		obj = cfg_listelt_value(element);
		str = cfg_obj_asstring(obj);

		nsname = dns_fixedname_initname(&fixed_name);

		isc_buffer_constinit(&b, str, strlen(str));
		isc_buffer_add(&b, strlen(str));
		result = dns_name_fromtext(nsname, &b, dns_rootname, 0);
		if (result != ISC_R_SUCCESS) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "server-name '%s' is not a valid "
				    "name",
				    str);
			return result;
		}
		if (dns_name_issubdomain(nsname, dns_zone_getorigin(zone))) {
			cfg_obj_log(zconfig, ISC_LOG_ERROR,
				    "server-name '%s' must not be a "
				    "subdomain of zone name '%s'",
				    str, zname);
			return ISC_R_FAILURE;
		}

		dns_name_toregion(nsname, &sregion);
		rdata = isc_mem_get(mctx, sizeof(*rdata) + sregion.length);
		region.length = sregion.length;
		region.base = (unsigned char *)(rdata + 1);
		memmove(region.base, sregion.base, region.length);
		dns_rdata_init(rdata);
		dns_rdata_fromregion(rdata, dns_zone_getclass(zone),
				     dns_rdatatype_ns, &region);
		ISC_LIST_APPEND(rdatalist->rdata, rdata, link);
	}

	return result;
}

/*%
 * Configure static-stub zone.
 */
static isc_result_t
configure_staticstub(const cfg_obj_t *zconfig, const cfg_obj_t *tconfig,
		     dns_zone_t *zone, const char *zname, const char *dbtype) {
	int i = 0;
	const cfg_obj_t *obj;
	isc_mem_t *mctx = dns_zone_getmctx(zone);
	dns_db_t *db = NULL;
	dns_dbversion_t *dbversion = NULL;
	dns_dbnode_t *apexnode = NULL;
	dns_name_t apexname;
	isc_result_t result;
	dns_rdataset_t rdataset;
	dns_rdatalist_t rdatalist_ns, rdatalist_a, rdatalist_aaaa;
	dns_rdatalist_t *rdatalists[] = { &rdatalist_ns, &rdatalist_a,
					  &rdatalist_aaaa, NULL };
	isc_region_t region;

	/* Create the DB beforehand */
	result = dns_db_create(mctx, dbtype, dns_zone_getorigin(zone),
			       dns_dbtype_stub, dns_zone_getclass(zone), 0,
			       NULL, &db);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	dns_rdataset_init(&rdataset);

	dns_rdatalist_init(&rdatalist_ns);
	rdatalist_ns.rdclass = dns_zone_getclass(zone);
	rdatalist_ns.type = dns_rdatatype_ns;
	rdatalist_ns.ttl = STATICSTUB_SERVER_TTL;

	dns_rdatalist_init(&rdatalist_a);
	rdatalist_a.rdclass = dns_zone_getclass(zone);
	rdatalist_a.type = dns_rdatatype_a;
	rdatalist_a.ttl = STATICSTUB_SERVER_TTL;

	dns_rdatalist_init(&rdatalist_aaaa);
	rdatalist_aaaa.rdclass = dns_zone_getclass(zone);
	rdatalist_aaaa.type = dns_rdatatype_aaaa;
	rdatalist_aaaa.ttl = STATICSTUB_SERVER_TTL;

	/* Prepare zone RRs from the configuration */
	obj = NULL;
	(void)named_config_findopt(zconfig, tconfig, "server-addresses", &obj);
	if (obj != NULL) {
		CHECK(configure_staticstub_serveraddrs(obj, zone, &rdatalist_ns,
						       &rdatalist_a,
						       &rdatalist_aaaa));
	}

	obj = NULL;
	(void)named_config_findopt(zconfig, tconfig, "server-names", &obj);
	if (obj != NULL) {
		CHECK(configure_staticstub_servernames(obj, zone, &rdatalist_ns,
						       zname));
	}

	/*
	 * Sanity check: there should be at least one NS RR at the zone apex
	 * to trigger delegation.
	 */
	if (ISC_LIST_EMPTY(rdatalist_ns.rdata)) {
		isc_log_write(NAMED_LOGCATEGORY_GENERAL, NAMED_LOGMODULE_SERVER,
			      ISC_LOG_ERROR,
			      "No NS record is configured for a "
			      "static-stub zone '%s'",
			      zname);
		result = ISC_R_FAILURE;
		goto cleanup;
	}

	/*
	 * Now add NS and glue A/AAAA RRsets to the zone DB.
	 * First open a new version for the add operation and get a pointer
	 * to the apex node (all RRs are of the apex name).
	 */
	CHECK(dns_db_newversion(db, &dbversion));

	dns_name_init(&apexname);
	dns_name_clone(dns_zone_getorigin(zone), &apexname);
	CHECK(dns_db_findnode(db, &apexname, false, &apexnode));

	/* Add NS RRset */
	dns_rdatalist_tordataset(&rdatalist_ns, &rdataset);
	CHECK(dns_db_addrdataset(db, apexnode, dbversion, 0, &rdataset, 0,
				 NULL));
	dns_rdataset_disassociate(&rdataset);

	/* Add glue A RRset, if any */
	if (!ISC_LIST_EMPTY(rdatalist_a.rdata)) {
		dns_rdatalist_tordataset(&rdatalist_a, &rdataset);
		CHECK(dns_db_addrdataset(db, apexnode, dbversion, 0, &rdataset,
					 0, NULL));
		dns_rdataset_disassociate(&rdataset);
	}

	/* Add glue AAAA RRset, if any */
	if (!ISC_LIST_EMPTY(rdatalist_aaaa.rdata)) {
		dns_rdatalist_tordataset(&rdatalist_aaaa, &rdataset);
		CHECK(dns_db_addrdataset(db, apexnode, dbversion, 0, &rdataset,
					 0, NULL));
		dns_rdataset_disassociate(&rdataset);
	}

	dns_db_closeversion(db, &dbversion, true);
	dns_zone_setdb(zone, db);

	result = ISC_R_SUCCESS;

cleanup:
	if (dns_rdataset_isassociated(&rdataset)) {
		dns_rdataset_disassociate(&rdataset);
	}
	if (apexnode != NULL) {
		dns_db_detachnode(db, &apexnode);
	}
	if (dbversion != NULL) {
		dns_db_closeversion(db, &dbversion, false);
	}
	if (db != NULL) {
		dns_db_detach(&db);
	}
	for (i = 0; rdatalists[i] != NULL; i++) {
		ISC_LIST_FOREACH (rdatalists[i]->rdata, rdata, link) {
			ISC_LIST_UNLINK(rdatalists[i]->rdata, rdata, link);
			dns_rdata_toregion(rdata, &region);
			isc_mem_put(mctx, rdata,
				    sizeof(*rdata) + region.length);
		}
	}

	INSIST(dbversion == NULL);

	return result;
}

/*%
 * Convert a config file zone type into a server zone type.
 */
static dns_zonetype_t
zonetype_fromconfig(const cfg_obj_t *zmap, const cfg_obj_t *tmap) {
	const cfg_obj_t *obj = NULL;

	(void)named_config_findopt(zmap, tmap, "type", &obj);
	INSIST(obj != NULL);
	return named_config_getzonetype(obj);
}

/*%
 * Helper function for strtoargv().  Pardon the gratuitous recursion.
 */
static isc_result_t
strtoargvsub(isc_mem_t *mctx, char *s, unsigned int *argcp, char ***argvp,
	     unsigned int n) {
	isc_result_t result;

	/* Discard leading whitespace. */
	while (*s == ' ' || *s == '\t') {
		s++;
	}

	if (*s == '\0') {
		/* We have reached the end of the string. */
		*argcp = n;
		*argvp = isc_mem_cget(mctx, n, sizeof(char *));
	} else {
		char *p = s;
		while (*p != ' ' && *p != '\t' && *p != '\0') {
			p++;
		}
		if (*p != '\0') {
			*p++ = '\0';
		}

		result = strtoargvsub(mctx, p, argcp, argvp, n + 1);
		if (result != ISC_R_SUCCESS) {
			return result;
		}
		(*argvp)[n] = s;
	}
	return ISC_R_SUCCESS;
}

/*%
 * Tokenize the string "s" into whitespace-separated words,
 * return the number of words in '*argcp' and an array
 * of pointers to the words in '*argvp'.  The caller
 * must free the array using isc_mem_put().  The string
 * is modified in-place.
 */
static isc_result_t
strtoargv(isc_mem_t *mctx, char *s, unsigned int *argcp, char ***argvp) {
	return strtoargvsub(mctx, s, argcp, argvp, 0);
}

static const char *const primary_synonyms[] = { "primary", "master", NULL };

static const char *const secondary_synonyms[] = { "secondary", "slave", NULL };

static void
checknames(dns_zonetype_t ztype, const cfg_obj_t **maps,
	   const cfg_obj_t **objp) {
	isc_result_t result;

	switch (ztype) {
	case dns_zone_secondary:
	case dns_zone_mirror:
		result = named_checknames_get(maps, secondary_synonyms, objp);
		break;
	case dns_zone_primary:
		result = named_checknames_get(maps, primary_synonyms, objp);
		break;
	default:
		UNREACHABLE();
	}

	INSIST(result == ISC_R_SUCCESS && objp != NULL && *objp != NULL);
}

/*
 * Callback to see if a non-recursive query coming from 'srcaddr' to
 * 'destaddr', with optional key 'mykey' for class 'rdclass' would be
 * delivered to 'myview'.
 *
 * We run this unlocked as both the view list and the interface list
 * are updated when the appropriate task has exclusivity.
 */
static bool
isself(dns_view_t *myview, dns_tsigkey_t *mykey, const isc_sockaddr_t *srcaddr,
       const isc_sockaddr_t *dstaddr, dns_rdataclass_t rdclass,
       void *arg ISC_ATTR_UNUSED) {
	dns_aclenv_t *env = NULL;
	dns_tsigkey_t *key = NULL;
	isc_netaddr_t netsrc;
	isc_netaddr_t netdst;

	/* interfacemgr can be destroyed only in exclusive mode. */
	if (named_g_server->interfacemgr == NULL) {
		return true;
	}

	if (!ns_interfacemgr_listeningon(named_g_server->interfacemgr, dstaddr))
	{
		return false;
	}

	isc_netaddr_fromsockaddr(&netsrc, srcaddr);
	isc_netaddr_fromsockaddr(&netdst, dstaddr);
	env = ns_interfacemgr_getaclenv(named_g_server->interfacemgr);

	ISC_LIST_FOREACH (named_g_server->viewlist, view, link) {
		const dns_name_t *tsig = NULL;

		if (view->matchrecursiveonly) {
			continue;
		}

		if (rdclass != view->rdclass) {
			continue;
		}

		if (mykey != NULL) {
			bool match;
			isc_result_t result;

			result = dns_view_gettsig(view, mykey->name, &key);
			if (result != ISC_R_SUCCESS) {
				continue;
			}
			match = dst_key_compare(mykey->key, key->key);
			dns_tsigkey_detach(&key);
			if (!match) {
				continue;
			}
			tsig = dns_tsigkey_identity(mykey);
		}

		if (dns_acl_allowed(&netsrc, tsig, view->matchclients, env) &&
		    dns_acl_allowed(&netdst, tsig, view->matchdestinations,
				    env))
		{
			return view == myview;
		}
	}

	return false;
}

/*%
 * For mirror zones, change "notify yes;" to "notify explicit;", informing the
 * user only if "notify" was explicitly configured rather than inherited from
 * default configuration.
 */
static dns_notifytype_t
process_notifytype(dns_notifytype_t ntype, dns_zonetype_t ztype,
		   const char *zname, const cfg_obj_t **maps) {
	const cfg_obj_t *obj = NULL;

	/*
	 * Return the original setting if this is not a mirror zone or if the
	 * zone is configured with something else than "notify yes;".
	 */
	if (ztype != dns_zone_mirror || ntype != dns_notifytype_yes) {
		return ntype;
	}

	/*
	 * Only log a message if "notify" was set in the configuration
	 * hierarchy supplied in 'maps'.
	 */
	if (named_config_get(maps, "notify", &obj) == ISC_R_SUCCESS) {
		cfg_obj_log(obj, ISC_LOG_INFO,
			    "'notify explicit;' will be used for mirror zone "
			    "'%s'",
			    zname);
	}

	return dns_notifytype_explicit;
}

isc_result_t
named_zone_configure(const cfg_obj_t *config, const cfg_obj_t *vconfig,
		     const cfg_obj_t *zconfig, cfg_aclconfctx_t *ac,
		     dns_kasplist_t *kasplist, dns_keystorelist_t *keystorelist,
		     dns_zone_t *zone, dns_zone_t *raw) {
	isc_result_t result;
	const char *zname;
	dns_rdataclass_t zclass;
	dns_rdataclass_t vclass;
	const cfg_obj_t *maps[6];
	const cfg_obj_t *nodefault[5];
	const cfg_obj_t *nooptions[3];
	const cfg_obj_t *zoptions = NULL;
	const cfg_obj_t *toptions = NULL;
	const cfg_obj_t *options = NULL;
	const cfg_obj_t *obj = NULL;
	const char *filename = NULL;
	const char *initial_file = NULL;
	const char *kaspname = NULL;
	const char *dupcheck;
	dns_checkdstype_t checkdstype = dns_checkdstype_yes;
	dns_notifytype_t notifytype = dns_notifytype_yes;
	uint32_t count;
	unsigned int dbargc;
	char **dbargv;
	static char default_dbtype[] = ZONEDB_DEFAULT;
	static char dlz_dbtype[] = "dlz";
	char *cpval = default_dbtype;
	isc_mem_t *mctx = dns_zone_getmctx(zone);
	dns_zonetype_t ztype;
	int i;
	int32_t journal_size;
	bool multi;
	dns_kasp_t *kasp = NULL;
	bool check = false, fail = false;
	bool warn = false, ignore = false;
	bool ixfrdiff;
	bool use_kasp = false;
	dns_masterformat_t masterformat;
	const dns_master_style_t *masterstyle = &dns_master_style_default;
	isc_stats_t *zoneqrystats;
	dns_stats_t *rcvquerystats;
	dns_stats_t *dnssecsignstats;
	dns_zonestat_level_t statlevel = dns_zonestat_none;
	dns_ttl_t maxttl = 0; /* unlimited */
	dns_zone_t *mayberaw = (raw != NULL) ? raw : zone;
	bool transferinsecs = ns_server_getoption(named_g_server->sctx,
						  NS_SERVER_TRANSFERINSECS);

	REQUIRE(config != NULL);
	REQUIRE(zconfig != NULL);

	i = 0;

	zoptions = cfg_tuple_get(zconfig, "options");
	INSIST(zoptions != NULL);
	nodefault[i] = nooptions[i] = maps[i] = zoptions;
	i++;

	toptions = named_zone_templateopts(config, zoptions);
	if (toptions != NULL) {
		nodefault[i] = nooptions[i] = maps[i] = toptions;
		i++;
	}

	nooptions[i] = NULL;
	if (vconfig != NULL) {
		nodefault[i] = maps[i] = cfg_tuple_get(vconfig, "options");
		i++;
	}

	(void)cfg_map_get(config, "options", &options);
	if (options != NULL) {
		nodefault[i] = maps[i] = options;
		i++;
	}

	nodefault[i] = NULL;
	maps[i++] = named_g_defaultoptions;
	maps[i] = NULL;

	if (vconfig != NULL) {
		CHECK(named_config_getclass(cfg_tuple_get(vconfig, "class"),
					    dns_rdataclass_in, &vclass));
	} else {
		vclass = dns_rdataclass_in;
	}

	/*
	 * Configure values common to all zone types.
	 */

	zname = cfg_obj_asstring(cfg_tuple_get(zconfig, "name"));

	CHECK(named_config_getclass(cfg_tuple_get(zconfig, "class"), vclass,
				    &zclass));
	dns_zone_setclass(zone, zclass);
	if (raw != NULL) {
		dns_zone_setclass(raw, zclass);
	}

	ztype = zonetype_fromconfig(zoptions, toptions);
	if (raw != NULL) {
		dns_zone_settype(raw, ztype);
		dns_zone_settype(zone, dns_zone_primary);
	} else {
		dns_zone_settype(zone, ztype);
	}

	obj = NULL;
	result = named_config_get(nooptions, "database", &obj);
	if (result == ISC_R_SUCCESS) {
		cpval = isc_mem_strdup(mctx, cfg_obj_asstring(obj));
	}

	obj = NULL;
	result = named_config_get(nooptions, "dlz", &obj);
	if (result == ISC_R_SUCCESS) {
		const char *dlzname = cfg_obj_asstring(obj);
		size_t len = strlen(dlzname) + 5;
		cpval = isc_mem_allocate(mctx, len);
		snprintf(cpval, len, "dlz %s", dlzname);
	}

	result = strtoargv(mctx, cpval, &dbargc, &dbargv);
	if (result != ISC_R_SUCCESS && cpval != default_dbtype) {
		isc_mem_free(mctx, cpval);
		CHECK(result);
	}

	/*
	 * ANSI C is strange here.  There is no logical reason why (char **)
	 * cannot be promoted automatically to (const char * const *) by the
	 * compiler w/o generating a warning.
	 */
	dns_zone_setdbtype(zone, dbargc, (const char *const *)dbargv);
	isc_mem_cput(mctx, dbargv, dbargc, sizeof(*dbargv));
	if (cpval != default_dbtype && cpval != dlz_dbtype) {
		isc_mem_free(mctx, cpval);
	}

	obj = NULL;
	result = named_config_get(nooptions, "file", &obj);
	if (result == ISC_R_SUCCESS) {
		filename = cfg_obj_asstring(obj);
	}

	obj = NULL;
	result = named_config_get(nooptions, "initial-file", &obj);
	if (result == ISC_R_SUCCESS) {
		initial_file = cfg_obj_asstring(obj);
	}

	if (ztype == dns_zone_secondary || ztype == dns_zone_mirror) {
		masterformat = dns_masterformat_raw;
	} else {
		masterformat = dns_masterformat_text;
	}
	obj = NULL;
	result = named_config_get(maps, "masterfile-format", &obj);
	if (result == ISC_R_SUCCESS) {
		const char *masterformatstr = cfg_obj_asstring(obj);

		if (strcasecmp(masterformatstr, "text") == 0) {
			masterformat = dns_masterformat_text;
		} else {
			masterformat = dns_masterformat_raw;
		}
	}

	obj = NULL;
	result = named_config_get(maps, "masterfile-style", &obj);
	if (result == ISC_R_SUCCESS) {
		const char *masterstylestr = cfg_obj_asstring(obj);
		if (strcasecmp(masterstylestr, "full") == 0) {
			masterstyle = &dns_master_style_full;
		} else {
			masterstyle = &dns_master_style_default;
		}
	}

	obj = NULL;
	result = named_config_get(maps, "max-records", &obj);
	INSIST(result == ISC_R_SUCCESS && obj != NULL);
	dns_zone_setmaxrecords(mayberaw, cfg_obj_asuint32(obj));
	if (zone != mayberaw) {
		dns_zone_setmaxrecords(zone, 0);
	}

	obj = NULL;
	result = named_config_get(maps, "max-records-per-type", &obj);
	INSIST(result == ISC_R_SUCCESS && obj != NULL);
	dns_zone_setmaxrrperset(mayberaw, cfg_obj_asuint32(obj));
	if (zone != mayberaw) {
		dns_zone_setmaxrrperset(zone, 0);
	}

	obj = NULL;
	result = named_config_get(maps, "max-types-per-name", &obj);
	INSIST(result == ISC_R_SUCCESS && obj != NULL);
	dns_zone_setmaxtypepername(mayberaw, cfg_obj_asuint32(obj));
	if (zone != mayberaw) {
		dns_zone_setmaxtypepername(zone, 0);
	}

	if (raw != NULL && filename != NULL) {
#define SIGNED ".signed"
		size_t signedlen = strlen(filename) + sizeof(SIGNED);
		char *signedname;

		dns_zone_setfile(raw, filename, initial_file, masterformat,
				 masterstyle);
		signedname = isc_mem_get(mctx, signedlen);

		(void)snprintf(signedname, signedlen, "%s" SIGNED, filename);
		dns_zone_setfile(zone, signedname, NULL, dns_masterformat_raw,
				 NULL);
		isc_mem_put(mctx, signedname, signedlen);
	} else {
		dns_zone_setfile(zone, filename, initial_file, masterformat,
				 masterstyle);
	}

	obj = NULL;
	result = named_config_get(nooptions, "journal", &obj);
	if (result == ISC_R_SUCCESS) {
		dns_zone_setjournal(mayberaw, cfg_obj_asstring(obj));
	}

	/*
	 * Notify messages are processed by the raw zone if it exists.
	 */
	if (ztype == dns_zone_secondary || ztype == dns_zone_mirror) {
		CHECK(configure_zone_acl(zconfig, vconfig, config, allow_notify,
					 ac, mayberaw, dns_zone_setnotifyacl,
					 dns_zone_clearnotifyacl));
	}

	/*
	 * XXXAG This probably does not make sense for stubs.
	 */
	CHECK(configure_zone_acl(zconfig, vconfig, config, allow_query, ac,
				 zone, dns_zone_setqueryacl,
				 dns_zone_clearqueryacl));

	CHECK(configure_zone_acl(zconfig, vconfig, config, allow_query_on, ac,
				 zone, dns_zone_setqueryonacl,
				 dns_zone_clearqueryonacl));

	obj = NULL;
	result = named_config_get(maps, "zone-statistics", &obj);
	INSIST(result == ISC_R_SUCCESS && obj != NULL);
	if (cfg_obj_isboolean(obj)) {
		if (cfg_obj_asboolean(obj)) {
			statlevel = dns_zonestat_full;
		} else {
			statlevel = dns_zonestat_none;
		}
	} else {
		const char *levelstr = cfg_obj_asstring(obj);
		if (strcasecmp(levelstr, "full") == 0) {
			statlevel = dns_zonestat_full;
		} else if (strcasecmp(levelstr, "terse") == 0) {
			statlevel = dns_zonestat_terse;
		} else if (strcasecmp(levelstr, "none") == 0) {
			statlevel = dns_zonestat_none;
		} else {
			UNREACHABLE();
		}
	}
	dns_zone_setstatlevel(zone, statlevel);

	zoneqrystats = NULL;
	rcvquerystats = NULL;
	dnssecsignstats = NULL;
	if (statlevel == dns_zonestat_full) {
		isc_stats_create(mctx, &zoneqrystats, ns_statscounter_max);
		dns_rdatatypestats_create(mctx, &rcvquerystats);
		dns_dnssecsignstats_create(mctx, &dnssecsignstats);
	}
	dns_zone_setrequeststats(zone, zoneqrystats);
	dns_zone_setrcvquerystats(zone, rcvquerystats);
	dns_zone_setdnssecsignstats(zone, dnssecsignstats);

	if (zoneqrystats != NULL) {
		isc_stats_detach(&zoneqrystats);
	}

	if (rcvquerystats != NULL) {
		dns_stats_detach(&rcvquerystats);
	}

	if (dnssecsignstats != NULL) {
		dns_stats_detach(&dnssecsignstats);
	}

	/*
	 * Configure authoritative zone functionality.  This applies
	 * to primary servers (type "primary") and secondaries
	 * acting as primaries (type "secondary"), but not to stubs.
	 */
	if (ztype != dns_zone_stub && ztype != dns_zone_staticstub &&
	    ztype != dns_zone_redirect)
	{
		bool logreports = false;

		/* Make a reference to the default policy. */
		result = dns_kasplist_find(kasplist, "default", &kasp);
		INSIST(result == ISC_R_SUCCESS && kasp != NULL);
		dns_zone_setdefaultkasp(zone, kasp);
		dns_kasp_detach(&kasp);

		obj = NULL;
		result = named_config_get(maps, "dnssec-policy", &obj);
		if (result == ISC_R_SUCCESS) {
			kaspname = cfg_obj_asstring(obj);
			if (strcmp(kaspname, "none") != 0) {
				result = dns_kasplist_find(kasplist, kaspname,
							   &kasp);
				if (result != ISC_R_SUCCESS) {
					cfg_obj_log(
						obj, ISC_LOG_ERROR,
						"dnssec-policy '%s' not found ",
						kaspname);
					CHECK(result);
				}
				dns_zone_setkasp(zone, kasp);
				use_kasp = true;
			}
		}
		if (!use_kasp) {
			dns_zone_setkasp(zone, NULL);
		}

		obj = NULL;
		result = named_config_get(maps, "provide-zoneversion", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setoption(zone, DNS_ZONEOPT_ZONEVERSION,
				   cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(maps, "notify", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (cfg_obj_isboolean(obj)) {
			if (cfg_obj_asboolean(obj)) {
				notifytype = dns_notifytype_yes;
			} else {
				notifytype = dns_notifytype_no;
			}
		} else {
			const char *str = cfg_obj_asstring(obj);
			if (strcasecmp(str, "explicit") == 0) {
				notifytype = dns_notifytype_explicit;
			} else if (strcasecmp(str, "master-only") == 0 ||
				   strcasecmp(str, "primary-only") == 0)
			{
				notifytype = dns_notifytype_masteronly;
			} else {
				UNREACHABLE();
			}
		}
		notifytype = process_notifytype(notifytype, ztype, zname,
						nodefault);
		if (raw != NULL) {
			dns_zone_setnotifytype(raw, dns_notifytype_no);
		}
		dns_zone_setnotifytype(zone, notifytype);

		obj = NULL;
		result = named_config_get(maps, "also-notify", &obj);
		if (result == ISC_R_SUCCESS &&
		    (notifytype == dns_notifytype_yes ||
		     notifytype == dns_notifytype_explicit ||
		     (notifytype == dns_notifytype_masteronly &&
		      ztype == dns_zone_primary)))
		{
			dns_ipkeylist_t ipkl;
			dns_ipkeylist_init(&ipkl);

			CHECK(named_config_getipandkeylist(config, obj, mctx,
							   &ipkl));
			dns_zone_setalsonotify(zone, ipkl.addrs, ipkl.sources,
					       ipkl.keys, ipkl.tlss,
					       ipkl.count);
			dns_ipkeylist_clear(mctx, &ipkl);
		} else {
			dns_zone_setalsonotify(zone, NULL, NULL, NULL, NULL, 0);
		}

		obj = NULL;
		result = named_config_get(maps, "parental-source", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setparentalsrc4(zone, cfg_obj_assockaddr(obj));

		obj = NULL;
		result = named_config_get(maps, "parental-source-v6", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setparentalsrc6(zone, cfg_obj_assockaddr(obj));

		obj = NULL;
		result = named_config_get(maps, "notify-source", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setnotifysrc4(zone, cfg_obj_assockaddr(obj));

		obj = NULL;
		result = named_config_get(maps, "notify-source-v6", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setnotifysrc6(zone, cfg_obj_assockaddr(obj));

		obj = NULL;
		result = named_config_get(maps, "notify-to-soa", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setoption(zone, DNS_ZONEOPT_NOTIFYTOSOA,
				   cfg_obj_asboolean(obj));

		dns_zone_setisself(zone, isself, NULL);

		CHECK(configure_zone_acl(
			zconfig, vconfig, config, allow_transfer, ac, zone,
			dns_zone_setxfracl, dns_zone_clearxfracl));

		obj = NULL;
		result = named_config_get(maps, "max-transfer-time-out", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setmaxxfrout(
			zone, transferinsecs ? cfg_obj_asuint32(obj)
					     : cfg_obj_asuint32(obj) * 60);

		obj = NULL;
		result = named_config_get(maps, "max-transfer-idle-out", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setidleout(zone, transferinsecs
						  ? cfg_obj_asuint32(obj)
						  : cfg_obj_asuint32(obj) * 60);

		obj = NULL;
		result = named_config_get(maps, "max-journal-size", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (raw != NULL) {
			dns_zone_setjournalsize(raw, -1);
		}
		dns_zone_setjournalsize(zone, -1);
		if (cfg_obj_isstring(obj)) {
			const char *str = cfg_obj_asstring(obj);
			if (strcasecmp(str, "unlimited") == 0) {
				journal_size = DNS_JOURNAL_SIZE_MAX;
			} else {
				INSIST(strcasecmp(str, "default") == 0);
				journal_size = -1;
			}
		} else {
			journal_size = (uint32_t)cfg_obj_asuint64(obj);
		}
		if (raw != NULL) {
			dns_zone_setjournalsize(raw, journal_size);
		}
		dns_zone_setjournalsize(zone, journal_size);

		obj = NULL;
		result = named_config_get(maps, "ixfr-from-differences", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (cfg_obj_isboolean(obj)) {
			ixfrdiff = cfg_obj_asboolean(obj);
		} else if ((strcasecmp(cfg_obj_asstring(obj), "primary") == 0 ||
			    strcasecmp(cfg_obj_asstring(obj), "master") == 0) &&
			   ztype == dns_zone_primary)
		{
			ixfrdiff = true;
		} else if ((strcasecmp(cfg_obj_asstring(obj), "secondary") ==
				    0 ||
			    strcasecmp(cfg_obj_asstring(obj), "slave") == 0) &&
			   ztype == dns_zone_secondary)
		{
			ixfrdiff = true;
		} else {
			ixfrdiff = false;
		}
		if (raw != NULL) {
			dns_zone_setoption(raw, DNS_ZONEOPT_IXFRFROMDIFFS,
					   true);
			dns_zone_setoption(zone, DNS_ZONEOPT_IXFRFROMDIFFS,
					   false);
		} else {
			dns_zone_setoption(zone, DNS_ZONEOPT_IXFRFROMDIFFS,
					   ixfrdiff);
		}

		obj = NULL;
		result = named_config_get(maps, "max-ixfr-ratio", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (cfg_obj_isstring(obj)) {
			dns_zone_setixfrratio(zone, 0);
		} else {
			dns_zone_setixfrratio(zone, cfg_obj_aspercentage(obj));
		}

		obj = NULL;
		result = named_config_get(maps, "request-expire", &obj);
		INSIST(result == ISC_R_SUCCESS);
		dns_zone_setrequestexpire(zone, cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(maps, "request-ixfr", &obj);
		INSIST(result == ISC_R_SUCCESS);
		dns_zone_setrequestixfr(zone, cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(maps, "request-ixfr-max-diffs", &obj);
		INSIST(result == ISC_R_SUCCESS);
		dns_zone_setrequestixfrmaxdiffs(zone, cfg_obj_asuint32(obj));

		obj = NULL;
		checknames(ztype, maps, &obj);
		INSIST(obj != NULL);
		if (strcasecmp(cfg_obj_asstring(obj), "warn") == 0) {
			fail = false;
			check = true;
		} else if (strcasecmp(cfg_obj_asstring(obj), "fail") == 0) {
			fail = check = true;
		} else if (strcasecmp(cfg_obj_asstring(obj), "ignore") == 0) {
			fail = check = false;
		} else {
			UNREACHABLE();
		}
		if (raw != NULL) {
			dns_zone_setoption(raw, DNS_ZONEOPT_CHECKNAMES, check);
			dns_zone_setoption(raw, DNS_ZONEOPT_CHECKNAMESFAIL,
					   fail);
			dns_zone_setoption(zone, DNS_ZONEOPT_CHECKNAMES, false);
			dns_zone_setoption(zone, DNS_ZONEOPT_CHECKNAMESFAIL,
					   false);
		} else {
			dns_zone_setoption(zone, DNS_ZONEOPT_CHECKNAMES, check);
			dns_zone_setoption(zone, DNS_ZONEOPT_CHECKNAMESFAIL,
					   fail);
		}

		obj = NULL;
		result = named_config_get(maps, "notify-delay", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setnotifydelay(zone, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "notify-defer", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setnotifydefer(zone, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "check-sibling", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setoption(zone, DNS_ZONEOPT_CHECKSIBLING,
				   cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(maps, "check-spf", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (strcasecmp(cfg_obj_asstring(obj), "warn") == 0) {
			check = true;
		} else if (strcasecmp(cfg_obj_asstring(obj), "ignore") == 0) {
			check = false;
		} else {
			UNREACHABLE();
		}
		dns_zone_setoption(zone, DNS_ZONEOPT_CHECKSPF, check);

		obj = NULL;
		result = named_config_get(maps, "check-svcb", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setoption(zone, DNS_ZONEOPT_CHECKSVCB,
				   cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(maps, "zero-no-soa-ttl", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setzeronosoattl(zone, cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(maps, "nsec3-test-zone", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setoption(zone, DNS_ZONEOPT_NSEC3TESTZONE,
				   cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(nooptions, "log-report-channel",
					  &obj);
		if (result == ISC_R_SUCCESS) {
			logreports = cfg_obj_asboolean(obj);
			dns_zone_setoption(zone, DNS_ZONEOPT_LOGREPORTS,
					   logreports);
		}
		obj = NULL;
		result = named_config_get(maps, "send-report-channel", &obj);
		if (result == ISC_R_SUCCESS && obj != NULL) {
			dns_fixedname_t fixed;
			dns_name_t *rad = dns_fixedname_initname(&fixed);
			const char *adstr = cfg_obj_asstring(obj);
			dns_name_t *zn = dns_zone_getorigin(zone);

			CHECK(dns_name_fromstring(rad, adstr, dns_rootname, 0,
						  mctx));
			if (logreports || dns_name_equal(rad, dns_rootname)) {
				/* Disable RC for error-logging zones or root */
				dns_zone_setrad(zone, NULL);
			} else if (dns_name_equal(rad, zn)) {
				/*
				 * It's illegal to set a matching agent
				 * domain at the zone level, but it could
				 * be set in options/view. If so, and the
				 * matching zone doesn't log reports, warn.
				 */
				cfg_obj_log(obj, ISC_LOG_WARNING,
					    "send-report-channel is set to "
					    "'%s' but that zone does not have "
					    "log-report-channel set",
					    zname);
				dns_zone_setrad(zone, NULL);
			} else if (dns_name_issubdomain(rad, zn)) {
				cfg_obj_log(obj, ISC_LOG_WARNING,
					    "send-report-channel '%s' ignored "
					    "for zone '%s' because it is a "
					    "subdomain of the zone",
					    adstr, zname);
				dns_zone_setrad(zone, NULL);
			} else {
				dns_zone_setrad(zone, rad);
			}
		}
	} else if (ztype == dns_zone_redirect) {
		dns_zone_setnotifytype(zone, dns_notifytype_no);

		obj = NULL;
		result = named_config_get(maps, "max-journal-size", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setjournalsize(zone, -1);
		if (cfg_obj_isstring(obj)) {
			const char *str = cfg_obj_asstring(obj);
			if (strcasecmp(str, "unlimited") == 0) {
				journal_size = DNS_JOURNAL_SIZE_MAX;
			} else {
				INSIST(strcasecmp(str, "default") == 0);
				journal_size = -1;
			}
		} else {
			journal_size = (uint32_t)cfg_obj_asuint64(obj);
		}
		dns_zone_setjournalsize(zone, journal_size);
	}

	if (use_kasp) {
		maxttl = dns_kasp_zonemaxttl(dns_zone_getkasp(zone), false);
	} else {
		obj = NULL;
		result = named_config_get(maps, "max-zone-ttl", &obj);
		if (result == ISC_R_SUCCESS) {
			if (cfg_obj_isduration(obj)) {
				maxttl = cfg_obj_asduration(obj);
			}
		}
	}
	dns_zone_setmaxttl(zone, maxttl);
	if (raw != NULL) {
		dns_zone_setmaxttl(raw, maxttl);
	}

	/*
	 * Configure update-related options.  These apply to
	 * primary servers only.
	 */
	if (ztype == dns_zone_primary) {
		dns_acl_t *updateacl;

		CHECK(configure_zone_acl(zconfig, vconfig, config, allow_update,
					 ac, mayberaw, dns_zone_setupdateacl,
					 dns_zone_clearupdateacl));

		updateacl = dns_zone_getupdateacl(mayberaw);
		if (updateacl != NULL && dns_acl_isinsecure(updateacl)) {
			isc_log_write(DNS_LOGCATEGORY_SECURITY,
				      NAMED_LOGMODULE_SERVER, ISC_LOG_WARNING,
				      "zone '%s' allows unsigned updates "
				      "from remote hosts, which is insecure",
				      zname);
		}

		CHECK(configure_zone_ssutable(zoptions, toptions, mayberaw,
					      zname));
	}

	/*
	 * Configure DNSSEC signing. These apply to primary zones or zones that
	 * use inline-signing (raw != NULL).
	 */
	if (ztype == dns_zone_primary || raw != NULL) {
		if (use_kasp) {
			int seconds;

			if (dns_kasp_nsec3(kasp)) {
				result = dns_zone_setnsec3param(
					zone, 1, dns_kasp_nsec3flags(kasp),
					dns_kasp_nsec3iter(kasp),
					dns_kasp_nsec3saltlen(kasp), NULL, true,
					false);
			} else {
				result = dns_zone_setnsec3param(
					zone, 0, 0, 0, 0, NULL, true, false);
			}
			INSIST(result == ISC_R_SUCCESS);

			seconds = (uint32_t)dns_kasp_sigvalidity_dnskey(kasp);
			dns_zone_setkeyvalidityinterval(zone, seconds);

			seconds = (uint32_t)dns_kasp_sigvalidity(kasp);
			dns_zone_setsigvalidityinterval(zone, seconds);

			seconds = (uint32_t)dns_kasp_sigrefresh(kasp);
			dns_zone_setsigresigninginterval(zone, seconds);
		}

		obj = NULL;
		result = named_config_get(maps, "key-directory", &obj);
		if (result == ISC_R_SUCCESS) {
			filename = cfg_obj_asstring(obj);
			dns_zone_setkeydirectory(zone, filename);
		}
		/* Also save a reference to the keystore list. */
		dns_zone_setkeystores(zone, keystorelist);

		obj = NULL;
		result = named_config_get(maps, "sig-signing-signatures", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setsignatures(zone, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "sig-signing-nodes", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setnodes(zone, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "sig-signing-type", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setprivatetype(zone, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "dnssec-loadkeys-interval",
					  &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		CHECK(dns_zone_setrefreshkeyinterval(zone,
						     cfg_obj_asuint32(obj)));
	}

	if (ztype == dns_zone_secondary || ztype == dns_zone_mirror) {
		CHECK(configure_zone_acl(zconfig, vconfig, config,
					 allow_update_forwarding, ac, mayberaw,
					 dns_zone_setforwardacl,
					 dns_zone_clearforwardacl));
	}

	/*%
	 * Configure parental agents, applies to primary and secondary zones.
	 */
	if (ztype == dns_zone_primary || ztype == dns_zone_secondary) {
		const cfg_obj_t *parentals = NULL;
		(void)named_config_get(nooptions, "parental-agents",
				       &parentals);
		if (parentals != NULL) {
			dns_ipkeylist_t ipkl;
			dns_ipkeylist_init(&ipkl);
			CHECK(named_config_getipandkeylist(config, parentals,
							   mctx, &ipkl));
			dns_zone_setparentals(zone, ipkl.addrs, ipkl.sources,
					      ipkl.keys, ipkl.tlss, ipkl.count);
			dns_ipkeylist_clear(mctx, &ipkl);
		} else {
			dns_zone_setparentals(zone, NULL, NULL, NULL, NULL, 0);
		}

		obj = NULL;
		result = named_config_get(maps, "checkds", &obj);
		if (result == ISC_R_SUCCESS) {
			if (cfg_obj_isboolean(obj)) {
				if (cfg_obj_asboolean(obj)) {
					checkdstype = dns_checkdstype_yes;
				} else {
					checkdstype = dns_checkdstype_no;
				}
			} else {
				const char *str = cfg_obj_asstring(obj);
				if (strcasecmp(str, "explicit") == 0) {
					checkdstype = dns_checkdstype_explicit;
				} else {
					UNREACHABLE();
				}
			}
		} else if (parentals != NULL) {
			checkdstype = dns_checkdstype_explicit;
		} else {
			checkdstype = dns_checkdstype_yes;
		}
		if (raw != NULL) {
			dns_zone_setcheckdstype(raw, dns_checkdstype_no);
		}
		dns_zone_setcheckdstype(zone, checkdstype);
	}

	/*%
	 * Configure primary zone functionality.
	 */
	if (ztype == dns_zone_primary) {
		obj = NULL;
		result = named_config_get(maps, "check-wildcard", &obj);
		if (result == ISC_R_SUCCESS) {
			check = cfg_obj_asboolean(obj);
		} else {
			check = false;
		}
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_CHECKWILDCARD, check);

		obj = NULL;
		result = named_config_get(maps, "check-dup-records", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dupcheck = cfg_obj_asstring(obj);
		if (strcasecmp(dupcheck, "warn") == 0) {
			fail = false;
			check = true;
		} else if (strcasecmp(dupcheck, "fail") == 0) {
			fail = check = true;
		} else if (strcasecmp(dupcheck, "ignore") == 0) {
			fail = check = false;
		} else {
			UNREACHABLE();
		}
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_CHECKDUPRR, check);
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_CHECKDUPRRFAIL, fail);

		obj = NULL;
		result = named_config_get(maps, "check-mx", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (strcasecmp(cfg_obj_asstring(obj), "warn") == 0) {
			fail = false;
			check = true;
		} else if (strcasecmp(cfg_obj_asstring(obj), "fail") == 0) {
			fail = check = true;
		} else if (strcasecmp(cfg_obj_asstring(obj), "ignore") == 0) {
			fail = check = false;
		} else {
			UNREACHABLE();
		}
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_CHECKMX, check);
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_CHECKMXFAIL, fail);

		obj = NULL;
		result = named_config_get(maps, "check-integrity", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_CHECKINTEGRITY,
				   cfg_obj_asboolean(obj));

		obj = NULL;
		result = named_config_get(maps, "check-mx-cname", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (strcasecmp(cfg_obj_asstring(obj), "warn") == 0) {
			warn = true;
			ignore = false;
		} else if (strcasecmp(cfg_obj_asstring(obj), "fail") == 0) {
			warn = ignore = false;
		} else if (strcasecmp(cfg_obj_asstring(obj), "ignore") == 0) {
			warn = ignore = true;
		} else {
			UNREACHABLE();
		}
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_WARNMXCNAME, warn);
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_IGNOREMXCNAME, ignore);

		obj = NULL;
		result = named_config_get(maps, "check-srv-cname", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (strcasecmp(cfg_obj_asstring(obj), "warn") == 0) {
			warn = true;
			ignore = false;
		} else if (strcasecmp(cfg_obj_asstring(obj), "fail") == 0) {
			warn = ignore = false;
		} else if (strcasecmp(cfg_obj_asstring(obj), "ignore") == 0) {
			warn = ignore = true;
		} else {
			UNREACHABLE();
		}
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_WARNSRVCNAME, warn);
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_IGNORESRVCNAME,
				   ignore);

		obj = NULL;
		result = named_config_get(maps, "serial-update-method", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		if (strcasecmp(cfg_obj_asstring(obj), "unixtime") == 0) {
			dns_zone_setserialupdatemethod(
				zone, dns_updatemethod_unixtime);
		} else if (strcasecmp(cfg_obj_asstring(obj), "date") == 0) {
			dns_zone_setserialupdatemethod(zone,
						       dns_updatemethod_date);
		} else {
			dns_zone_setserialupdatemethod(
				zone, dns_updatemethod_increment);
		}
	}

	/*
	 * Configure secondary zone functionality.
	 */
	switch (ztype) {
	case dns_zone_mirror:
		/*
		 * Disable outgoing zone transfers for mirror zones unless they
		 * are explicitly enabled by zone configuration.
		 */
		obj = NULL;
		(void)named_config_get(nooptions, "allow-transfer", &obj);
		if (obj == NULL) {
			dns_acl_t *none;
			CHECK(dns_acl_none(mctx, &none));
			dns_zone_setxfracl(zone, none);
			dns_acl_detach(&none);
		}
		FALLTHROUGH;
	case dns_zone_secondary:
	case dns_zone_stub:
	case dns_zone_redirect:
		count = 0;
		obj = NULL;
		(void)named_config_get(nooptions, "primaries", &obj);
		if (obj == NULL) {
			(void)named_config_get(nooptions, "masters", &obj);
		}

		/*
		 * Use the built-in primary server list if one was not
		 * explicitly specified and this is a root zone mirror.
		 */
		if (obj == NULL && ztype == dns_zone_mirror &&
		    dns_name_equal(dns_zone_getorigin(zone), dns_rootname))
		{
			result = named_config_getremotesdef(
				named_g_defaultconfig, "remote-servers",
				DEFAULT_IANA_ROOT_ZONE_PRIMARIES, &obj);
			CHECK(result);
		}
		if (obj != NULL) {
			dns_ipkeylist_t ipkl;
			dns_ipkeylist_init(&ipkl);

			CHECK(named_config_getipandkeylist(config, obj, mctx,
							   &ipkl));
			dns_zone_setprimaries(mayberaw, ipkl.addrs,
					      ipkl.sources, ipkl.keys,
					      ipkl.tlss, ipkl.count);
			count = ipkl.count;
			dns_ipkeylist_clear(mctx, &ipkl);
		} else {
			dns_zone_setprimaries(mayberaw, NULL, NULL, NULL, NULL,
					      0);
		}

		multi = false;
		if (count > 1) {
			obj = NULL;
			result = named_config_get(maps, "multi-master", &obj);
			INSIST(result == ISC_R_SUCCESS && obj != NULL);
			multi = cfg_obj_asboolean(obj);
		}
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_MULTIMASTER, multi);

		obj = NULL;
		result = named_config_get(maps, "min-transfer-rate-in", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		uint32_t traffic_bytes =
			cfg_obj_asuint32(cfg_tuple_get(obj, "traffic_bytes"));
		uint32_t time_minutes =
			cfg_obj_asuint32(cfg_tuple_get(obj, "time_minutes"));
		dns_zone_setminxfrratein(mayberaw, traffic_bytes,
					 transferinsecs ? time_minutes
							: time_minutes * 60);

		obj = NULL;
		result = named_config_get(maps, "max-transfer-time-in", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setmaxxfrin(
			mayberaw, transferinsecs ? cfg_obj_asuint32(obj)
						 : cfg_obj_asuint32(obj) * 60);

		obj = NULL;
		result = named_config_get(maps, "max-transfer-idle-in", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setidlein(mayberaw,
				   transferinsecs ? cfg_obj_asuint32(obj)
						  : cfg_obj_asuint32(obj) * 60);

		obj = NULL;
		result = named_config_get(maps, "max-refresh-time", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setmaxrefreshtime(mayberaw, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "min-refresh-time", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setminrefreshtime(mayberaw, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "max-retry-time", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setmaxretrytime(mayberaw, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "min-retry-time", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setminretrytime(mayberaw, cfg_obj_asuint32(obj));

		obj = NULL;
		result = named_config_get(maps, "transfer-source", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setxfrsource4(mayberaw, cfg_obj_assockaddr(obj));

		obj = NULL;
		result = named_config_get(maps, "transfer-source-v6", &obj);
		INSIST(result == ISC_R_SUCCESS && obj != NULL);
		dns_zone_setxfrsource6(mayberaw, cfg_obj_assockaddr(obj));

		obj = NULL;
		(void)named_config_get(maps, "try-tcp-refresh", &obj);
		dns_zone_setoption(mayberaw, DNS_ZONEOPT_TRYTCPREFRESH,
				   cfg_obj_asboolean(obj));
		break;

	case dns_zone_staticstub:
		CHECK(configure_staticstub(zoptions, toptions, zone, zname,
					   default_dbtype));
		break;

	default:
		break;
	}

	result = ISC_R_SUCCESS;

cleanup:
	if (kasp != NULL) {
		dns_kasp_detach(&kasp);
	}
	return result;
}

/*
 * Set up a DLZ zone as writeable
 */
isc_result_t
named_zone_configure_writeable_dlz(dns_dlzdb_t *dlzdatabase, dns_zone_t *zone,
				   dns_rdataclass_t rdclass, dns_name_t *name) {
	dns_db_t *db = NULL;
	isc_result_t result;

	dns_zone_settype(zone, dns_zone_dlz);
	result = dns_sdlz_setdb(dlzdatabase, rdclass, name, &db);
	if (result != ISC_R_SUCCESS) {
		return result;
	}
	result = dns_zone_dlzpostload(zone, db);
	dns_db_detach(&db);
	return result;
}

bool
named_zone_reusable(dns_zone_t *zone, const cfg_obj_t *zconfig,
		    const cfg_obj_t *vconfig, const cfg_obj_t *config,
		    dns_kasplist_t *kasplist) {
	const cfg_obj_t *zoptions = NULL;
	const cfg_obj_t *toptions = NULL;
	const cfg_obj_t *obj = NULL;
	const char *cfilename = NULL;
	const char *zfilename = NULL;
	dns_zone_t *raw = NULL;
	bool has_raw, inline_signing;
	dns_zonetype_t ztype;

	zoptions = cfg_tuple_get(zconfig, "options");
	toptions = named_zone_templateopts(config, zoptions);

	/*
	 * We always reconfigure a static-stub zone for simplicity, assuming
	 * the amount of data to be loaded is small.
	 */
	if (zonetype_fromconfig(zoptions, toptions) == dns_zone_staticstub) {
		dns_zone_log(zone, ISC_LOG_DEBUG(1),
			     "not reusable: staticstub");
		return false;
	}

	/* If there's a raw zone, use that for filename and type comparison */
	dns_zone_getraw(zone, &raw);
	if (raw != NULL) {
		zfilename = dns_zone_getfile(raw);
		ztype = dns_zone_gettype(raw);
		dns_zone_detach(&raw);
		has_raw = true;
	} else {
		zfilename = dns_zone_getfile(zone);
		ztype = dns_zone_gettype(zone);
		has_raw = false;
	}

	inline_signing = named_zone_inlinesigning(zconfig, vconfig, config,
						  kasplist);
	if (!inline_signing && has_raw) {
		dns_zone_log(zone, ISC_LOG_DEBUG(1),
			     "not reusable: old zone was inline-signing");
		return false;
	} else if (inline_signing && !has_raw) {
		dns_zone_log(zone, ISC_LOG_DEBUG(1),
			     "not reusable: old zone was not inline-signing");
		return false;
	}

	if (zonetype_fromconfig(zoptions, toptions) != ztype) {
		dns_zone_log(zone, ISC_LOG_DEBUG(1),
			     "not reusable: type mismatch");
		return false;
	}

	obj = NULL;
	(void)named_config_findopt(zoptions, toptions, "file", &obj);
	if (obj != NULL) {
		cfilename = cfg_obj_asstring(obj);
	} else {
		cfilename = NULL;
	}
	if (!((cfilename == NULL && zfilename == NULL) ||
	      (cfilename != NULL && zfilename != NULL &&
	       strcmp(cfilename, zfilename) == 0)))
	{
		dns_zone_log(zone, ISC_LOG_DEBUG(1),
			     "not reusable: filename mismatch");
		return false;
	}

	return true;
}

bool
named_zone_inlinesigning(const cfg_obj_t *zconfig, const cfg_obj_t *vconfig,
			 const cfg_obj_t *config, dns_kasplist_t *kasplist) {
	const cfg_obj_t *maps[5] = { 0 }, *noopts[3] = { 0 };
	const cfg_obj_t *signing = NULL;
	const cfg_obj_t *policy = NULL;
	const cfg_obj_t *toptions = NULL;
	dns_kasp_t *kasp = NULL;
	isc_result_t res;
	bool inline_signing = false;
	int i = 0;

	noopts[i] = maps[i] = cfg_tuple_get(zconfig, "options");
	i++;

	if (config != NULL) {
		toptions = named_zone_templateopts(config, maps[0]);
		if (toptions != NULL) {
			noopts[i] = maps[i] = toptions;
			i++;
		}
	}

	noopts[i] = NULL;
	if (vconfig != NULL) {
		maps[i++] = cfg_tuple_get(vconfig, "options");
	}
	if (config != NULL) {
		const cfg_obj_t *options = NULL;
		(void)cfg_map_get(config, "options", &options);
		if (options != NULL) {
			maps[i++] = options;
		}
	}
	maps[i] = NULL;

	/* Check the value in dnssec-policy. */
	policy = NULL;
	res = named_config_get(maps, "dnssec-policy", &policy);
	/* If no dnssec-policy found, then zone is not using inline-signing. */
	if (res != ISC_R_SUCCESS ||
	    strcmp(cfg_obj_asstring(policy), "none") == 0)
	{
		return false;
	}

	/* Lookup the policy. */
	res = dns_kasplist_find(kasplist, cfg_obj_asstring(policy), &kasp);
	if (res != ISC_R_SUCCESS) {
		return false;
	}

	inline_signing = dns_kasp_inlinesigning(kasp);
	dns_kasp_detach(&kasp);

	/*
	 * The zone option 'inline-signing' may override the value in
	 * dnssec-policy. This is a zone-only option, so look in the
	 * zone and template blocks only.
	 */
	res = named_config_get(noopts, "inline-signing", &signing);
	if (res == ISC_R_SUCCESS && cfg_obj_isboolean(signing)) {
		return cfg_obj_asboolean(signing);
	}

	return inline_signing;
}

const cfg_obj_t *
named_zone_templateopts(const cfg_obj_t *config, const cfg_obj_t *zoptions) {
	const cfg_obj_t *templates = NULL;
	const cfg_obj_t *obj = NULL;

	(void)cfg_map_get(config, "template", &templates);
	(void)cfg_map_get(zoptions, "template", &obj);
	if (obj != NULL && templates != NULL) {
		const char *tmplname = cfg_obj_asstring(obj);
		CFG_LIST_FOREACH (templates, e) {
			const cfg_obj_t *t = cfg_tuple_get(cfg_listelt_value(e),
							   "name");
			if (strcasecmp(cfg_obj_asstring(t), tmplname) == 0) {
				return cfg_tuple_get(cfg_listelt_value(e),
						     "options");
			}
		}
	}

	return NULL;
}
