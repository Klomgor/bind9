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
#include <stdlib.h>

#include <isc/attributes.h>
#include <isc/buffer.h>
#include <isc/commandline.h>
#include <isc/file.h>
#include <isc/getaddresses.h>
#include <isc/lib.h>
#include <isc/log.h>
#include <isc/loop.h>
#include <isc/managers.h>
#include <isc/mem.h>
#include <isc/net.h>
#include <isc/netmgr.h>
#include <isc/random.h>
#include <isc/refcount.h>
#include <isc/result.h>
#include <isc/stdtime.h>
#include <isc/string.h>
#include <isc/thread.h>
#include <isc/util.h>

#include <dns/lib.h>
#include <dns/name.h>

#include <isccc/alist.h>
#include <isccc/base64.h>
#include <isccc/cc.h>
#include <isccc/ccmsg.h>
#include <isccc/sexpr.h>
#include <isccc/types.h>
#include <isccc/util.h>

#include <isccfg/namedconf.h>

#include "util.h"

#define SERVERADDRS  10
#define RNDC_TIMEOUT 60 * 1000

bool verbose;

static const char *admin_conffile = NULL;
static const char *admin_keyfile = NULL;
static const char *version = PACKAGE_VERSION;
static const char *servername = NULL;
static isc_sockaddr_t serveraddrs[SERVERADDRS];
static isc_sockaddr_t local4, local6;
static bool local4set = false, local6set = false;
static int nserveraddrs;
static int currentaddr = 0;
static unsigned int remoteport = 0;
static isc_buffer_t *databuf = NULL;
static isccc_ccmsg_t rndc_ccmsg;
static uint32_t algorithm;
static isccc_region_t secret;
static bool failed = false;
static bool c_flag = false;
static isc_mem_t *rndc_mctx = NULL;
static char *command = NULL;
static char *args = NULL;
static uint32_t serial;
static bool quiet = false;
static bool showresult = false;
static int32_t timeout = RNDC_TIMEOUT;

static void
rndc_startconnect(isc_sockaddr_t *addr);

ISC_NORETURN static void
usage(int status);

