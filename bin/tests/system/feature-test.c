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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif

#include <isc/crypto.h>
#include <isc/lib.h>
#include <isc/md.h>
#include <isc/mem.h>
#include <isc/net.h>
#include <isc/util.h>

#include <dns/ds.h>
#include <dns/edns.h>
#include <dns/lib.h>

#include <dst/dst.h>

static void
usage(void) {
	fprintf(stderr, "usage: feature-test <arg>\n");
	fprintf(stderr, "args:\n");
	fprintf(stderr, "\t--edns-version\n");
	fprintf(stderr, "\t--enable-dnstap\n");
	fprintf(stderr, "\t--enable-querytrace\n");
	fprintf(stderr, "\t--extended-ds-digest\n");
	fprintf(stderr, "\t--fips-provider\n");
	fprintf(stderr, "\t--gethostname\n");
	fprintf(stderr, "\t--gssapi\n");
	fprintf(stderr, "\t--have-fips-dh\n");
	fprintf(stderr, "\t--have-fips-mode\n");
	fprintf(stderr, "\t--have-geoip2\n");
	fprintf(stderr, "\t--have-json-c\n");
	fprintf(stderr, "\t--have-libxml2\n");
	fprintf(stderr, "\t--ipv6only=no\n");
	fprintf(stderr, "\t--md5\n");
	fprintf(stderr, "\t--rsasha1\n");
	fprintf(stderr, "\t--tsan\n");
	fprintf(stderr, "\t--with-dlz-filesystem\n");
	fprintf(stderr, "\t--with-libidn2\n");
	fprintf(stderr, "\t--with-lmdb\n");
	fprintf(stderr, "\t--with-libnghttp2\n");
	fprintf(stderr, "\t--with-zlib\n");
}

int
main(int argc, char **argv) {
	if (argc != 2) {
		usage();
		return 1;
	}

	if (strcmp(argv[1], "--edns-version") == 0) {
#ifdef DNS_EDNS_VERSION
		printf("%d\n", DNS_EDNS_VERSION);
#else  /* ifdef DNS_EDNS_VERSION */
		printf("0\n");
#endif /* ifdef DNS_EDNS_VERSION */
		return 0;
	}

	if (strcmp(argv[1], "--enable-dnstap") == 0) {
#ifdef HAVE_DNSTAP
		return 0;
#else  /* ifdef HAVE_DNSTAP */
		return 1;
#endif /* ifdef HAVE_DNSTAP */
	}

	if (strcmp(argv[1], "--enable-querytrace") == 0) {
#ifdef WANT_QUERYTRACE
		return 0;
#else  /* ifdef WANT_QUERYTRACE */
		return 1;
#endif /* ifdef WANT_QUERYTRACE */
	}

	if (strcasecmp(argv[1], "--extended-ds-digest") == 0) {
#if defined(DNS_DSDIGEST_SHA256PRIVATE) && defined(DNS_DSDIGEST_SHA384PRIVATE)
		return 0;
#else
		return 1;
#endif
	}

	if (strcasecmp(argv[1], "--fips-provider") == 0) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
		OSSL_PROVIDER *fips = OSSL_PROVIDER_load(NULL, "fips");
		if (fips != NULL) {
			OSSL_PROVIDER_unload(fips);
		}
		return fips != NULL ? 0 : 1;
#else
		return 1;
#endif
	}

	if (strcmp(argv[1], "--gethostname") == 0) {
		char hostname[_POSIX_HOST_NAME_MAX + 1];
		int n;

		n = gethostname(hostname, sizeof(hostname));
		if (n == -1) {
			perror("gethostname");
			return 1;
		}
		fprintf(stdout, "%s\n", hostname);
		return 0;
	}

	if (strcmp(argv[1], "--gssapi") == 0) {
#if HAVE_GSSAPI
		return 0;
#else  /* HAVE_GSSAPI */
		return 1;
#endif /* HAVE_GSSAPI */
	}

	if (strcmp(argv[1], "--have-fips-dh") == 0) {
#if defined(ENABLE_FIPS_MODE)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
		return 0;
#else
		return 1;
#endif
#else
		if (isc_crypto_fips_mode()) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
			return 0;
#else
			return 1;
#endif
		}
		return 0;
