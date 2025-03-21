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

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h> /* Required for struct timeval on some platforms. */
#include <syslog.h>
#include <time.h>

#include <isc/log.h>
#include <isc/overflow.h>
#include <isc/strerr.h>
#include <isc/string.h>
#include <isc/time.h>
#include <isc/tm.h>
#include <isc/util.h>

#if defined(CLOCK_REALTIME)
#define CLOCKSOURCE_HIRES CLOCK_REALTIME
#endif /* #if defined(CLOCK_REALTIME) */

#if defined(CLOCK_REALTIME_COARSE)
#define CLOCKSOURCE CLOCK_REALTIME_COARSE
#elif defined(CLOCK_REALTIME_FAST)
#define CLOCKSOURCE CLOCK_REALTIME_FAST
#else /* if defined(CLOCK_REALTIME_COARSE) */
#define CLOCKSOURCE CLOCK_REALTIME
#endif /* if defined(CLOCK_REALTIME_COARSE) */

#if !defined(CLOCKSOURCE_HIRES)
#define CLOCKSOURCE_HIRES CLOCKSOURCE
#endif /* #ifndef CLOCKSOURCE_HIRES */

#if !defined(UNIT_TESTING)
static const isc_time_t epoch = { 0, 0 };
const isc_time_t *const isc_time_epoch = &epoch;
#endif

void
isc_time_set(isc_time_t *t, unsigned int seconds, unsigned int nanoseconds) {
	REQUIRE(t != NULL);
	REQUIRE(nanoseconds < NS_PER_SEC);

	t->seconds = seconds;
	t->nanoseconds = nanoseconds;
}

void
isc_time_settoepoch(isc_time_t *t) {
	REQUIRE(t != NULL);

	t->seconds = 0;
	t->nanoseconds = 0;
}

bool
isc_time_isepoch(const isc_time_t *t) {
	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);

	if (t->seconds == 0 && t->nanoseconds == 0) {
		return true;
	}

	return false;
}

static isc_time_t
time_now(clockid_t clock) {
	isc_time_t t;
	struct timespec ts;

	RUNTIME_CHECK(clock_gettime(clock, &ts) == 0);
	INSIST(ts.tv_sec >= 0 && ts.tv_nsec >= 0 &&
	       ts.tv_nsec < (long)NS_PER_SEC);

	/*
	 * Ensure the tv_sec value fits in t->seconds.
	 */
	INSIST(sizeof(ts.tv_sec) <= sizeof(t.seconds) ||
	       ((ts.tv_sec | (unsigned int)-1) ^ (unsigned int)-1) == 0U);

	t.seconds = ts.tv_sec;
	t.nanoseconds = ts.tv_nsec;

	return t;
}

isc_time_t
isc_time_now_hires(void) {
	return time_now(CLOCKSOURCE_HIRES);
}

isc_time_t
isc_time_now(void) {
	return time_now(CLOCKSOURCE);
}

isc_nanosecs_t
isc_time_monotonic(void) {
	struct timespec ts;

	RUNTIME_CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) != -1);

	isc_time_t time = {
		.seconds = ts.tv_sec,
		.nanoseconds = ts.tv_nsec,
	};

	return isc_nanosecs_fromtime(time);
}

isc_result_t
isc_time_nowplusinterval(isc_time_t *t, const isc_interval_t *i) {
	struct timespec ts;

	REQUIRE(t != NULL);
	REQUIRE(i != NULL);
	INSIST(i->nanoseconds < NS_PER_SEC);

	if (clock_gettime(CLOCKSOURCE, &ts) == -1) {
		UNEXPECTED_SYSERROR(errno, "clock_gettime()");
		return ISC_R_UNEXPECTED;
	}

	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= (long)NS_PER_SEC) {
		return ISC_R_UNEXPECTED;
	}

	/*
	 * Ensure the resulting seconds value fits in the size of an
	 * unsigned int.  (It is written this way as a slight optimization;
	 * note that even if both values == INT_MAX, then when added
	 * and getting another 1 added below the result is UINT_MAX.)
	 */
	if ((ts.tv_sec > INT_MAX || i->seconds > INT_MAX) &&
	    ((long long)ts.tv_sec + i->seconds > UINT_MAX))
	{
		return ISC_R_RANGE;
	}

	t->seconds = ts.tv_sec + i->seconds;
	t->nanoseconds = ts.tv_nsec + i->nanoseconds;
	if (t->nanoseconds >= NS_PER_SEC) {
		t->seconds++;
		t->nanoseconds -= NS_PER_SEC;
	}

	return ISC_R_SUCCESS;
}