static void
usage(int status) {
	fprintf(stderr, "\
Usage: %s [-b address] [-c config] [-s server] [-p port]\n\
	[-k key-file ] [-y key] [-r] [-V] [-4 | -6] command\n\
\n\
command is one of the following:\n\
\n\
  addzone zone [class [view]] { zone-options }\n\
		Add zone to given view. Requires allow-new-zones option.\n\
  closelogs     Close currently open log files.\n\
  delzone [-clean] zone [class [view]]\n\
		Removes zone from given view.\n\
  dnssec -checkds [-key id [-alg algorithm]] [-when time] (published|withdrawn) zone [class [view]]\n\
		Mark the DS record for the KSK of the given zone as seen\n\
		in the parent.  If the zone has multiple KSKs, select a\n\
		specific key by providing the keytag with -key id and\n\
		optionally the key's algorithm with -alg algorithm.\n\
		Requires the zone to have a dnssec-policy.\n\
  dnssec -rollover -key id [-alg algorithm] [-when time] zone [class [view]]\n\
		Rollover key with id of the given zone. Requires the zone\n\
		to have a dnssec-policy.\n\
  dnssec -status zone [class [view]]\n\
		Show the DNSSEC signing state for the specified zone.\n\
		Requires the zone to have a dnssec-policy.\n\
  dnstap -reopen\n\
		Close, truncate and re-open the DNSTAP output file.\n\
  dnstap -roll [count]\n\
		Close, rename and re-open the DNSTAP output file(s).\n\
  dumpdb [-all|-cache|-zones|-adb|-bad|-expired|-fail] [view ...]\n\
		Dump cache(s) to the dump file (named_dump.db).\n\
  fetchlimit [view]\n\
		Show servers and domains currently rate-limited to fetch limits.\n\
  flush         Flushes all of the server's caches.\n\
  flush [view]	Flushes the server's cache for a view.\n\
  flushname name [view]\n\
		Flush the given name from the server's cache(s)\n\
  flushtree name [view]\n\
		Flush all names under the given name from the server's cache(s)\n\
  freeze	Suspend updates to all dynamic zones.\n\
  freeze zone [class [view]]\n\
		Suspend updates to a dynamic zone.\n\
  halt		Stop the server without saving pending updates.\n\
  halt -p	Stop the server without saving pending updates reporting\n\
		process id.\n\
  skr -import file zone [class [view]]\n\
		Import a SKR file for the specified zone, for offline KSK\n\
		signing.\n\
  loadkeys zone [class [view]]\n\
		Update keys without signing immediately.\n\
  managed-keys refresh [class [view]]\n\
		Check trust anchor for RFC 5011 key changes\n\
  managed-keys status [class [view]]\n\
		Display RFC 5011 managed keys information\n\
  managed-keys sync [class [view]]\n\
		Write RFC 5011 managed keys to disk\n\
  memprof [ on | off | dump ]\n\
		Enable / disable memory profiling or dump the profile.\n\
		Requires named to built with jemalloc and run with the relevant\n\
		MALLOC_CONF environment variables.\n\
  modzone zone [class [view]] { zone-options }\n\
		Modify a zone's configuration.\n\
		Requires allow-new-zones option.\n\
  notify zone [class [view]]\n\
		Resend NOTIFY messages for the zone.\n\
  notrace	Set debugging level to 0.\n\
  nta -dump\n\
		List all negative trust anchors.\n\
  nta [-lifetime duration] [-force] domain [view]\n\
		Set a negative trust anchor, disabling DNSSEC validation\n\
		for the given domain.\n\
		Using -lifetime specifies the duration of the NTA, up\n\
		to one week.\n\
		Using -force prevents the NTA from expiring before its\n\
		full lifetime, even if the domain can validate sooner.\n\
  nta -remove domain [view]\n\
		Remove a negative trust anchor, re-enabling validation\n\
		for the given domain.\n\
  querylog [ on | off ]\n\
		Enable / disable query logging.\n\
  reconfig	Reload configuration file and new zones only.\n\
  recursing	Dump the queries that are currently recursing (named.recursing)\n\
  refresh zone [class [view]]\n\
		Schedule immediate maintenance for a zone.\n\
  reload	Reload configuration file and zones.\n\
  reload zone [class [view]]\n\
		Reload a single zone.\n\
  reset-stats <counter-name ...>\n\
		Reset the requested statistics counter(s).\n\
  responselog [ on | off ]\n\
		Enable / disable response logging.\n\
  retransfer zone [class [view]]\n\
		Retransfer a single zone without checking serial number.\n\
  scan		Scan available network interfaces for changes.\n\
  secroots [view ...]\n\
		Write security roots to the secroots file.\n\
  serve-stale [ on | off | reset | status ] [class [view]]\n\
		Control whether stale answers are returned\n\
  showzone zone [class [view]]\n\
		Print a zone's configuration.\n\
  sign zone [class [view]]\n\
		Update zone keys, and sign as needed.\n\
  signing -clear all zone [class [view]]\n\
		Remove the private records for all keys that have\n\
		finished signing the given zone.\n\
  signing -clear <keyid>/<algorithm> zone [class [view]]\n\
		Remove the private record that indicating the given key\n\
		has finished signing the given zone.\n\
  signing -list zone [class [view]]\n\
		List the private records showing the state of DNSSEC\n\
		signing in the given zone.\n\
  signing -nsec3param hash flags iterations salt zone [class [view]]\n\
		Add NSEC3 chain to zone if already signed.\n\
		Prime zone with NSEC3 chain if not yet signed.\n\
  signing -nsec3param none zone [class [view]]\n\
		Remove NSEC3 chains from zone.\n\
  signing -serial <value> zone [class [view]]\n\
		Set the zones's serial to <value>.\n\
  stats		Write server statistics to the statistics file.\n\
  status	Display status of the server.\n\
  stop		Save pending updates to master files and stop the server.\n\
  stop -p	Save pending updates to master files and stop the server\n\
		reporting process id.\n\
  sync [-clean]	Dump changes to all dynamic zones to disk, and optionally\n\
		remove their journal files.\n\
  sync [-clean] zone [class [view]]\n\
		Dump a single zone's changes to disk, and optionally\n\
		remove its journal file.\n\
  tcp-timeouts	Display the tcp-*-timeout option values\n\
  tcp-timeouts initial idle keepalive advertised primaries\n\
		Update the tcp-*-timeout option values\n\
  thaw		Enable updates to all dynamic zones and reload them.\n\
  thaw zone [class [view]]\n\
		Enable updates to a frozen dynamic zone and reload it.\n\
  trace		Increment debugging level by one.\n\
  trace level	Change the debugging level.\n\
  validation [ on | off | status ] [view]\n\
		Enable / disable DNSSEC validation.\n\
  zonestatus zone [class [view]]\n\
		Display the current status of a zone.\n\
\n\
Version: %s\n",
		isc_commandline_progname, version);

	exit(status);
}

