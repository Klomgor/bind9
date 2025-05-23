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

#include <stdlib.h>

#include <isc/log.h>
#include <isc/result.h>
#include <isc/util.h>

#include <named/log.h>

#ifndef ISC_FACILITY
#define ISC_FACILITY LOG_DAEMON
#endif /* ifndef ISC_FACILITY */

isc_result_t
named_log_init(bool safe) {
	isc_result_t result;
	isc_logconfig_t *lcfg = NULL;

	/*
	 * This is not technically needed, as we are calling named_log_init()
	 * only at the start of named process.  But since the named binary is
	 * the only place that also calls isc_logconfig_set(), this is a good
	 * hygiene.
	 */
	rcu_read_lock();
	lcfg = isc_logconfig_get();
	if (safe) {
		named_log_setsafechannels(lcfg);
	} else {
		named_log_setdefaultchannels(lcfg);
	}

	result = named_log_setdefaultcategory(lcfg);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	named_log_setdefaultsslkeylogfile(lcfg);
	rcu_read_unlock();

	return ISC_R_SUCCESS;

cleanup:
	rcu_read_unlock();

	return result;
}

void
named_log_setdefaultchannels(isc_logconfig_t *lcfg) {
	isc_logdestination_t destination;

	/*
	 * By default, the logging library makes "default_debug" log to
	 * stderr.  In BIND, we want to override this and log to named.run
	 * instead, unless the -g option was given.
	 */
	if (!named_g_logstderr) {
		destination.file.stream = NULL;
		destination.file.name = "named.run";
		destination.file.versions = ISC_LOG_ROLLNEVER;
		destination.file.maximum_size = 0;
		isc_log_createchannel(lcfg, "default_debug", ISC_LOG_TOFILE,
				      ISC_LOG_DYNAMIC, &destination,
				      ISC_LOG_PRINTTIME | ISC_LOG_DEBUGONLY);
	} else if (named_g_logstderr && (named_g_logflags != 0)) {
		/*
		 * If the option -g is given, but we also requested iso
		 * timestamps, we'll still need to override the "default_debug"
		 * logger with a new one.
		 */
		isc_log_createchannel(lcfg, "default_debug", ISC_LOG_TOFILEDESC,
				      ISC_LOG_DYNAMIC,
				      ISC_LOGDESTINATION_STDERR,
				      ISC_LOG_PRINTTIME | named_g_logflags);
	}

	if (named_g_logfile != NULL) {
		destination.file.stream = NULL;
		destination.file.name = named_g_logfile;
		destination.file.versions = ISC_LOG_ROLLNEVER;
		destination.file.maximum_size = 0;
		isc_log_createchannel(lcfg, "default_logfile", ISC_LOG_TOFILE,
				      ISC_LOG_DYNAMIC, &destination,
				      ISC_LOG_PRINTTIME |
					      ISC_LOG_PRINTCATEGORY |
					      ISC_LOG_PRINTLEVEL);
	}

#if ISC_FACILITY != LOG_DAEMON
	destination.facility = ISC_FACILITY;
	isc_log_createchannel(lcfg, "default_syslog", ISC_LOG_TOSYSLOG,
			      ISC_LOG_INFO, &destination, 0);
#endif /* if ISC_FACILITY != LOG_DAEMON */

	/*
	 * Set the initial debug level.
	 */
	isc_log_setdebuglevel(named_g_debuglevel);
}