int
isc_time_compare(const isc_time_t *t1, const isc_time_t *t2) {
	REQUIRE(t1 != NULL && t2 != NULL);
	INSIST(t1->nanoseconds < NS_PER_SEC && t2->nanoseconds < NS_PER_SEC);

	if (t1->seconds < t2->seconds) {
		return -1;
	}
	if (t1->seconds > t2->seconds) {
		return 1;
	}
	if (t1->nanoseconds < t2->nanoseconds) {
		return -1;
	}
	if (t1->nanoseconds > t2->nanoseconds) {
		return 1;
	}
	return 0;
}

isc_result_t
isc_time_add(const isc_time_t *t, const isc_interval_t *i, isc_time_t *result) {
	REQUIRE(t != NULL && i != NULL && result != NULL);
	REQUIRE(t->nanoseconds < NS_PER_SEC && i->nanoseconds < NS_PER_SEC);

	/* Seconds */
	if (ISC_OVERFLOW_ADD(t->seconds, i->seconds, &result->seconds)) {
		return ISC_R_RANGE;
	}

	/* Nanoseconds */
	result->nanoseconds = t->nanoseconds + i->nanoseconds;
	if (result->nanoseconds >= NS_PER_SEC) {
		if (result->seconds == UINT_MAX) {
			return ISC_R_RANGE;
		}
		result->nanoseconds -= NS_PER_SEC;
		result->seconds++;
	}

	return ISC_R_SUCCESS;
}

isc_result_t
isc_time_subtract(const isc_time_t *t, const isc_interval_t *i,
		  isc_time_t *result) {
	REQUIRE(t != NULL && i != NULL && result != NULL);
	REQUIRE(t->nanoseconds < NS_PER_SEC && i->nanoseconds < NS_PER_SEC);

	/* Seconds */
	if (ISC_OVERFLOW_SUB(t->seconds, i->seconds, &result->seconds)) {
		return ISC_R_RANGE;
	}

	/* Nanoseconds */
	if (t->nanoseconds >= i->nanoseconds) {
		result->nanoseconds = t->nanoseconds - i->nanoseconds;
	} else {
		if (result->seconds == 0) {
			return ISC_R_RANGE;
		}
		result->seconds--;
		result->nanoseconds = NS_PER_SEC + t->nanoseconds -
				      i->nanoseconds;
	}

	return ISC_R_SUCCESS;
}

uint64_t
isc_time_microdiff(const isc_time_t *t1, const isc_time_t *t2) {
	uint64_t i1, i2, i3;

	REQUIRE(t1 != NULL && t2 != NULL);
	INSIST(t1->nanoseconds < NS_PER_SEC && t2->nanoseconds < NS_PER_SEC);

	i1 = (uint64_t)t1->seconds * NS_PER_SEC + t1->nanoseconds;
	i2 = (uint64_t)t2->seconds * NS_PER_SEC + t2->nanoseconds;

	if (i1 <= i2) {
		return 0;
	}

	i3 = i1 - i2;

	/*
	 * Convert to microseconds.
	 */
	i3 /= NS_PER_US;

	return i3;
}

uint32_t
isc_time_seconds(const isc_time_t *t) {
	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);

	return (uint32_t)t->seconds;
}

isc_result_t
isc_time_secondsastimet(const isc_time_t *t, time_t *secondsp) {
	time_t seconds;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);

	/*
	 * Ensure that the number of seconds represented by t->seconds
	 * can be represented by a time_t.  Since t->seconds is an
	 * unsigned int and since time_t is mostly opaque, this is
	 * trickier than it seems.  (This standardized opaqueness of
	 * time_t is *very* frustrating; time_t is not even limited to
	 * being an integral type.)
	 *
	 * The mission, then, is to avoid generating any kind of warning
	 * about "signed versus unsigned" while trying to determine if
	 * the unsigned int t->seconds is out range for tv_sec,
	 * which is pretty much only true if time_t is a signed integer
	 * of the same size as the return value of isc_time_seconds.
	 *
	 * If the paradox in the if clause below is true, t->seconds is
	 * out of range for time_t.
	 */
	seconds = (time_t)t->seconds;

	INSIST(sizeof(unsigned int) == sizeof(uint32_t));
	INSIST(sizeof(time_t) >= sizeof(uint32_t));

	if (t->seconds > (~0U >> 1) && seconds <= (time_t)(~0U >> 1)) {
		return ISC_R_RANGE;
	}

	*secondsp = seconds;

	return ISC_R_SUCCESS;
}

uint32_t
isc_time_nanoseconds(const isc_time_t *t) {
	REQUIRE(t != NULL);

	ENSURE(t->nanoseconds < NS_PER_SEC);

	return (uint32_t)t->nanoseconds;
}