#define CMDLINE_FLAGS "46b:c:hk:Mmp:qrs:t:Vy:"

static void
preparse_args(int argc, char **argv) {
	bool ipv4only = false, ipv6only = false;
	int ch;

	while ((ch = isc_commandline_parse(argc, argv, CMDLINE_FLAGS)) != -1) {
		switch (ch) {
		case '4':
			if (ipv6only) {
				fatal("only one of -4 and -6 allowed");
			}
			ipv4only = true;
			break;
		case '6':
			if (ipv4only) {
				fatal("only one of -4 and -6 allowed");
			}
			ipv6only = true;
			break;
		default:
			break;
		}
	}

	isc_commandline_reset = true;
	isc_commandline_index = 1;
}

static void
get_addresses(const char *host, in_port_t port) {
	isc_result_t result;
	int found = 0, count;

	REQUIRE(host != NULL);

	count = SERVERADDRS - nserveraddrs;
	result = isc_getaddresses(host, port, &serveraddrs[nserveraddrs], count,
				  &found);
	nserveraddrs += found;

	if (result != ISC_R_SUCCESS) {
		fatal("couldn't get address for '%s': %s", host,
		      isc_result_totext(result));
	}
	INSIST(nserveraddrs > 0);
}

static void
rndc_senddone(isc_nmhandle_t *handle ISC_ATTR_UNUSED, isc_result_t result,
	      void *arg ISC_ATTR_UNUSED) {
	if (result != ISC_R_SUCCESS) {
		fatal("send failed: %s", isc_result_totext(result));
	}
}

static void
rndc_recvdone(isc_nmhandle_t *handle, isc_result_t result, void *arg) {
	isccc_ccmsg_t *ccmsg = (isccc_ccmsg_t *)arg;
	isccc_sexpr_t *response = NULL;
	isccc_sexpr_t *data = NULL;
	isccc_region_t source;
	char *errormsg = NULL;
	char *textmsg = NULL;

	REQUIRE(handle != NULL);
	REQUIRE(ccmsg != NULL);

	if (result == ISC_R_EOF) {
		fatal("connection to remote host closed.\n"
		      "* This may indicate that the\n"
		      "* remote server is using an older\n"
		      "* version of the command protocol,\n"
		      "* this host is not authorized to connect,\n"
		      "* the clocks are not synchronized,\n"
		      "* the key signing algorithm is incorrect,\n"
		      "* or the key is invalid.");
	} else if (result != ISC_R_SUCCESS) {
		fatal("recv failed: %s", isc_result_totext(result));
	}

	isccc_ccmsg_toregion(ccmsg, &source);

	DO("parse message",
	   isccc_cc_fromwire(&source, &response, algorithm, &secret));

	data = isccc_alist_lookup(response, "_data");
	if (!isccc_alist_alistp(data)) {
		fatal("bad or missing data section in response");
	}
	result = isccc_cc_lookupstring(data, "err", &errormsg);
	if (result == ISC_R_SUCCESS) {
		failed = true;
		fprintf(stderr, "%s: '%s' failed: %s\n",
			isc_commandline_progname, command, errormsg);
	} else if (result != ISC_R_NOTFOUND) {
		fprintf(stderr, "%s: parsing response failed: %s\n",
			isc_commandline_progname, isc_result_totext(result));
	}

	result = isccc_cc_lookupstring(data, "text", &textmsg);
	if (result == ISC_R_SUCCESS) {
		if ((!quiet || failed) && strlen(textmsg) != 0U) {
			fprintf(failed ? stderr : stdout, "%s\n", textmsg);
		}
	} else if (result != ISC_R_NOTFOUND) {
		fprintf(stderr, "%s: parsing response failed: %s\n",
			isc_commandline_progname, isc_result_totext(result));
	}

	if (showresult) {
		isc_result_t eresult;

		result = isccc_cc_lookupuint32(data, "result", &eresult);
		if (result == ISC_R_SUCCESS) {
			printf("%s %u\n", isc_result_toid(eresult), eresult);
		} else {
			printf("NONE -1\n");
		}
	}

	isccc_sexpr_free(&response);

	isccc_ccmsg_disconnect(ccmsg);
	isc_loopmgr_shutdown();
}

