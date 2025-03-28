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

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <isc/dir.h>
#include <isc/magic.h>
#include <isc/string.h>
#include <isc/util.h>

#include "errno2result.h"

#define ISC_DIR_MAGIC  ISC_MAGIC('D', 'I', 'R', '*')
#define VALID_DIR(dir) ISC_MAGIC_VALID(dir, ISC_DIR_MAGIC)

void
isc_dir_init(isc_dir_t *dir) {
	REQUIRE(dir != NULL);

	dir->entry.name[0] = '\0';
	dir->entry.length = 0;

	dir->handle = NULL;

	dir->magic = ISC_DIR_MAGIC;
}

/*!
 * \brief Allocate workspace and open directory stream. If either one fails,
 * NULL will be returned.
 */
isc_result_t
isc_dir_open(isc_dir_t *dir, const char *dirname) {
	char *p;
	isc_result_t result = ISC_R_SUCCESS;

	REQUIRE(VALID_DIR(dir));
	REQUIRE(dirname != NULL);

	/*
	 * Copy directory name.  Need to have enough space for the name,
	 * a possible path separator, the wildcard, and the final NUL.
	 */
	if (strlen(dirname) + 3 > sizeof(dir->dirname)) {
		/* XXXDCL ? */
		return ISC_R_NOSPACE;
	}
	strlcpy(dir->dirname, dirname, sizeof(dir->dirname));

	/*
	 * Append path separator, if needed, and "*".
	 */
	p = dir->dirname + strlen(dir->dirname);
	if (dir->dirname < p && *(p - 1) != '/') {
		*p++ = '/';
	}
	*p++ = '*';
	*p = '\0';

	/*
	 * Open stream.
	 */
	dir->handle = opendir(dirname);

	if (dir->handle == NULL) {
		return isc__errno2result(errno);
	}

	return result;
}

/*!
 * \brief Return previously retrieved file or get next one.
 *
 * Unix's dirent has
 * separate open and read functions, but the Win32 and DOS interfaces open
 * the dir stream and reads the first file in one operation.
 */
isc_result_t
isc_dir_read(isc_dir_t *dir) {
	struct dirent *entry;

	REQUIRE(VALID_DIR(dir) && dir->handle != NULL);

	/*
	 * Fetch next file in directory.
	 */
	entry = readdir(dir->handle);

	if (entry == NULL) {
		return ISC_R_NOMORE;
	}

	/*
	 * Make sure that the space for the name is long enough.
	 */
	if (sizeof(dir->entry.name) <= strlen(entry->d_name)) {
		return ISC_R_UNEXPECTED;
	}

	strlcpy(dir->entry.name, entry->d_name, sizeof(dir->entry.name));

	/*
	 * Some dirents have d_namlen, but it is not portable.
	 */
	dir->entry.length = strlen(entry->d_name);

	return ISC_R_SUCCESS;
}

/*!
 * \brief Close directory stream.
 */
void
isc_dir_close(isc_dir_t *dir) {
	REQUIRE(VALID_DIR(dir) && dir->handle != NULL);

	(void)closedir(dir->handle);
	dir->handle = NULL;
}

/*!
 * \brief Reposition directory stream at start.
 */
isc_result_t
isc_dir_reset(isc_dir_t *dir) {
	REQUIRE(VALID_DIR(dir) && dir->handle != NULL);

	rewinddir(dir->handle);

	return ISC_R_SUCCESS;
}

isc_result_t
isc_dir_chdir(const char *dirname) {
	/*!
	 * \brief Change the current directory to 'dirname'.
	 */

	REQUIRE(dirname != NULL);

	if (chdir(dirname) < 0) {
		return isc__errno2result(errno);
	}

	return ISC_R_SUCCESS;
}

isc_result_t
isc_dir_chroot(const char *dirname) {
#ifdef HAVE_CHROOT
	void *tmp;
#endif /* ifdef HAVE_CHROOT */

	REQUIRE(dirname != NULL);

#ifdef HAVE_CHROOT
	/*
	 * Try to use getservbyname and getprotobyname before chroot.
	 * If WKS records are used in a zone under chroot, Name Service Switch
	 * may fail to load library in chroot.
	 * Do not report errors if it fails, we do not need any result now.
	 */
	tmp = getprotobyname("udp");
	if (tmp != NULL) {
		(void)getservbyname("domain", "udp");
	}

	if (chroot(dirname) < 0 || chdir("/") < 0) {
		return isc__errno2result(errno);
	}

	return ISC_R_SUCCESS;
#else  /* ifdef HAVE_CHROOT */
	return ISC_R_NOTIMPLEMENTED;
#endif /* ifdef HAVE_CHROOT */
}