void
named_log_setsafechannels(isc_logconfig_t *lcfg) {
	isc_logdestination_t destination;

	if (!named_g_logstderr) {
		isc_log_createchannel(lcfg, "default_debug", ISC_LOG_TONULL,
				      ISC_LOG_DYNAMIC, NULL, 0);

		/*
		 * Setting the debug level to zero should get the output
		 * discarded a bit faster.
		 */
		isc_log_setdebuglevel(0);
	} else if (named_g_logflags != 0) {
		/*
		 * The -g option sets logstderr, and also sets logflags
		 * to print ISO timestamps. Since that isn't the default
		 * behavior, we need to override the "default_debug"
		 * channel with a new one.
		 */
		isc_log_createchannel(lcfg, "default_debug", ISC_LOG_TOFILEDESC,
				      ISC_LOG_DYNAMIC,
				      ISC_LOGDESTINATION_STDERR,
				      named_g_logflags);
		isc_log_setdebuglevel(named_g_debuglevel);
	} else {
		UNREACHABLE();
	}

	if (named_g_logfile != NULL) {
		destination.file.stream = NULL;
		destination.file.name = named_g_logfile;
		destination.file.versions = ISC_LOG_ROLLNEVER;
		destination.file.maximum_size = 0;
		isc_log_createchannel(lcfg, "default_logfile", ISC_LOG_TOFILE,
				      ISC_LOG_DYNAMIC, &destination,
				      ISC_LOG_PRINTTIME |
					      ISC_LOG_PRINTCATEGORY |
					      ISC_LOG_PRINTLEVEL);
	}

#if ISC_FACILITY != LOG_DAEMON
	destination.facility = ISC_FACILITY;
	isc_log_createchannel(lcfg, "default_syslog", ISC_LOG_TOSYSLOG,
			      ISC_LOG_INFO, &destination, 0);
#endif /* if ISC_FACILITY != LOG_DAEMON */
}

/*
 * If the SSLKEYLOGFILE environment variable is set, TLS pre-master secrets are
 * logged (for debugging purposes) to the file whose path is provided in that
 * variable.  Set up a default logging channel which maintains up to 10 files
 * containing TLS pre-master secrets, each up to 100 MB in size.  If the
 * SSLKEYLOGFILE environment variable is set to the string "config", suppress
 * creation of the default channel, allowing custom logging channel
 * configuration for TLS pre-master secrets to be provided via the "logging"
 * stanza in the configuration file.
 */
void
named_log_setdefaultsslkeylogfile(isc_logconfig_t *lcfg) {
	const char *sslkeylogfile_path = getenv("SSLKEYLOGFILE");
	isc_logdestination_t destination = {
		.file = {
			.name = sslkeylogfile_path,
			.versions = 10,
			.suffix = isc_log_rollsuffix_timestamp,
			.maximum_size = 100 * 1024 * 1024,
		},
	};

	if (sslkeylogfile_path == NULL ||
	    strcmp(sslkeylogfile_path, "config") == 0)
	{
		return;
	}

	isc_log_createandusechannel(lcfg, "default_sslkeylogfile",
				    ISC_LOG_TOFILE, ISC_LOG_INFO, &destination,
				    0, ISC_LOGCATEGORY_SSLKEYLOG,
				    ISC_LOGMODULE_DEFAULT);
}

isc_result_t
named_log_setdefaultcategory(isc_logconfig_t *lcfg) {
	isc_result_t result = ISC_R_SUCCESS;

	result = isc_log_usechannel(lcfg, "default_debug",
				    ISC_LOGCATEGORY_DEFAULT,
				    ISC_LOGMODULE_DEFAULT);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	if (!named_g_logstderr) {
		if (named_g_logfile != NULL) {
			result = isc_log_usechannel(lcfg, "default_logfile",
						    ISC_LOGCATEGORY_DEFAULT,
						    ISC_LOGMODULE_DEFAULT);
		} else if (!named_g_nosyslog) {
			result = isc_log_usechannel(lcfg, "default_syslog",
						    ISC_LOGCATEGORY_DEFAULT,
						    ISC_LOGMODULE_DEFAULT);
		}
	}

cleanup:
	return result;
}

isc_result_t
named_log_setunmatchedcategory(isc_logconfig_t *lcfg) {
	isc_result_t result;

	result = isc_log_usechannel(lcfg, "null", NAMED_LOGCATEGORY_UNMATCHED,
				    ISC_LOGMODULE_DEFAULT);
	return result;
}
