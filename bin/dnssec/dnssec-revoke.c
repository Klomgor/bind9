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
#include <unistd.h>

#include <isc/attributes.h>
#include <isc/buffer.h>
#include <isc/commandline.h>
#include <isc/file.h>
#include <isc/hash.h>
#include <isc/lib.h>
#include <isc/mem.h>
#include <isc/result.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/keyvalues.h>
#include <dns/lib.h>

#include <dst/dst.h>

#include "dnssectool.h"

static isc_mem_t *mctx = NULL;

ISC_NORETURN static void
usage(void);

static void
usage(void) {
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "    %s [options] keyfile\n\n",
		isc_commandline_progname);
	fprintf(stderr, "Version: %s\n", PACKAGE_VERSION);
	fprintf(stderr, "    -f:           force overwrite\n");
	fprintf(stderr, "    -h:           help\n");
	fprintf(stderr, "    -K directory: use directory for key files\n");
	fprintf(stderr, "    -r:           remove old keyfiles after "
			"creating revoked version\n");
	fprintf(stderr, "    -v level:     set level of verbosity\n");
	fprintf(stderr, "    -V:           print version information\n");
	fprintf(stderr, "Output:\n");
	fprintf(stderr, "     K<name>+<alg>+<new id>.key, "
			"K<name>+<alg>+<new id>.private\n");

	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv) {
	isc_result_t result;
	char const *filename = NULL;
	char *dir = NULL;
	char newname[1024], oldname[1024];
	char keystr[DST_KEY_FORMATSIZE];
	char *endp;
	int ch;
	dst_key_t *key = NULL;
	uint32_t flags;
	isc_buffer_t buf;
	bool force = false;
	bool removefile = false;
	bool id = false;

	isc_commandline_init(argc, argv);

	if (argc == 1) {
		usage();
	}

	isc_mem_create(isc_commandline_progname, &mctx);

	isc_commandline_errprint = false;

	while ((ch = isc_commandline_parse(argc, argv, "E:fK:rRhv:V")) != -1) {
		switch (ch) {
		case 'E':
			fatal("%s", isc_result_totext(DST_R_NOENGINE));
			break;
		case 'f':
			force = true;
			break;
		case 'K':
			/*
			 * We don't have to copy it here, but do it to
			 * simplify cleanup later
			 */
			dir = isc_mem_strdup(mctx, isc_commandline_argument);
			break;
		case 'r':
			removefile = true;
			break;
		case 'R':
			id = true;
			break;
		case 'v':
			verbose = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fatal("-v must be followed by a number");
			}
			break;
		case '?':
			if (isc_commandline_option != '?') {
				fprintf(stderr, "%s: invalid argument -%c\n",
					isc_commandline_progname,
					isc_commandline_option);
			}
			FALLTHROUGH;
		case 'h':
			/* Does not return. */
			usage();

		case 'V':
			/* Does not return. */
			version(isc_commandline_progname);

		default:
			fprintf(stderr, "%s: unhandled option -%c\n",
				isc_commandline_progname,
				isc_commandline_option);
			exit(EXIT_FAILURE);
		}
	}

	if (argc < isc_commandline_index + 1 ||
	    argv[isc_commandline_index] == NULL)
	{
		fatal("The key file name was not specified");
	}
	if (argc > isc_commandline_index + 1) {
		fatal("Extraneous arguments");
	}

	if (dir != NULL) {
		filename = argv[isc_commandline_index];
	} else {
		result = isc_file_splitpath(mctx, argv[isc_commandline_index],
					    &dir, &filename);
		if (result != ISC_R_SUCCESS) {
			fatal("cannot process filename %s: %s",
			      argv[isc_commandline_index],
			      isc_result_totext(result));
		}
		if (strcmp(dir, ".") == 0) {
			isc_mem_free(mctx, dir);
		}
	}

	result = dst_key_fromnamedfile(
		filename, dir, DST_TYPE_PUBLIC | DST_TYPE_PRIVATE, mctx, &key);
	if (result != ISC_R_SUCCESS) {
		fatal("Invalid keyfile name %s: %s", filename,
		      isc_result_totext(result));
	}

	if (id) {
		fprintf(stdout, "%u\n", dst_key_rid(key));
		goto cleanup;
	}
	dst_key_format(key, keystr, sizeof(keystr));

	if (verbose > 2) {
		fprintf(stderr, "%s: %s\n", isc_commandline_progname, keystr);
	}

	if (force) {
		set_keyversion(key);
	} else {
		check_keyversion(key, keystr);
	}

	flags = dst_key_flags(key);
	if ((flags & DNS_KEYFLAG_REVOKE) == 0) {
		isc_stdtime_t now = isc_stdtime_now();

		if ((flags & DNS_KEYFLAG_KSK) == 0) {
			fprintf(stderr,
				"%s: warning: Key is not flagged "
				"as a KSK. Revoking a ZSK is "
				"legal, but undefined.\n",
				isc_commandline_progname);
		}

		dst_key_settime(key, DST_TIME_REVOKE, now);

		dst_key_setflags(key, flags | DNS_KEYFLAG_REVOKE);

		isc_buffer_init(&buf, newname, sizeof(newname));
		dst_key_buildfilename(key, DST_TYPE_PUBLIC, dir, &buf);

		if (access(newname, F_OK) == 0 && !force) {
			fatal("Key file %s already exists; "
			      "use -f to force overwrite",
			      newname);
		}

		result = dst_key_tofile(key, DST_TYPE_PUBLIC | DST_TYPE_PRIVATE,
					dir);
		if (result != ISC_R_SUCCESS) {
			dst_key_format(key, keystr, sizeof(keystr));
			fatal("Failed to write key %s: %s", keystr,
			      isc_result_totext(result));
		}

		isc_buffer_clear(&buf);
		dst_key_buildfilename(key, 0, dir, &buf);
		printf("%s\n", newname);

		/*
		 * Remove old key file, if told to (and if
		 * it isn't the same as the new file)
		 */
		if (removefile) {
			isc_buffer_init(&buf, oldname, sizeof(oldname));
			dst_key_setflags(key, flags & ~DNS_KEYFLAG_REVOKE);
			dst_key_buildfilename(key, DST_TYPE_PRIVATE, dir, &buf);
			if (strcmp(oldname, newname) == 0) {
				goto cleanup;
			}
			(void)unlink(oldname);
			isc_buffer_clear(&buf);
			dst_key_buildfilename(key, DST_TYPE_PUBLIC, dir, &buf);
			(void)unlink(oldname);
		}
	} else {
		dst_key_format(key, keystr, sizeof(keystr));
		fatal("Key %s is already revoked", keystr);
	}

cleanup:
	dst_key_free(&key);
	if (verbose > 10) {
		isc_mem_stats(mctx, stdout);
	}
	if (dir != NULL) {
		isc_mem_free(mctx, dir);
	}
	isc_mem_detach(&mctx);

	return 0;
}