static void
rndc_recvnonce(isc_nmhandle_t *handle ISC_ATTR_UNUSED, isc_result_t result,
	       void *arg) {
	isccc_ccmsg_t *ccmsg = (isccc_ccmsg_t *)arg;
	isccc_sexpr_t *response = NULL;
	isccc_sexpr_t *_ctrl = NULL;
	isccc_region_t source;
	uint32_t nonce;
	isccc_sexpr_t *request = NULL;
	isccc_time_t now = isc_stdtime_now();
	isc_region_t r;
	isccc_sexpr_t *data = NULL;
	isc_buffer_t b;

	REQUIRE(ccmsg != NULL);

	if (result == ISC_R_EOF) {
		fatal("connection to remote host closed.\n"
		      "* This may indicate that the\n"
		      "* remote server is using an older\n"
		      "* version of the command protocol,\n"
		      "* this host is not authorized to connect,\n"
		      "* the clocks are not synchronized,\n"
		      "* the key signing algorithm is incorrect\n"
		      "* or the key is invalid.");
	} else if (result != ISC_R_SUCCESS) {
		fatal("recv failed: %s", isc_result_totext(result));
	}

	isccc_ccmsg_toregion(ccmsg, &source);

	DO("parse message",
	   isccc_cc_fromwire(&source, &response, algorithm, &secret));

	_ctrl = isccc_alist_lookup(response, "_ctrl");
	if (!isccc_alist_alistp(_ctrl)) {
		fatal("bad or missing ctrl section in response");
	}
	nonce = 0;
	if (isccc_cc_lookupuint32(_ctrl, "_nonce", &nonce) != ISC_R_SUCCESS) {
		nonce = 0;
	}

	DO("create message", isccc_cc_createmessage(1, NULL, NULL, ++serial,
						    now, now + 60, &request));
	data = isccc_alist_lookup(request, "_data");
	if (data == NULL) {
		fatal("_data section missing");
	}
	if (isccc_cc_definestring(data, "type", args) == NULL) {
		fatal("out of memory");
	}
	if (nonce != 0) {
		_ctrl = isccc_alist_lookup(request, "_ctrl");
		if (_ctrl == NULL) {
			fatal("_ctrl section missing");
		}
		if (isccc_cc_defineuint32(_ctrl, "_nonce", nonce) == NULL) {
			fatal("out of memory");
		}
	}

	isc_buffer_clear(databuf);
	/* Skip the length field (4 bytes) */
	isc_buffer_add(databuf, 4);

	DO("render message",
	   isccc_cc_towire(request, &databuf, algorithm, &secret));

	isc_buffer_init(&b, databuf->base, 4);
	isc_buffer_putuint32(&b, databuf->used - 4);

	r.base = databuf->base;
	r.length = databuf->used;

	isccc_ccmsg_readmessage(ccmsg, rndc_recvdone, ccmsg);
	isccc_ccmsg_sendmessage(ccmsg, &r, rndc_senddone, NULL);

	isccc_sexpr_free(&response);
	isccc_sexpr_free(&request);
	return;
}

