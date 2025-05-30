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
#include <sched.h> /* IWYU pragma: keep */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNIT_TESTING
#include <cmocka.h>

#include <isc/buffer.h>
#include <isc/lex.h>
#include <isc/lib.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/types.h>
#include <isc/util.h>

#include <isccfg/cfg.h>
#include <isccfg/grammar.h>
#include <isccfg/namedconf.h>

#include <tests/isc.h>

ISC_SETUP_TEST_IMPL(group) {
	isc_logconfig_t *logconfig = isc_logconfig_get();
	isc_log_createandusechannel(
		logconfig, "default_stderr", ISC_LOG_TOFILEDESC,
		ISC_LOG_DYNAMIC, ISC_LOGDESTINATION_STDERR, 0,
		ISC_LOGCATEGORY_DEFAULT, ISC_LOGMODULE_DEFAULT);

	return 0;
}

struct duration_conf {
	const char *string;
	uint32_t time;
	const char *out;
};
typedef struct duration_conf duration_conf_t;

static void
output(void *closure, const char *text, int textlen) {
	int r;

	REQUIRE(textlen >= 0 && textlen < CFG_DURATION_MAXLEN);

	r = snprintf(closure, CFG_DURATION_MAXLEN, "%s", text);
	INSIST(r == textlen);
}

/* test cfg_obj_asduration() and cfg_print_duration() */
ISC_RUN_TEST_IMPL(duration) {
	isc_result_t result;
	/*
	 * When 'out' is NULL, the printed result is expected to be the same as
	 * the input 'string', compared by ignoring the case of the characters.
	 */
	duration_conf_t durations[] = {
		{ .string = "unlimited", .time = 0 },
		{ .string = "PT0S", .time = 0 },
		{ .string = "PT42S", .time = 42 },
		{ .string = "PT10m", .time = 600 },
		{ .string = "PT10m4S", .time = 604 },
		{ .string = "PT3600S", .time = 3600 },
		{ .string = "pT2H", .time = 7200 },
		{ .string = "Pt2H3S", .time = 7203 },
		{ .string = "PT2h1m3s", .time = 7263 },
		{ .string = "p7d", .time = 604800 },
		{ .string = "P7DT2h", .time = 612000 },
		{ .string = "P2W", .time = 1209600 },
		{ .string = "P3M", .time = 8035200 },
		{ .string = "P3MT10M", .time = 8035800 },
		{ .string = "p5y", .time = 157680000 },
		{ .string = "P5YT2H", .time = 157687200 },
		{ .string = "P1Y1M1DT1H1M1S", .time = 34304461 },
		{ .string = "P99Y399M999DT3999H9999M2911754S",
		  .time = UINT32_MAX - 1 },
		{ .string = "P99Y399M999DT3999H9999M2911755S",
		  .time = UINT32_MAX },
		{ .string = "P4294967295Y4294967295M4294967295D"
			    "T4294967295H4294967295M4294967295S",
		  .time = UINT32_MAX },
		{ .string = "PT4294967294S", .time = UINT32_MAX - 1 },
		{ .string = "PT4294967295S", .time = UINT32_MAX },
		{ .string = "0", .time = 0 },
		{ .string = "30", .time = 30 },
		{ .string = "42s", .time = 42, .out = "42" },
		{ .string = "10m", .time = 600, .out = "600" },
		{ .string = "2H", .time = 7200, .out = "7200" },
		{ .string = "7d", .time = 604800, .out = "604800" },
		{ .string = "2w", .time = 1209600, .out = "1209600" },
		{ 0 }, /* Indicates that the remaining durations are invalid. */
		{ .string = "PT4Y" },
		{ .string = "P-4Y2M" },
		{ .string = "P5H1M30S" },
		{ .string = "P7Y4W" },
		{ .string = "X7Y4M" },
		{ .string = "T7H4M" },
		{ .string = "1Y6M" },
		{ .string = "PT4294967296S" },
		{ .string = "PT99999999999S" },
		{ .string = "P99999999999Y99999999999M99999999999D"
			    "T99999999999H99999999999M99999999999S" },
	};
	isc_buffer_t buf1;
	cfg_parser_t *p1 = NULL;
	cfg_obj_t *c1 = NULL;
	bool must_fail = false;

	for (size_t i = 0; i < ARRAY_SIZE(durations); i++) {
		const cfg_obj_t *kasps = NULL;
		const char cfg_tpl[] =
			"dnssec-policy \"dp\"\n"
			"{\nkeys {csk lifetime %s algorithm rsasha256;};\n};\n";
		char conf[sizeof(cfg_tpl) + CFG_DURATION_MAXLEN] = { 0 };
		char out[CFG_DURATION_MAXLEN] = { 0 };
		cfg_printer_t pctx = { .f = output, .closure = out };

		if (durations[i].string == NULL) {
			must_fail = true;
			continue;
		}

		snprintf(&conf[0], sizeof(conf), cfg_tpl, durations[i].string);

		isc_buffer_init(&buf1, conf, strlen(conf) - 1);
		isc_buffer_add(&buf1, strlen(conf) - 1);

		/* Parse with default line numbering */
		result = cfg_parser_create(mctx, &p1);
		assert_int_equal(result, ISC_R_SUCCESS);

		result = cfg_parse_buffer(p1, &buf1, "text1", 0,
					  &cfg_type_namedconf, 0, &c1);
		if (must_fail) {
			assert_int_equal(result, DNS_R_BADTTL);
			cfg_parser_destroy(&p1);
			continue;
		}
		assert_int_equal(result, ISC_R_SUCCESS);

		(void)cfg_map_get(c1, "dnssec-policy", &kasps);
		assert_non_null(kasps);
		CFG_LIST_FOREACH (kasps, element) {
			const cfg_listelt_t *key_element;
			const cfg_obj_t *lifetime = NULL;
			const cfg_obj_t *keys = NULL;
			const cfg_obj_t *key = NULL;
			const cfg_obj_t *kopts = NULL;
			cfg_obj_t *kconf = cfg_listelt_value(element);
			int cmp;

			assert_non_null(kconf);

			kopts = cfg_tuple_get(kconf, "options");
			result = cfg_map_get(kopts, "keys", &keys);

			key_element = cfg_list_first(keys);
			assert_non_null(key_element);

			key = cfg_listelt_value(key_element);
			assert_non_null(key);

			lifetime = cfg_tuple_get(key, "lifetime");
			assert_non_null(lifetime);

			assert_int_equal(durations[i].time,
					 cfg_obj_asduration(lifetime));

			cfg_print_duration_or_unlimited(&pctx, lifetime);
			cmp = strncasecmp(durations[i].out != NULL
						  ? durations[i].out
						  : durations[i].string,
					  out, strlen(durations[i].string));
			assert_int_equal(cmp, 0);
		}

		cfg_obj_destroy(p1, &c1);
		cfg_parser_destroy(&p1);
	}
}

ISC_TEST_LIST_START

ISC_TEST_ENTRY(duration)

ISC_TEST_LIST_END

ISC_TEST_MAIN_CUSTOM(setup_test_group, NULL)