uint32_t
isc_time_miliseconds(const isc_time_t *t) {
	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);

	return (t->seconds * MS_PER_SEC) + (t->nanoseconds / NS_PER_MS);
}

void
isc_time_formattimestamp(const isc_time_t *t, char *buf, unsigned int len) {
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%d-%b-%Y %X", localtime_r(&now, &tm));
	INSIST(flen < len);
	if (flen != 0) {
		snprintf(buf + flen, len - flen, ".%03u",
			 t->nanoseconds / NS_PER_MS);
	} else {
		strlcpy(buf, "99-Bad-9999 99:99:99.999", len);
	}
}

void
isc_time_formathttptimestamp(const isc_time_t *t, char *buf, unsigned int len) {
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	/*
	 * 5 spaces, 1 comma, 3 GMT, 2 %d, 4 %Y, 8 %H:%M:%S, 3+ %a, 3+
	 * %b (29+)
	 */
	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT",
			gmtime_r(&now, &tm));
	INSIST(flen < len);
}

isc_result_t
isc_time_parsehttptimestamp(char *buf, isc_time_t *t) {
	struct tm t_tm;
	time_t when;
	char *p;

	REQUIRE(buf != NULL);
	REQUIRE(t != NULL);

	p = isc_tm_strptime(buf, "%a, %d %b %Y %H:%M:%S", &t_tm);
	if (p == NULL) {
		return ISC_R_UNEXPECTED;
	}
	when = isc_tm_timegm(&t_tm);
	if (when == -1) {
		return ISC_R_UNEXPECTED;
	}
	isc_time_set(t, when, 0);
	return ISC_R_SUCCESS;
}

void
isc_time_formatISO8601Lms(const isc_time_t *t, char *buf, unsigned int len) {
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", localtime_r(&now, &tm));
	INSIST(flen < len);
	if (flen > 0U && len - flen >= 6) {
		snprintf(buf + flen, len - flen, ".%03u",
			 t->nanoseconds / NS_PER_MS);
	}
}

void
isc_time_formatISO8601TZms(const isc_time_t *t, char *buf, unsigned int len) {
	char strftime_buf[64] = { 0 };
	char ms_buf[8] = { 0 };
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(strftime_buf, len, "%Y-%m-%dT%H:%M:%S.xxx%z",
			localtime_r(&now, &tm));
	snprintf(ms_buf, sizeof(ms_buf), ".%03u", t->nanoseconds / NS_PER_MS);

	INSIST(flen < len);
	size_t local_date_len = sizeof("yyyy-mm-ddThh:mm:ss") - 1ul;
	size_t ms_date_len = local_date_len + 4;

	memmove(buf, strftime_buf, local_date_len);
	memmove(buf + local_date_len, ms_buf, 4);
	memmove(buf + ms_date_len, strftime_buf + ms_date_len, 3);
	buf[ms_date_len + 3] = ':';
	memmove(buf + ms_date_len + 4, strftime_buf + ms_date_len + 3, 3);
}
void
isc_time_formatISO8601(const isc_time_t *t, char *buf, unsigned int len) {
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));
	INSIST(flen < len);
}

void
isc_time_formatISO8601ms(const isc_time_t *t, char *buf, unsigned int len) {
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));
	INSIST(flen < len);
	if (flen > 0U && len - flen >= 5) {
		flen -= 1; /* rewind one character (Z) */
		snprintf(buf + flen, len - flen, ".%03uZ",
			 t->nanoseconds / NS_PER_MS);
	}
}

void
isc_time_formatISO8601us(const isc_time_t *t, char *buf, unsigned int len) {
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));
	INSIST(flen < len);
	if (flen > 0U && len - flen >= 5) {
		flen -= 1; /* rewind one character (Z) */
		snprintf(buf + flen, len - flen, ".%06uZ",
			 t->nanoseconds / NS_PER_US);
	}
}

void
isc_time_formatshorttimestamp(const isc_time_t *t, char *buf,
			      unsigned int len) {
	time_t now;
	unsigned int flen;
	struct tm tm;

	REQUIRE(t != NULL);
	INSIST(t->nanoseconds < NS_PER_SEC);
	REQUIRE(buf != NULL);
	REQUIRE(len > 0);

	now = (time_t)t->seconds;
	flen = strftime(buf, len, "%Y%m%d%H%M%S", gmtime_r(&now, &tm));
	INSIST(flen < len);
	if (flen > 0U && len - flen >= 5) {
		snprintf(buf + flen, len - flen, "%03u",
			 t->nanoseconds / NS_PER_MS);
	}
}