static void
rndc_connected(isc_nmhandle_t *handle, isc_result_t result, void *arg) {
	isccc_ccmsg_t *ccmsg = (isccc_ccmsg_t *)arg;
	char socktext[ISC_SOCKADDR_FORMATSIZE];
	isccc_sexpr_t *request = NULL;
	isccc_sexpr_t *data = NULL;
	isccc_time_t now = isc_stdtime_now();
	isc_region_t r;
	isc_buffer_t b;

	REQUIRE(ccmsg != NULL);

	if (result != ISC_R_SUCCESS) {
		isc_sockaddr_format(&serveraddrs[currentaddr], socktext,
				    sizeof(socktext));
		if (++currentaddr < nserveraddrs) {
			notify("connection failed: %s: %s", socktext,
			       isc_result_totext(result));
			rndc_startconnect(&serveraddrs[currentaddr]);
			return;
		}

		fatal("connect failed: %s: %s", socktext,
		      isc_result_totext(result));
	}

	DO("create message", isccc_cc_createmessage(1, NULL, NULL, ++serial,
						    now, now + 60, &request));
	data = isccc_alist_lookup(request, "_data");
	if (data == NULL) {
		fatal("_data section missing");
	}
	if (isccc_cc_definestring(data, "type", "null") == NULL) {
		fatal("out of memory");
	}

	isc_buffer_clear(databuf);
	/* Skip the length field (4 bytes) */
	isc_buffer_add(databuf, 4);

	DO("render message",
	   isccc_cc_towire(request, &databuf, algorithm, &secret));

	isc_buffer_init(&b, databuf->base, 4);
	isc_buffer_putuint32(&b, databuf->used - 4);

	r.base = databuf->base;
	r.length = databuf->used;

	/* isccc_ccmsg_init() attaches to the handle */
	isccc_ccmsg_init(rndc_mctx, handle, ccmsg);
	isccc_ccmsg_setmaxsize(ccmsg, 1024 * 1024);

	isccc_ccmsg_readmessage(ccmsg, rndc_recvnonce, ccmsg);
	isccc_ccmsg_sendmessage(ccmsg, &r, rndc_senddone, NULL);

	isccc_sexpr_free(&request);
}

static void
rndc_startconnect(isc_sockaddr_t *addr) {
	char socktext[ISC_SOCKADDR_FORMATSIZE];
	isc_sockaddr_t *local = NULL;

	isc_sockaddr_format(addr, socktext, sizeof(socktext));

	notify("using server %s (%s)", servername, socktext);

	switch (isc_sockaddr_pf(addr)) {
	case AF_INET:
		local = &local4;
		break;
	case AF_INET6:
		local = &local6;
		break;
	default:
		UNREACHABLE();
	}

	isc_nm_tcpconnect(local, addr, rndc_connected, &rndc_ccmsg, timeout);
}

static void
rndc_start(void *arg) {
	UNUSED(arg);

	currentaddr = 0;
	rndc_startconnect(&serveraddrs[currentaddr]);
}