#endif
	}

	if (strcmp(argv[1], "--have-fips-mode") == 0) {
#if defined(ENABLE_FIPS_MODE)
		return 0;
#else
		return isc_crypto_fips_mode() ? 0 : 1;
#endif
	}

	if (strcmp(argv[1], "--have-geoip2") == 0) {
#ifdef HAVE_GEOIP2
		return 0;
#else  /* ifdef HAVE_GEOIP2 */
		return 1;
#endif /* ifdef HAVE_GEOIP2 */
	}

	if (strcmp(argv[1], "--have-json-c") == 0) {
#ifdef HAVE_JSON_C
		return 0;
#else  /* ifdef HAVE_JSON_C */
		return 1;
#endif /* ifdef HAVE_JSON_C */
	}

	if (strcmp(argv[1], "--have-libxml2") == 0) {
#ifdef HAVE_LIBXML2
		return 0;
#else  /* ifdef HAVE_LIBXML2 */
		return 1;
#endif /* ifdef HAVE_LIBXML2 */
	}

	if (strcmp(argv[1], "--tsan") == 0) {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
		return 0;
#endif
#endif
#if __SANITIZE_THREAD__
		return 0;
#else
		return 1;
#endif
	}

	if (strcmp(argv[1], "--md5") == 0) {
		if (!dst_algorithm_supported(DST_ALG_HMACMD5)) {
			return 1;
		}
		return 0;
	}

	if (strcmp(argv[1], "--ipv6only=no") == 0) {
#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
		int s;
		int n = -1;
		int v6only = -1;
		socklen_t len = sizeof(v6only);

		s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		if (s >= 0) {
			n = getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
				       (void *)&v6only, &len);
			close(s);
		}
		return (n == 0 && v6only == 0) ? 0 : 1;
#else  /* defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY) */
		return 1;
#endif /* defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY) */
	}

	if (strcasecmp(argv[1], "--rsasha1") == 0) {
		if (!dst_algorithm_supported(DST_ALG_RSASHA1)) {
			return 1;
		}

		return 0;
	}

	if (strcmp(argv[1], "--with-dlz-filesystem") == 0) {
#ifdef DLZ_FILESYSTEM
		return 0;
#else  /* ifdef DLZ_FILESYSTEM */
		return 1;
#endif /* ifdef DLZ_FILESYSTEM */
	}

	if (strcmp(argv[1], "--with-libidn2") == 0) {
#ifdef HAVE_LIBIDN2
		return 0;
#else  /* ifdef HAVE_LIBIDN2 */
		return 1;
#endif /* ifdef HAVE_LIBIDN2 */
	}

	if (strcmp(argv[1], "--with-lmdb") == 0) {
#ifdef HAVE_LMDB
		return 0;
#else  /* ifdef HAVE_LMDB */
		return 1;
#endif /* ifdef HAVE_LMDB */
	}

	if (strcmp(argv[1], "--with-libnghttp2") == 0) {
#ifdef HAVE_LIBNGHTTP2
		return 0;
#else  /* ifdef HAVE_LIBNGHTTP2 */
		return 1;
#endif /* ifdef HAVE_LIBNGHTTP2 */
	}

	if (strcmp(argv[1], "--with-zlib") == 0) {
#ifdef HAVE_ZLIB
		return 0;
#else  /* ifdef HAVE_ZLIB */
		return 1;
#endif /* ifdef HAVE_ZLIB */
	}

	fprintf(stderr, "unknown arg: %s\n", argv[1]);
	usage();
	return 1;
}