static void
parse_config(isc_mem_t *mctx, const char *keyname, cfg_parser_t **pctxp,
	     cfg_obj_t **configp) {
	isc_result_t result;
	const char *conffile = admin_conffile;
	const cfg_obj_t *addresses = NULL;
	const cfg_obj_t *defkey = NULL;
	const cfg_obj_t *options = NULL;
	const cfg_obj_t *servers = NULL;
	const cfg_obj_t *server = NULL;
	const cfg_obj_t *keys = NULL;
	const cfg_obj_t *key = NULL;
	const cfg_obj_t *defport = NULL;
	const cfg_obj_t *secretobj = NULL;
	const cfg_obj_t *algorithmobj = NULL;
	cfg_obj_t *config = NULL;
	const cfg_obj_t *address = NULL;
	const char *secretstr;
	const char *algorithmstr;
	static char secretarray[1024];
	const cfg_type_t *conftype = &cfg_type_rndcconf;
	bool key_only = false;

	if (!isc_file_exists(conffile)) {
		conffile = admin_keyfile;
		conftype = &cfg_type_rndckey;

		if (c_flag) {
			fatal("%s does not exist", admin_conffile);
		}

		if (!isc_file_exists(conffile)) {
			fatal("neither %s nor %s was found", admin_conffile,
			      admin_keyfile);
		}
		key_only = true;
	} else if (!c_flag && isc_file_exists(admin_keyfile)) {
		fprintf(stderr,
			"WARNING: key file (%s) exists, but using "
			"default configuration file (%s)\n",
			admin_keyfile, admin_conffile);
	}

	DO("create parser", cfg_parser_create(mctx, pctxp));

	/*
	 * The parser will output its own errors, so DO() is not used.
	 */
	result = cfg_parse_file(*pctxp, conffile, conftype, &config);
	if (result != ISC_R_SUCCESS) {
		fatal("could not load rndc configuration");
	}

	if (!key_only) {
		(void)cfg_map_get(config, "options", &options);
	}

	if (key_only && servername == NULL) {
		servername = "127.0.0.1";
	} else if (servername == NULL && options != NULL) {
		const cfg_obj_t *defserverobj = NULL;
		(void)cfg_map_get(options, "default-server", &defserverobj);
		if (defserverobj != NULL) {
			servername = cfg_obj_asstring(defserverobj);
		}
	}

	if (servername == NULL) {
		fatal("no server specified and no default");
	}

	if (!key_only) {
		(void)cfg_map_get(config, "server", &servers);
		if (servers != NULL) {
			CFG_LIST_FOREACH (servers, elt) {
				const char *name = NULL;
				server = cfg_listelt_value(elt);
				name = cfg_obj_asstring(
					cfg_map_getname(server));
				if (strcasecmp(name, servername) == 0) {
					break;
				}
				server = NULL;
			}
		}
	}

	/*
	 * Look for the name of the key to use.
	 */
	if (keyname != NULL) {
		/* Was set on command line, do nothing. */
	} else if (server != NULL) {
		DO("get key for server", cfg_map_get(server, "key", &defkey));
		keyname = cfg_obj_asstring(defkey);
	} else if (options != NULL) {
		DO("get default key",
		   cfg_map_get(options, "default-key", &defkey));
		keyname = cfg_obj_asstring(defkey);
	} else if (!key_only) {
		fatal("no key for server and no default");
	}

	/*
	 * Get the key's definition.
	 */
	if (key_only) {
		DO("get key", cfg_map_get(config, "key", &key));
	} else {
		DO("get config key list", cfg_map_get(config, "key", &keys));
		bool match = false;
		CFG_LIST_FOREACH (keys, elt) {
			const char *name = NULL;

			key = cfg_listelt_value(elt);
			name = cfg_obj_asstring(cfg_map_getname(key));
			if (strcasecmp(name, keyname) == 0) {
				match = true;
				break;
			}
		}
		if (!match) {
			fatal("no key definition for name %s", keyname);
		}
	}
	(void)cfg_map_get(key, "secret", &secretobj);
	(void)cfg_map_get(key, "algorithm", &algorithmobj);
	if (secretobj == NULL || algorithmobj == NULL) {
		fatal("key must have algorithm and secret");
	}

	secretstr = cfg_obj_asstring(secretobj);
	algorithmstr = cfg_obj_asstring(algorithmobj);

	if (strcasecmp(algorithmstr, "hmac-md5") == 0) {
		algorithm = ISCCC_ALG_HMACMD5;
	} else if (strcasecmp(algorithmstr, "hmac-sha1") == 0) {
		algorithm = ISCCC_ALG_HMACSHA1;
	} else if (strcasecmp(algorithmstr, "hmac-sha224") == 0) {
		algorithm = ISCCC_ALG_HMACSHA224;
	} else if (strcasecmp(algorithmstr, "hmac-sha256") == 0) {
		algorithm = ISCCC_ALG_HMACSHA256;
	} else if (strcasecmp(algorithmstr, "hmac-sha384") == 0) {
		algorithm = ISCCC_ALG_HMACSHA384;
	} else if (strcasecmp(algorithmstr, "hmac-sha512") == 0) {
		algorithm = ISCCC_ALG_HMACSHA512;
	} else {
		fatal("unsupported algorithm: %s", algorithmstr);
	}

	secret.rstart = (unsigned char *)secretarray;
	secret.rend = (unsigned char *)secretarray + sizeof(secretarray);
	DO("decode base64 secret", isccc_base64_decode(secretstr, &secret));
	secret.rend = secret.rstart;
	secret.rstart = (unsigned char *)secretarray;

	/*
	 * Find the port to connect to.
	 */
	if (remoteport != 0) {
		/* Was set on command line, do nothing. */
	} else {
		if (server != NULL) {
			(void)cfg_map_get(server, "port", &defport);
		}
		if (defport == NULL && options != NULL) {
			(void)cfg_map_get(options, "default-port", &defport);
		}
	}
	if (defport != NULL) {
		remoteport = cfg_obj_asuint32(defport);
		if (remoteport > 65535 || remoteport == 0) {
			fatal("port %u out of range", remoteport);
		}
	} else if (remoteport == 0) {
		remoteport = NS_CONTROL_PORT;
	}

	if (server != NULL) {
		result = cfg_map_get(server, "addresses", &addresses);
	} else {
		result = ISC_R_NOTFOUND;
	}
	if (result == ISC_R_SUCCESS) {
		CFG_LIST_FOREACH (addresses, element) {
			isc_sockaddr_t sa;

			address = cfg_listelt_value(element);
			if (!cfg_obj_issockaddr(address)) {
				unsigned int myport;
				const char *name;
				const cfg_obj_t *obj;

				obj = cfg_tuple_get(address, "name");
				name = cfg_obj_asstring(obj);
				obj = cfg_tuple_get(address, "port");
				if (cfg_obj_isuint32(obj)) {
					myport = cfg_obj_asuint32(obj);
					if (myport > UINT16_MAX || myport == 0)
					{
						fatal("port %u out of range",
						      myport);
					}
				} else {
					myport = remoteport;
				}
				if (nserveraddrs < SERVERADDRS) {
					get_addresses(name, (in_port_t)myport);
				} else {
					fprintf(stderr,
						"too many address: "
						"%s: dropped\n",
						name);
				}
				continue;
			}
			sa = *cfg_obj_assockaddr(address);
			if (isc_sockaddr_getport(&sa) == 0) {
				isc_sockaddr_setport(&sa, remoteport);
			}
			if (nserveraddrs < SERVERADDRS) {
				serveraddrs[nserveraddrs++] = sa;
			} else {
				char socktext[ISC_SOCKADDR_FORMATSIZE];

				isc_sockaddr_format(&sa, socktext,
						    sizeof(socktext));
				fprintf(stderr,
					"too many address: %s: dropped\n",
					socktext);
			}
		}
	}

	if (!local4set && server != NULL) {
		address = NULL;
		cfg_map_get(server, "source-address", &address);
		if (address != NULL) {
			local4 = *cfg_obj_assockaddr(address);
			local4set = true;
		}
	}
	if (!local4set && options != NULL) {
		address = NULL;
		cfg_map_get(options, "default-source-address", &address);
		if (address != NULL) {
			local4 = *cfg_obj_assockaddr(address);
			local4set = true;
		}
	}

	if (!local6set && server != NULL) {
		address = NULL;
		cfg_map_get(server, "source-address-v6", &address);
		if (address != NULL) {
			local6 = *cfg_obj_assockaddr(address);
			local6set = true;
		}
	}
	if (!local6set && options != NULL) {
		address = NULL;
		cfg_map_get(options, "default-source-address-v6", &address);
		if (address != NULL) {
			local6 = *cfg_obj_assockaddr(address);
			local6set = true;
		}
	}

	*configp = config;
}

int
main(int argc, char **argv) {
	bool show_final_mem = false;
	isc_logconfig_t *logconfig = NULL;
	cfg_parser_t *pctx = NULL;
	cfg_obj_t *config = NULL;
	const char *keyname = NULL;
	struct in_addr in;
	struct in6_addr in6;
	char *p = NULL;
	size_t argslen;
	int ch;
	int i;

	isc_commandline_init(argc, argv);

	admin_conffile = RNDC_CONFFILE;
	admin_keyfile = RNDC_KEYFILE;

	isc_sockaddr_any(&local4);
	isc_sockaddr_any6(&local6);

	isc_commandline_errprint = false;

	preparse_args(argc, argv);

	while ((ch = isc_commandline_parse(argc, argv, CMDLINE_FLAGS)) != -1) {
		switch (ch) {
		case '4':
			if (isc_net_probeipv4() != ISC_R_SUCCESS) {
				fatal("can't find IPv4 networking");
			}
			isc_net_disableipv6();
			break;
		case '6':
			if (isc_net_probeipv6() != ISC_R_SUCCESS) {
				fatal("can't find IPv6 networking");
			}
			isc_net_disableipv4();
			break;
		case 'b':
			if (inet_pton(AF_INET, isc_commandline_argument, &in) ==
			    1)
			{
				isc_sockaddr_fromin(&local4, &in, 0);
				local4set = true;
			} else if (inet_pton(AF_INET6, isc_commandline_argument,
					     &in6) == 1)
			{
				isc_sockaddr_fromin6(&local6, &in6, 0);
				local6set = true;
			}
			break;

		case 'c':
			admin_conffile = isc_commandline_argument;
			c_flag = true;
			break;

		case 'k':
			admin_keyfile = isc_commandline_argument;
			break;

		case 'M':
			isc_mem_debugging = ISC_MEM_DEBUGTRACE;
			break;

		case 'm':
			show_final_mem = true;
			break;

		case 'p':
			remoteport = atoi(isc_commandline_argument);
			if (remoteport > 65535 || remoteport == 0) {
				fatal("port '%s' out of range",
				      isc_commandline_argument);
			}
			break;

		case 'q':
			quiet = true;
			break;

		case 'r':
			showresult = true;
			break;

		case 's':
			servername = isc_commandline_argument;
			break;

		case 't':
			timeout = strtol(isc_commandline_argument, &p, 10);
			if (*p != '\0' || timeout < 0 || timeout > 86400) {
				fatal("invalid timeout '%s'",
				      isc_commandline_argument);
			}
			timeout *= 1000;
			break;

		case 'V':
			verbose = true;
			break;

		case 'y':
			keyname = isc_commandline_argument;
			break;

		case '?':
			if (isc_commandline_option != '?') {
				fprintf(stderr, "%s: invalid argument -%c\n",
					isc_commandline_progname,
					isc_commandline_option);
				usage(1);
			}
			FALLTHROUGH;
		case 'h':
			usage(0);
			break;
		default:
			fprintf(stderr, "%s: unhandled option -%c\n",
				isc_commandline_progname,
				isc_commandline_option);
			exit(EXIT_FAILURE);
		}
	}

	argc -= isc_commandline_index;
	argv += isc_commandline_index;

	if (argv[0] == NULL) {
		usage(1);
	} else {
		command = argv[0];
		if (strcmp(command, "restart") == 0) {
			fatal("'%s' is not implemented", command);
		}
		notify("%s", command);
	}

	serial = isc_random32();

	isc_managers_create(&rndc_mctx, 1);
	isc_loopmgr_setup(rndc_start, NULL);

	isc_nm_setinitialtimeout(timeout);
	isc_nm_setprimariestimeout(timeout);
	isc_nm_setidletimeout(timeout);
	isc_nm_setkeepalivetimeout(timeout);

	logconfig = isc_logconfig_get();
	isc_log_settag(logconfig, isc_commandline_progname);
	isc_log_createandusechannel(
		logconfig, "default_stderr", ISC_LOG_TOFILEDESC, ISC_LOG_INFO,
		ISC_LOGDESTINATION_STDERR,
		ISC_LOG_PRINTTAG | ISC_LOG_PRINTLEVEL, ISC_LOGCATEGORY_DEFAULT,
		ISC_LOGMODULE_DEFAULT);

	parse_config(rndc_mctx, keyname, &pctx, &config);

	isc_buffer_allocate(rndc_mctx, &databuf, 2048);

	/*
	 * Convert argc/argv into a space-delimited command string
	 * similar to what the user might enter in interactive mode
	 * (if that were implemented).
	 */
	argslen = 0;
	for (i = 0; i < argc; i++) {
		argslen += strlen(argv[i]) + 1;
	}

	args = isc_mem_get(rndc_mctx, argslen);

	p = args;
	for (i = 0; i < argc; i++) {
		size_t len = strlen(argv[i]);
		memmove(p, argv[i], len);
		p += len;
		*p++ = ' ';
	}

	p--;
	*p++ = '\0';
	INSIST(p == args + argslen);

	if (nserveraddrs == 0 && servername != NULL) {
		get_addresses(servername, (in_port_t)remoteport);
	}

	isc_loopmgr_run();

	isccc_ccmsg_invalidate(&rndc_ccmsg);

	cfg_obj_destroy(pctx, &config);
	cfg_parser_destroy(&pctx);

	isc_mem_put(rndc_mctx, args, argslen);

	isc_buffer_free(&databuf);

	if (show_final_mem) {
		isc_mem_stats(rndc_mctx, stderr);
	}

	isc_managers_destroy(&rndc_mctx);

	if (failed) {
		return 1;
	}

	return 0;
}
