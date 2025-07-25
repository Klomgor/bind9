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
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*
 * As a workaround, include an OpenSSL header file before including cmocka.h,
 * because OpenSSL 3.1.0 uses __attribute__(malloc), conflicting with a
 * redefined malloc in cmocka.h.
 */
#include <openssl/err.h>

#define UNIT_TESTING
#include <cmocka.h>

#include <isc/async.h>
#include <isc/atomic.h>
#include <isc/buffer.h>
#include <isc/lib.h>
#include <isc/mutex.h>
#include <isc/netmgr.h>
#include <isc/nonce.h>
#include <isc/os.h>
#include <isc/refcount.h>
#include <isc/sockaddr.h>
#include <isc/thread.h>
#include <isc/util.h>
#include <isc/uv.h>

#include "uv_wrap.h"
#define KEEP_BEFORE

#include "netmgr/http.c"
#include "netmgr/netmgr-int.h"
#include "netmgr/socket.c"

#include <tests/isc.h>

static isc_sockaddr_t tcp_listen_addr;

static uint64_t send_magic = 0;
static uint64_t stop_magic = 0;

static uv_buf_t send_msg = { .base = (char *)&send_magic,
			     .len = sizeof(send_magic) };

static atomic_int_fast64_t active_cconnects = 0;
static atomic_int_fast64_t nsends = 0;
static atomic_int_fast64_t ssends = 0;
static atomic_int_fast64_t sreads = 0;
static atomic_int_fast64_t csends = 0;
static atomic_int_fast64_t creads = 0;
static atomic_int_fast64_t ctimeouts = 0;
static atomic_int_fast64_t total_sends = 0;

static int expected_ssends;
static int expected_sreads;
static int expected_csends;
static int expected_cconnects;
static int expected_creads;
static int expected_ctimeouts;

#define have_expected_ssends(v) ((v) >= expected_ssends && expected_ssends >= 0)
#define have_expected_sreads(v) ((v) >= expected_sreads && expected_sreads >= 0)
#define have_expected_csends(v) ((v) >= expected_csends && expected_csends >= 0)
#define have_expected_cconnects(v) \
	((v) >= expected_cconnects && expected_cconnects >= 0)
#define have_expected_creads(v) ((v) >= expected_creads && expected_creads >= 0)
#define have_expected_ctimeouts(v) \
	((v) >= expected_ctimeouts && expected_ctimeouts >= 0)

static bool noanswer = false;

static atomic_bool POST = true;

static atomic_bool use_TLS = false;
static isc_tlsctx_t *server_tlsctx = NULL;
static isc_tlsctx_t *client_tlsctx = NULL;
static isc_tlsctx_client_session_cache_t *client_sess_cache = NULL;

static isc_quota_t listener_quota;
static atomic_bool check_listener_quota = false;

static isc_nm_http_endpoints_t *endpoints = NULL;

static atomic_bool use_PROXY = false;
static atomic_bool use_PROXY_over_TLS = false;

/* Timeout for soft-timeout tests (0.05 seconds) */
#define T_SOFT	  50
#define T_CONNECT 30 * 1000

#define NSENDS	100
#define NWRITES 10

#define CHECK_RANGE_FULL(v)                                    \
	{                                                      \
		int __v = atomic_load(&v);                     \
		assert_true(__v >= atomic_load(&total_sends)); \
	}

#define CHECK_RANGE_HALF(v)                                        \
	{                                                          \
		int __v = atomic_load(&v);                         \
		assert_true(__v >= atomic_load(&total_sends) / 2); \
	}

/* Enable this to print values while running tests */
#undef PRINT_DEBUG
#ifdef PRINT_DEBUG
#define X(v) fprintf(stderr, #v " = %" PRIu64 "\n", atomic_load(&v))
#else
#define X(v)
#endif

static isc_nm_proxy_type_t
get_proxy_type(void) {
	if (!atomic_load(&use_PROXY)) {
		return ISC_NM_PROXY_NONE;
	} else if (atomic_load(&use_TLS) && atomic_load(&use_PROXY_over_TLS)) {
		return ISC_NM_PROXY_ENCRYPTED;
	}

	return ISC_NM_PROXY_PLAIN;
}

static void
proxy_verify_unspec_endpoint(isc_nmhandle_t *handle) {
	isc_sockaddr_t real_local, real_peer, local, peer;

	if (isc_nm_is_proxy_unspec(handle)) {
		peer = isc_nmhandle_peeraddr(handle);
		local = isc_nmhandle_localaddr(handle);
		real_peer = isc_nmhandle_real_peeraddr(handle);
		real_local = isc_nmhandle_real_localaddr(handle);

		assert_true(isc_sockaddr_equal(&peer, &real_peer));
		assert_true(isc_sockaddr_equal(&local, &real_local));
	}
}

typedef struct csdata {
	isc_mem_t *mctx;
	isc_nm_recv_cb_t reply_cb;
	void *cb_arg;
	isc_region_t region;
} csdata_t;

static void
connect_send_cb(isc_nmhandle_t *handle, isc_result_t result, void *arg) {
	csdata_t data;

	(void)atomic_fetch_sub(&active_cconnects, 1);
	memmove(&data, arg, sizeof(data));
	isc_mem_put(data.mctx, arg, sizeof(data));
	if (result != ISC_R_SUCCESS) {
		goto error;
	}

	REQUIRE(VALID_NMHANDLE(handle));

	result = isc__nm_http_request(handle, &data.region, data.reply_cb,
				      data.cb_arg);
	if (result != ISC_R_SUCCESS) {
		goto error;
	}

	isc_mem_putanddetach(&data.mctx, data.region.base, data.region.length);
	return;
error:
	data.reply_cb(handle, result, NULL, data.cb_arg);
	isc_mem_putanddetach(&data.mctx, data.region.base, data.region.length);
}

static void
connect_send_request(const char *uri, bool post, isc_region_t *region,
		     isc_nm_recv_cb_t cb, void *cbarg, bool tls,
		     unsigned int timeout) {
	isc_region_t copy;
	csdata_t *data = NULL;
	isc_tlsctx_t *ctx = NULL;

	copy = (isc_region_t){ .base = isc_mem_get(mctx, region->length),
			       .length = region->length };
	memmove(copy.base, region->base, region->length);
	data = isc_mem_get(mctx, sizeof(*data));
	*data = (csdata_t){ .reply_cb = cb, .cb_arg = cbarg, .region = copy };
	isc_mem_attach(mctx, &data->mctx);
	if (tls) {
		ctx = client_tlsctx;
	}

	isc_nm_httpconnect(NULL, &tcp_listen_addr, uri, post, connect_send_cb,
			   data, ctx, NULL, client_sess_cache, timeout,
			   get_proxy_type(), NULL);
}

static int
setup_ephemeral_port(isc_sockaddr_t *addr, sa_family_t family) {
	isc_result_t result;
	socklen_t addrlen = sizeof(*addr);
	int fd;
	int r;

	isc_sockaddr_fromin6(addr, &in6addr_loopback, 0);

	fd = socket(AF_INET6, family, 0);
	if (fd < 0) {
		perror("setup_ephemeral_port: socket()");
		return -1;
	}

	r = bind(fd, (const struct sockaddr *)&addr->type.sa,
		 sizeof(addr->type.sin6));
	if (r != 0) {
		perror("setup_ephemeral_port: bind()");
		isc__nm_closesocket(fd);
		return r;
	}

	r = getsockname(fd, (struct sockaddr *)&addr->type.sa, &addrlen);
	if (r != 0) {
		perror("setup_ephemeral_port: getsockname()");
		isc__nm_closesocket(fd);
		return r;
	}

	result = isc__nm_socket_reuse(fd, 1);
	if (result != ISC_R_SUCCESS && result != ISC_R_NOTIMPLEMENTED) {
		fprintf(stderr,
			"setup_ephemeral_port: isc__nm_socket_reuse(): %s",
			isc_result_totext(result));
		close(fd);
		return -1;
	}

	result = isc__nm_socket_reuse_lb(fd);
	if (result != ISC_R_SUCCESS && result != ISC_R_NOTIMPLEMENTED) {
		fprintf(stderr,
			"setup_ephemeral_port: isc__nm_socket_reuse_lb(): %s",
			isc_result_totext(result));
		close(fd);
		return -1;
	}

#if IPV6_RECVERR
#define setsockopt_on(socket, level, name) \
	setsockopt(socket, level, name, &(int){ 1 }, sizeof(int))

	r = setsockopt_on(fd, IPPROTO_IPV6, IPV6_RECVERR);
	if (r != 0) {
		perror("setup_ephemeral_port");
		close(fd);
		return r;
	}
#endif

	return fd;
}

/* Generic */

static void
noop_read_cb(isc_nmhandle_t *handle, isc_result_t result, isc_region_t *region,
	     void *cbarg) {
	UNUSED(handle);
	UNUSED(result);
	UNUSED(region);
	UNUSED(cbarg);
}

thread_local uint8_t tcp_buffer_storage[4096];
thread_local size_t tcp_buffer_length = 0;

static int
setup_test(void **state) {
	char *env_workers = getenv("ISC_TASK_WORKERS");
	uv_os_sock_t tcp_listen_sock = -1;

	tcp_listen_addr = (isc_sockaddr_t){ .length = 0 };
	tcp_listen_sock = setup_ephemeral_port(&tcp_listen_addr, SOCK_STREAM);
	if (tcp_listen_sock < 0) {
		return -1;
	}
	close(tcp_listen_sock);
	tcp_listen_sock = -1;

	if (env_workers != NULL) {
		workers = atoi(env_workers);
	} else {
		workers = isc_os_ncpus();
	}
	INSIST(workers > 0);

	atomic_store(&total_sends, NSENDS * NWRITES);
	atomic_store(&nsends, atomic_load(&total_sends));

	atomic_store(&csends, 0);
	atomic_store(&creads, 0);
	atomic_store(&sreads, 0);
	atomic_store(&ssends, 0);
	atomic_store(&ctimeouts, 0);
	atomic_store(&active_cconnects, 0);

	expected_cconnects = -1;
	expected_csends = -1;
	expected_creads = -1;
	expected_sreads = -1;
	expected_ssends = -1;
	expected_ctimeouts = -1;

	atomic_store(&POST, false);
	atomic_store(&use_TLS, false);
	atomic_store(&use_PROXY, false);
	atomic_store(&use_PROXY_over_TLS, false);

	noanswer = false;

	isc_nonce_buf(&send_magic, sizeof(send_magic));
	isc_nonce_buf(&stop_magic, sizeof(stop_magic));
	if (send_magic == stop_magic) {
		return -1;
	}

	setup_loopmgr(state);
	setup_netmgr(state);

	server_tlsctx = NULL;
	isc_tlsctx_createserver(NULL, NULL, &server_tlsctx);
	isc_tlsctx_enable_http2server_alpn(server_tlsctx);
	client_tlsctx = NULL;
	isc_tlsctx_createclient(&client_tlsctx);
	isc_tlsctx_enable_http2client_alpn(client_tlsctx);
	isc_tlsctx_client_session_cache_create(
		mctx, client_tlsctx,
		ISC_TLSCTX_CLIENT_SESSION_CACHE_DEFAULT_SIZE,
		&client_sess_cache);

	isc_quota_init(&listener_quota, 0);
	atomic_store(&check_listener_quota, false);

	INSIST(endpoints == NULL);
	endpoints = isc_nm_http_endpoints_new(mctx);

	return 0;
}

static int
teardown_test(void **state ISC_ATTR_UNUSED) {
	teardown_netmgr(state);
	teardown_loopmgr(state);

	if (server_tlsctx != NULL) {
		isc_tlsctx_free(&server_tlsctx);
	}
	if (client_tlsctx != NULL) {
		isc_tlsctx_free(&client_tlsctx);
	}

	isc_tlsctx_client_session_cache_detach(&client_sess_cache);

	isc_quota_destroy(&listener_quota);

	isc_nm_http_endpoints_detach(&endpoints);

	return 0;
}

thread_local size_t nwrites = NWRITES;

static void
sockaddr_to_url(isc_sockaddr_t *sa, const bool https, char *outbuf,
		size_t outbuf_len, const char *append) {
	isc_nm_http_makeuri(https, sa, NULL, 0, append, outbuf, outbuf_len);
}

static isc_quota_t *
init_listener_quota(size_t nthreads) {
	isc_quota_t *quotap = NULL;
	if (atomic_load(&check_listener_quota)) {
		unsigned int max_quota = ISC_MAX(nthreads / 2, 1);
		isc_quota_max(&listener_quota, max_quota);
		quotap = &listener_quota;
	}
	return quotap;
}

static void
doh_receive_reply_cb(isc_nmhandle_t *handle, isc_result_t eresult,
		     isc_region_t *region, void *cbarg) {
	assert_non_null(handle);
	UNUSED(cbarg);
	UNUSED(region);

	if (eresult == ISC_R_SUCCESS) {
		if (atomic_load(&use_PROXY)) {
			assert_true(isc_nm_is_proxy_handle(handle));
		}
		(void)atomic_fetch_sub(&nsends, 1);
		if (have_expected_csends(atomic_fetch_add(&csends, 1) + 1) ||
		    have_expected_creads(atomic_fetch_add(&creads, 1) + 1))
		{
			isc_loopmgr_shutdown();
		}
	} else {
		isc_loopmgr_shutdown();
	}
}

static void
doh_reply_sent_cb(isc_nmhandle_t *handle, isc_result_t eresult, void *cbarg) {
	UNUSED(eresult);
	UNUSED(cbarg);

	assert_non_null(handle);

	if (eresult == ISC_R_SUCCESS) {
		atomic_fetch_add(&ssends, 1);
	}
}

static void
doh_receive_request_cb(isc_nmhandle_t *handle, isc_result_t eresult,
		       isc_region_t *region, void *cbarg) {
	uint64_t magic = 0;

	UNUSED(cbarg);
	assert_non_null(handle);

	if (eresult != ISC_R_SUCCESS) {
		return;
	}

	if (atomic_load(&use_PROXY)) {
		assert_true(isc_nm_is_proxy_handle(handle));
		proxy_verify_unspec_endpoint(handle);
	}

	atomic_fetch_add(&sreads, 1);

	memmove(tcp_buffer_storage + tcp_buffer_length, region->base,
		region->length);
	tcp_buffer_length += region->length;

	while (tcp_buffer_length >= sizeof(magic)) {
		magic = *(uint64_t *)tcp_buffer_storage;
		assert_true(magic == stop_magic || magic == send_magic);

		tcp_buffer_length -= sizeof(magic);
		memmove(tcp_buffer_storage, tcp_buffer_storage + sizeof(magic),
			tcp_buffer_length);

		if (magic == send_magic) {
			if (!noanswer) {
				isc_nm_send(handle, region, doh_reply_sent_cb,
					    NULL);
			}
			return;
		} else if (magic == stop_magic) {
			/*
			 * We are done, so we don't send anything back.
			 * There should be no more packets in the buffer.
			 */
			assert_int_equal(tcp_buffer_length, 0);
		}
	}
}

ISC_LOOP_TEST_IMPL(mock_doh_uv_tcp_bind) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_nmsocket_t *listen_sock = NULL;

	WILL_RETURN(uv_tcp_bind, UV_EADDRINUSE);

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   noop_read_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);
	result = isc_nm_listenhttp(ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0, NULL,
				   NULL, endpoints, 0, false, &listen_sock);
	assert_int_not_equal(result, ISC_R_SUCCESS);
	assert_null(listen_sock);

	RESET_RETURN;

	isc_loopmgr_shutdown();
}

static void
listen_sock_close(void *arg) {
	isc_nmsocket_t *listen_sock = arg;

	isc_nm_stoplistening(listen_sock);
	isc_nmsocket_close(&listen_sock);
	assert_null(listen_sock);
}

static void
doh_noop(void *arg ISC_ATTR_UNUSED) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_nmsocket_t *listen_sock = NULL;
	char req_url[256];

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   noop_read_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = isc_nm_listenhttp(ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0, NULL,
				   NULL, endpoints, 0, get_proxy_type(),
				   &listen_sock);
	assert_int_equal(result, ISC_R_SUCCESS);
	isc_loop_teardown(isc_loop_main(), listen_sock_close, listen_sock);

	sockaddr_to_url(&tcp_listen_addr, false, req_url, sizeof(req_url),
			ISC_NM_HTTP_DEFAULT_PATH);
	connect_send_request(req_url, atomic_load(&POST),
			     &(isc_region_t){ .base = (uint8_t *)send_msg.base,
					      .length = send_msg.len },
			     noop_read_cb, NULL, atomic_load(&use_TLS), 30000);

	isc_loopmgr_shutdown();

	assert_int_equal(0, atomic_load(&csends));
	assert_int_equal(0, atomic_load(&creads));
	assert_int_equal(0, atomic_load(&sreads));
	assert_int_equal(0, atomic_load(&ssends));
}

ISC_LOOP_TEST_IMPL(doh_noop_POST) {
	atomic_store(&POST, true);
	doh_noop(arg);
}

ISC_LOOP_TEST_IMPL(doh_noop_GET) {
	atomic_store(&POST, false);
	doh_noop(arg);
}

static void
doh_noresponse(void *arg ISC_ATTR_UNUSED) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_nmsocket_t *listen_sock = NULL;
	char req_url[256];

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   noop_read_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = isc_nm_listenhttp(ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0, NULL,
				   NULL, endpoints, 0, get_proxy_type(),
				   &listen_sock);
	assert_int_equal(result, ISC_R_SUCCESS);
	isc_loop_teardown(isc_loop_main(), listen_sock_close, listen_sock);

	sockaddr_to_url(&tcp_listen_addr, false, req_url, sizeof(req_url),
			ISC_NM_HTTP_DEFAULT_PATH);
	connect_send_request(req_url, atomic_load(&POST),
			     &(isc_region_t){ .base = (uint8_t *)send_msg.base,
					      .length = send_msg.len },
			     noop_read_cb, NULL, atomic_load(&use_TLS), 30000);

	isc_loopmgr_shutdown();
}

ISC_LOOP_TEST_IMPL(doh_noresponse_POST) {
	atomic_store(&POST, true);
	doh_noresponse(arg);
}

ISC_LOOP_TEST_IMPL(doh_noresponse_GET) {
	atomic_store(&POST, false);
	doh_noresponse(arg);
}

static void
timeout_query_sent_cb(isc_nmhandle_t *handle, isc_result_t eresult,
		      void *cbarg) {
	UNUSED(eresult);
	UNUSED(cbarg);

	assert_non_null(handle);

	if (eresult == ISC_R_SUCCESS) {
		atomic_fetch_add(&csends, 1);
	}

	isc_nmhandle_detach(&handle);
}

static void
timeout_retry_cb(isc_nmhandle_t *handle, isc_result_t eresult,
		 isc_region_t *region ISC_ATTR_UNUSED,
		 void *arg ISC_ATTR_UNUSED) {
	assert_non_null(handle);

	atomic_fetch_add(&ctimeouts, 1);

	if (eresult == ISC_R_TIMEDOUT && atomic_load(&ctimeouts) < 5) {
		isc_nmhandle_settimeout(handle, T_SOFT);
		return;
	}

	isc_nmhandle_detach(&handle);
	isc_loopmgr_shutdown();
}

static void
timeout_request_cb(isc_nmhandle_t *handle, isc_result_t result, void *arg) {
	isc_nmhandle_t *sendhandle = NULL;
	isc_nmhandle_t *readhandle = NULL;

	REQUIRE(VALID_NMHANDLE(handle));

	if (result != ISC_R_SUCCESS) {
		return;
	}

	isc_nmhandle_attach(handle, &sendhandle);
	isc_nm_send(handle,
		    &(isc_region_t){ .base = (uint8_t *)send_msg.base,
				     .length = send_msg.len },
		    timeout_query_sent_cb, arg);

	isc_nmhandle_attach(handle, &readhandle);
	isc_nmhandle_settimeout(handle, T_SOFT);
	isc_nm_read(handle, timeout_retry_cb, NULL);
}

static void
doh_timeout_recovery(void *arg ISC_ATTR_UNUSED) {
	isc_nmsocket_t *listen_sock = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	isc_tlsctx_t *ctx = atomic_load(&use_TLS) ? server_tlsctx : NULL;
	char req_url[256];

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   doh_receive_request_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = isc_nm_listenhttp(ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0, NULL,
				   NULL, endpoints, 0, get_proxy_type(),
				   &listen_sock);
	assert_int_equal(result, ISC_R_SUCCESS);
	isc_loop_teardown(isc_loop_main(), listen_sock_close, listen_sock);

	/*
	 * Accept connections but don't send responses, forcing client
	 * reads to time out.
	 */
	noanswer = true;

	/*
	 * timeout_retry_cb() will give up after five timeouts.
	 */
	sockaddr_to_url(&tcp_listen_addr, false, req_url, sizeof(req_url),
			ISC_NM_HTTP_DEFAULT_PATH);
	isc_nm_httpconnect(NULL, &tcp_listen_addr, req_url, atomic_load(&POST),
			   timeout_request_cb, NULL, ctx, NULL,
			   client_sess_cache, T_CONNECT, get_proxy_type(),
			   NULL);
}

static int
doh_timeout_recovery_teardown(void **state) {
	assert_true(atomic_load(&ctimeouts) == 5);
	return teardown_test(state);
}

ISC_LOOP_TEST_IMPL(doh_timeout_recovery_POST) {
	atomic_store(&POST, true);
	doh_timeout_recovery(arg);
}

ISC_LOOP_TEST_IMPL(doh_timeout_recovery_GET) {
	atomic_store(&POST, false);
	doh_timeout_recovery(arg);
}

static void
doh_connect_thread(void *arg);

static void
doh_receive_send_reply_cb(isc_nmhandle_t *handle, isc_result_t eresult,
			  isc_region_t *region, void *cbarg ISC_ATTR_UNUSED) {
	if (eresult != ISC_R_SUCCESS) {
		return;
	}

	assert_non_null(handle);
	UNUSED(region);

	int_fast64_t sends = atomic_fetch_sub(&nsends, 1);
	atomic_fetch_add(&csends, 1);
	atomic_fetch_add(&creads, 1);
	if (sends > 0) {
		size_t i;
		for (i = 0; i < NWRITES / 2; i++) {
			eresult = isc__nm_http_request(
				handle,
				&(isc_region_t){
					.base = (uint8_t *)send_msg.base,
					.length = send_msg.len },
				doh_receive_send_reply_cb, NULL);
			if (eresult == ISC_R_CANCELED) {
				break;
			}
			assert_true(eresult == ISC_R_SUCCESS);
		}

		isc_async_current(doh_connect_thread, NULL);
	}
	if (sends <= 0) {
		isc_loopmgr_shutdown();
	}
}

static void
doh_connect_thread(void *arg ISC_ATTR_UNUSED) {
	char req_url[256];
	int64_t sends = atomic_load(&nsends);

	sockaddr_to_url(&tcp_listen_addr, atomic_load(&use_TLS), req_url,
			sizeof(req_url), ISC_NM_HTTP_DEFAULT_PATH);

	/*
	 * We need to back off and slow down if we start getting
	 * errors, to prevent a thundering herd problem.
	 */
	int_fast64_t active = atomic_fetch_add(&active_cconnects, 1);
	if (active > workers) {
		atomic_fetch_sub(&active_cconnects, 1);
		return;
	}
	connect_send_request(req_url, atomic_load(&POST),
			     &(isc_region_t){ .base = (uint8_t *)send_msg.base,
					      .length = send_msg.len },
			     doh_receive_send_reply_cb, NULL,
			     atomic_load(&use_TLS), 30000);

	if (sends <= 0) {
		isc_loopmgr_shutdown();
	}
}

static void
doh_recv_one(void *arg ISC_ATTR_UNUSED) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_nmsocket_t *listen_sock = NULL;
	char req_url[256];
	isc_quota_t *quotap = init_listener_quota(workers);

	atomic_store(&total_sends, 1);
	expected_creads = 1;

	atomic_store(&nsends, atomic_load(&total_sends));

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   doh_receive_request_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = isc_nm_listenhttp(
		ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0, quotap,
		atomic_load(&use_TLS) ? server_tlsctx : NULL, endpoints, 0,
		get_proxy_type(), &listen_sock);
	assert_int_equal(result, ISC_R_SUCCESS);

	sockaddr_to_url(&tcp_listen_addr, atomic_load(&use_TLS), req_url,
			sizeof(req_url), ISC_NM_HTTP_DEFAULT_PATH);
	connect_send_request(req_url, atomic_load(&POST),
			     &(isc_region_t){ .base = (uint8_t *)send_msg.base,
					      .length = send_msg.len },
			     doh_receive_reply_cb, NULL, atomic_load(&use_TLS),
			     30000);

	isc_loop_teardown(isc_loop_main(), listen_sock_close, listen_sock);
}

static int
doh_recv_one_teardown(void **state) {
	X(total_sends);
	X(csends);
	X(creads);
	X(sreads);
	X(ssends);

	assert_int_equal(atomic_load(&csends), 1);
	assert_int_equal(atomic_load(&creads), 1);
	assert_int_equal(atomic_load(&sreads), 1);
	assert_int_equal(atomic_load(&ssends), 1);

	return teardown_test(state);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_POST) {
	atomic_store(&POST, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_GET) {
	atomic_store(&POST, false);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_POST_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, false);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_POST_quota) {
	atomic_store(&POST, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_GET_quota) {
	atomic_store(&POST, false);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_POST_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_one_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, false);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

static void
doh_connect_send_two_requests_cb(isc_nmhandle_t *handle, isc_result_t result,
				 void *arg) {
	REQUIRE(VALID_NMHANDLE(handle));
	if (result != ISC_R_SUCCESS) {
		return;
	}

	result = isc__nm_http_request(
		handle,
		&(isc_region_t){ .base = (uint8_t *)send_msg.base,
				 .length = send_msg.len },
		doh_receive_reply_cb, arg);
	if (result != ISC_R_SUCCESS) {
		return;
	}

	result = isc__nm_http_request(
		handle,
		&(isc_region_t){ .base = (uint8_t *)send_msg.base,
				 .length = send_msg.len },
		doh_receive_reply_cb, arg);
	if (result != ISC_R_SUCCESS) {
		return;
	}
}

static void
doh_recv_two(void *arg ISC_ATTR_UNUSED) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_nmsocket_t *listen_sock = NULL;
	char req_url[256];
	isc_tlsctx_t *ctx = NULL;
	isc_quota_t *quotap = init_listener_quota(workers);

	atomic_store(&total_sends, 2);
	expected_creads = 2;

	atomic_store(&nsends, atomic_load(&total_sends));

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   doh_receive_request_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = isc_nm_listenhttp(
		ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0, quotap,
		atomic_load(&use_TLS) ? server_tlsctx : NULL, endpoints, 0,
		get_proxy_type(), &listen_sock);
	assert_int_equal(result, ISC_R_SUCCESS);

	sockaddr_to_url(&tcp_listen_addr, atomic_load(&use_TLS), req_url,
			sizeof(req_url), ISC_NM_HTTP_DEFAULT_PATH);

	if (atomic_load(&use_TLS)) {
		ctx = client_tlsctx;
	}

	isc_nm_httpconnect(NULL, &tcp_listen_addr, req_url, atomic_load(&POST),
			   doh_connect_send_two_requests_cb, NULL, ctx, NULL,
			   client_sess_cache, 5000, get_proxy_type(), NULL);

	isc_loop_teardown(isc_loop_main(), listen_sock_close, listen_sock);
}

static int
doh_recv_two_teardown(void **state) {
	X(total_sends);
	X(csends);
	X(creads);
	X(sreads);
	X(ssends);

	assert_int_equal(atomic_load(&csends), 2);
	assert_int_equal(atomic_load(&creads), 2);
	assert_int_equal(atomic_load(&sreads), 2);
	assert_int_equal(atomic_load(&ssends), 2);

	return teardown_test(state);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_POST) {
	atomic_store(&POST, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_GET) {
	atomic_store(&POST, false);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_POST_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, false);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_POST_quota) {
	atomic_store(&POST, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_GET_quota) {
	atomic_store(&POST, false);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_POST_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_two_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&POST, false);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

static void
doh_recv_send(void *arg ISC_ATTR_UNUSED) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_nmsocket_t *listen_sock = NULL;
	size_t nthreads = isc_loopmgr_nloops();
	isc_quota_t *quotap = init_listener_quota(workers);

	atomic_store(&total_sends, 1000);
	atomic_store(&nsends, 1000);

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   doh_receive_request_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = isc_nm_listenhttp(
		ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0, quotap,
		atomic_load(&use_TLS) ? server_tlsctx : NULL, endpoints, 0,
		get_proxy_type(), &listen_sock);
	assert_int_equal(result, ISC_R_SUCCESS);

	for (size_t i = 0; i < nthreads; i++) {
		isc_async_run(isc_loop_get(i), doh_connect_thread, NULL);
	}

	isc_loop_teardown(isc_loop_main(), listen_sock_close, listen_sock);
}

static int
doh_recv_send_teardown(void **state) {
	int res = teardown_test(state);

	X(total_sends);
	X(csends);
	X(creads);
	X(sreads);
	X(ssends);

	CHECK_RANGE_FULL(csends);
	CHECK_RANGE_FULL(creads);
	CHECK_RANGE_FULL(sreads);
	CHECK_RANGE_FULL(ssends);

	return res;
}

ISC_LOOP_TEST_IMPL(doh_recv_send_POST) {
	atomic_store(&POST, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_send_GET) {
	atomic_store(&POST, false);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_send_POST_TLS) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_send_GET_TLS) {
	atomic_store(&POST, false);
	atomic_store(&use_TLS, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_send_POST_quota) {
	atomic_store(&POST, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_send_GET_quota) {
	atomic_store(&POST, false);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_send_POST_TLS_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(doh_recv_send_GET_TLS_quota) {
	atomic_store(&POST, false);
	atomic_store(&use_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

static int
doh_bad_connect_uri_teardown(void **state) {
	X(total_sends);
	X(csends);
	X(creads);
	X(sreads);
	X(ssends);

	/* As we used an ill-formed URI, there ought to be an error. */
	assert_int_equal(atomic_load(&csends), 0);
	assert_int_equal(atomic_load(&creads), 0);
	assert_int_equal(atomic_load(&sreads), 0);
	assert_int_equal(atomic_load(&ssends), 0);

	return teardown_test(state);
}

/* See: GL #2858, !5319 */
ISC_LOOP_TEST_IMPL(doh_bad_connect_uri) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_nmsocket_t *listen_sock = NULL;
	char req_url[256];
	isc_quota_t *quotap = init_listener_quota(workers);

	atomic_store(&total_sends, 1);

	atomic_store(&nsends, atomic_load(&total_sends));

	result = isc_nm_http_endpoints_add(endpoints, ISC_NM_HTTP_DEFAULT_PATH,
					   doh_receive_request_cb, NULL);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = isc_nm_listenhttp(ISC_NM_LISTEN_ALL, &tcp_listen_addr, 0,
				   quotap, server_tlsctx, endpoints, 0,
				   get_proxy_type(), &listen_sock);
	assert_int_equal(result, ISC_R_SUCCESS);

	/*
	 * "https://::1:XXXX/dns-query" is a bad URI, it should be
	 * "https://[::1]:XXXX/dns-query"
	 */
	(void)snprintf(req_url, sizeof(req_url), "https://::1:%u/%s",
		       isc_sockaddr_getport(&tcp_listen_addr),
		       ISC_NM_HTTP_DEFAULT_PATH);
	connect_send_request(req_url, atomic_load(&POST),
			     &(isc_region_t){ .base = (uint8_t *)send_msg.base,
					      .length = send_msg.len },
			     doh_receive_reply_cb, NULL, true, 30000);

	isc_loop_teardown(isc_loop_main(), listen_sock_close, listen_sock);
}

ISC_RUN_TEST_IMPL(doh_parse_GET_query_string) {
	/* valid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] =
			"dns=AAABAAABAAAAAAAAAWE-"
			"NjJjaGFyYWN0ZXJsYWJlbC1tYWtlcy1iYXNlNjR1cmwtZGlzdGluY3"
			"QtZnJvbS1zdGFuZGFyZC1iYXNlNjQHZXhhbXBsZQNjb20AAAEAAQ";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_true(ret);
		assert_non_null(queryp);
		assert_true(len > 0);
		assert_true(len == strlen(str) - 4);
		assert_true(memcmp(queryp, str + 4, len) == 0);
	}
	/* valid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] =
			"?dns=AAABAAABAAAAAAAAAWE-"
			"NjJjaGFyYWN0ZXJsYWJlbC1tYWtlcy1iYXNlNjR1cmwtZGlzdGluY3"
			"QtZnJvbS1zdGFuZGFyZC1iYXNlNjQHZXhhbXBsZQNjb20AAAEAAQ&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_true(ret);
		assert_non_null(queryp);
		assert_true(len > 0);
		assert_true(len == strlen(str) - 6);
		assert_true(memcmp(queryp, str + 5, len) == 0);
	}
	/* valid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=123&dns=567";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_true(ret);
		assert_non_null(queryp);
		assert_true(len > 0);
		assert_true(len == 3);
		assert_true(memcmp(queryp, "567", 3) == 0);
	}
	/* valid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?name1=123&dns=567&name2=123&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_true(ret);
		assert_non_null(queryp);
		assert_true(len > 0);
		assert_true(len == 3);
		assert_true(memcmp(queryp, "567", 3) == 0);
	}
	/* complex, but still valid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] =
			"?title=%D0%92%D1%96%D0%B4%D1%81%D0%BE%D1%82%D0%BA%D0%"
			"BE%D0%B2%D0%B5_%D0%BA%D0%BE%D0%B4%D1%83%D0%B2%D0%B0%"
			"D0%BD%D0%BD%D1%8F&dns=123&veaction=edit&section=0";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_true(ret);
		assert_non_null(queryp);
		assert_true(len > 0);
		assert_true(len == 3);
		assert_true(memcmp(queryp, "123", 3) == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] =
			"?title=%D0%92%D1%96%D0%B4%D1%81%D0%BE%D1%82%D0%BA%D0%"
			"BE%D0%B2%D0%B5_%D0%BA%D0%BE%D0%B4%D1%83%D0%B2%D0%B0%"
			"D0%BD%D0%BD%D1%8F&veaction=edit&section=0";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=123&&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* valid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=123%12&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_true(ret);
		assert_non_null(queryp);
		assert_true(len > 0);
		assert_true(len == 6);
		assert_true(memcmp(queryp, "123%12", 6) == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=123%ZZ&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=123%%&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* invalid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=123%AZ&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_false(ret);
		assert_null(queryp);
		assert_true(len == 0);
	}
	/* valid */
	{
		bool ret;
		const char *queryp = NULL;
		size_t len = 0;
		char str[] = "?dns=123%0AZ&";

		ret = isc__nm_parse_httpquery(str, &queryp, &len);
		assert_true(ret);
		assert_non_null(queryp);
		assert_true(len > 0);
		assert_true(len == 7);
		assert_true(memcmp(queryp, "123%0AZ", 7) == 0);
	}
}

ISC_RUN_TEST_IMPL(doh_base64url_to_base64) {
	char *res;
	size_t res_len = 0;
	/* valid */
	{
		char test[] = "YW55IGNhcm5hbCBwbGVhc3VyZS4";
		char res_test[] = "YW55IGNhcm5hbCBwbGVhc3VyZS4=";

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char test[] = "YW55IGNhcm5hbCBwbGVhcw";
		char res_test[] = "YW55IGNhcm5hbCBwbGVhcw==";

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char test[] = "YW55IGNhcm5hbCBwbGVhc3Vy";
		char res_test[] = "YW55IGNhcm5hbCBwbGVhc3Vy";

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char test[] = "YW55IGNhcm5hbCBwbGVhc3U";
		char res_test[] = "YW55IGNhcm5hbCBwbGVhc3U=";

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char test[] = "YW55IGNhcm5hbCBwbGVhcw";
		char res_test[] = "YW55IGNhcm5hbCBwbGVhcw==";

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char test[] = "PDw_Pz8-Pg";
		char res_test[] = "PDw/Pz8+Pg==";

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char test[] = "PDw_Pz8-Pg";
		char res_test[] = "PDw/Pz8+Pg==";

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  NULL);
		assert_non_null(res);
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* invalid */
	{
		char test[] = "YW55IGNhcm5hbCBwbGVhcw";
		res_len = 0;

		res = isc__nm_base64url_to_base64(mctx, test, 0, &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		char test[] = "";
		res_len = 0;

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		char test[] = "PDw_Pz8-Pg==";
		res_len = 0;

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		char test[] = "PDw_Pz8-Pg%3D%3D"; /* percent encoded "==" at the
						     end */
		res_len = 0;

		res = isc__nm_base64url_to_base64(mctx, test, strlen(test),
						  &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		res_len = 0;

		res = isc__nm_base64url_to_base64(mctx, NULL, 31231, &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
}

ISC_RUN_TEST_IMPL(doh_base64_to_base64url) {
	char *res;
	size_t res_len = 0;
	/* valid */
	{
		char res_test[] = "YW55IGNhcm5hbCBwbGVhc3VyZS4";
		char test[] = "YW55IGNhcm5hbCBwbGVhc3VyZS4=";

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char res_test[] = "YW55IGNhcm5hbCBwbGVhcw";
		char test[] = "YW55IGNhcm5hbCBwbGVhcw==";

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char res_test[] = "YW55IGNhcm5hbCBwbGVhc3Vy";
		char test[] = "YW55IGNhcm5hbCBwbGVhc3Vy";

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char res_test[] = "YW55IGNhcm5hbCBwbGVhc3U";
		char test[] = "YW55IGNhcm5hbCBwbGVhc3U=";

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char res_test[] = "YW55IGNhcm5hbCBwbGVhcw";
		char test[] = "YW55IGNhcm5hbCBwbGVhcw==";

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char res_test[] = "PDw_Pz8-Pg";
		char test[] = "PDw/Pz8+Pg==";

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_non_null(res);
		assert_true(res_len == strlen(res_test));
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* valid */
	{
		char res_test[] = "PDw_Pz8-Pg";
		char test[] = "PDw/Pz8+Pg==";

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  NULL);
		assert_non_null(res);
		assert_true(strcmp(res, res_test) == 0);
		isc_mem_free(mctx, res);
	}
	/* invalid */
	{
		char test[] = "YW55IGNhcm5hbCBwbGVhcw";
		res_len = 0;

		res = isc__nm_base64_to_base64url(mctx, test, 0, &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		char test[] = "";
		res_len = 0;

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		char test[] = "PDw_Pz8-Pg==";
		res_len = 0;

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		char test[] = "PDw_Pz8-Pg%3D%3D"; /* percent encoded "==" at the
						     end */
		res_len = 0;

		res = isc__nm_base64_to_base64url(mctx, test, strlen(test),
						  &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
	/* invalid */
	{
		res_len = 0;

		res = isc__nm_base64_to_base64url(mctx, NULL, 31231, &res_len);
		assert_null(res);
		assert_true(res_len == 0);
	}
}

ISC_RUN_TEST_IMPL(doh_path_validation) {
	assert_true(isc_nm_http_path_isvalid("/"));
	assert_true(isc_nm_http_path_isvalid(ISC_NM_HTTP_DEFAULT_PATH));
	assert_false(isc_nm_http_path_isvalid("laaaa"));
	assert_false(isc_nm_http_path_isvalid(""));
	assert_false(isc_nm_http_path_isvalid("//"));
	assert_true(isc_nm_http_path_isvalid("/lala///"));
	assert_true(isc_nm_http_path_isvalid("/lalaaaaaa"));
	assert_true(isc_nm_http_path_isvalid("/lalaaa/la/la/la"));
	assert_true(isc_nm_http_path_isvalid("/la/a"));
	assert_true(isc_nm_http_path_isvalid("/la+la"));
	assert_true(isc_nm_http_path_isvalid("/la&la/la*la/l-a_/la!/la\'"));
	assert_true(isc_nm_http_path_isvalid("/la/(la)/la"));
	assert_true(isc_nm_http_path_isvalid("/la,la,la"));
	assert_true(isc_nm_http_path_isvalid("/la-'la'-la"));
	assert_true(isc_nm_http_path_isvalid("/la:la=la"));
	assert_true(isc_nm_http_path_isvalid("/l@l@l@"));
	assert_false(isc_nm_http_path_isvalid("/#lala"));
	assert_true(isc_nm_http_path_isvalid("/lala;la"));
	assert_false(
		isc_nm_http_path_isvalid("la&la/laalaala*lala/l-al_a/lal!/"));
	assert_true(isc_nm_http_path_isvalid("/Lal/lAla.jpg"));

	/* had to replace ? with ! because it does not verify a query string */
	assert_true(isc_nm_http_path_isvalid("/watch!v=oavMtUWDBTM"));
	assert_false(isc_nm_http_path_isvalid("/watch?v=dQw4w9WgXcQ"));
	assert_true(isc_nm_http_path_isvalid("/datatracker.ietf.org/doc/html/"
					     "rfc2616"));
	assert_true(isc_nm_http_path_isvalid("/doc/html/rfc8484"));
	assert_true(isc_nm_http_path_isvalid("/123"));
}

ISC_RUN_TEST_IMPL(doh_connect_makeuri) {
	struct in_addr localhostv4 = { .s_addr = ntohl(INADDR_LOOPBACK) };
	isc_sockaddr_t sa;
	char uri[256];

	/* Firstly, test URI generation using isc_sockaddr_t */
	isc_sockaddr_fromin(&sa, &localhostv4, 0);
	uri[0] = '\0';
	isc_nm_http_makeuri(true, &sa, NULL, 0, ISC_NM_HTTP_DEFAULT_PATH, uri,
			    sizeof(uri));
	assert_true(strcmp("https://127.0.0.1:443/dns-query", uri) == 0);

	uri[0] = '\0';
	isc_nm_http_makeuri(false, &sa, NULL, 0, ISC_NM_HTTP_DEFAULT_PATH, uri,
			    sizeof(uri));
	assert_true(strcmp("http://127.0.0.1:80/dns-query", uri) == 0);

	/*
	 * The port value should be ignored, because we can get one from
	 * the isc_sockaddr_t object.
	 */
	uri[0] = '\0';
	isc_nm_http_makeuri(true, &sa, NULL, 44343, ISC_NM_HTTP_DEFAULT_PATH,
			    uri, sizeof(uri));
	assert_true(strcmp("https://127.0.0.1:443/dns-query", uri) == 0);

	uri[0] = '\0';
	isc_nm_http_makeuri(false, &sa, NULL, 8080, ISC_NM_HTTP_DEFAULT_PATH,
			    uri, sizeof(uri));
	assert_true(strcmp("http://127.0.0.1:80/dns-query", uri) == 0);

	/* IPv6 */
	isc_sockaddr_fromin6(&sa, &in6addr_loopback, 0);
	uri[0] = '\0';
	isc_nm_http_makeuri(true, &sa, NULL, 0, ISC_NM_HTTP_DEFAULT_PATH, uri,
			    sizeof(uri));
	assert_true(strcmp("https://[::1]:443/dns-query", uri) == 0);

	uri[0] = '\0';
	isc_nm_http_makeuri(false, &sa, NULL, 0, ISC_NM_HTTP_DEFAULT_PATH, uri,
			    sizeof(uri));
	assert_true(strcmp("http://[::1]:80/dns-query", uri) == 0);

	/*
	 * The port value should be ignored, because we can get one from
	 * the isc_sockaddr_t object.
	 */
	uri[0] = '\0';
	isc_nm_http_makeuri(true, &sa, NULL, 44343, ISC_NM_HTTP_DEFAULT_PATH,
			    uri, sizeof(uri));
	assert_true(strcmp("https://[::1]:443/dns-query", uri) == 0);

	uri[0] = '\0';
	isc_nm_http_makeuri(false, &sa, NULL, 8080, ISC_NM_HTTP_DEFAULT_PATH,
			    uri, sizeof(uri));
	assert_true(strcmp("http://[::1]:80/dns-query", uri) == 0);

	/* Try to set the port numbers. */
	isc_sockaddr_setport(&sa, 44343);
	uri[0] = '\0';
	isc_nm_http_makeuri(true, &sa, NULL, 0, ISC_NM_HTTP_DEFAULT_PATH, uri,
			    sizeof(uri));
	assert_true(strcmp("https://[::1]:44343/dns-query", uri) == 0);

	isc_sockaddr_setport(&sa, 8080);
	uri[0] = '\0';
	isc_nm_http_makeuri(false, &sa, NULL, 0, ISC_NM_HTTP_DEFAULT_PATH, uri,
			    sizeof(uri));
	assert_true(strcmp("http://[::1]:8080/dns-query", uri) == 0);

	/*
	 * Try to make a URI using a hostname and a port number. The
	 * isc_sockaddr_t object will be ignored.
	 */
	isc_sockaddr_any(&sa);
	uri[0] = '\0';
	isc_nm_http_makeuri(true, &sa, "example.com", 0,
			    ISC_NM_HTTP_DEFAULT_PATH, uri, sizeof(uri));
	assert_true(strcmp("https://example.com:443/dns-query", uri) == 0);

	uri[0] = '\0';
	isc_nm_http_makeuri(false, &sa, "example.com", 0,
			    ISC_NM_HTTP_DEFAULT_PATH, uri, sizeof(uri));
	assert_true(strcmp("http://example.com:80/dns-query", uri) == 0);

	/* Try to set the port numbers. */
	isc_sockaddr_setport(&sa, 443);
	uri[0] = '\0';
	isc_nm_http_makeuri(true, &sa, "example.com", 44343,
			    ISC_NM_HTTP_DEFAULT_PATH, uri, sizeof(uri));
	assert_true(strcmp("https://example.com:44343/dns-query", uri) == 0);

	isc_sockaddr_setport(&sa, 80);
	uri[0] = '\0';
	isc_nm_http_makeuri(false, &sa, "example.com", 8080,
			    ISC_NM_HTTP_DEFAULT_PATH, uri, sizeof(uri));
	assert_true(strcmp("http://example.com:8080/dns-query", uri) == 0);

	/* IPv4 as the hostname - nothing fancy here */
	uri[0] = '\0';
	isc_nm_http_makeuri(false, NULL, "127.0.0.1", 8080,
			    ISC_NM_HTTP_DEFAULT_PATH, uri, sizeof(uri));
	assert_true(strcmp("http://127.0.0.1:8080/dns-query", uri) == 0);

	uri[0] = '\0';
	isc_nm_http_makeuri(true, NULL, "127.0.0.1", 44343,
			    ISC_NM_HTTP_DEFAULT_PATH, uri, sizeof(uri));
	assert_true(strcmp("https://127.0.0.1:44343/dns-query", uri) == 0);

	/*
	 * A peculiar edge case: IPv6 given as the hostname (notice
	 * the brackets)
	 */
	uri[0] = '\0';
	isc_nm_http_makeuri(false, NULL, "::1", 8080, ISC_NM_HTTP_DEFAULT_PATH,
			    uri, sizeof(uri));
	assert_true(strcmp("http://[::1]:8080/dns-query", uri) == 0);

	uri[0] = '\0';
	isc_nm_http_makeuri(true, NULL, "[::1]", 44343,
			    ISC_NM_HTTP_DEFAULT_PATH, uri, sizeof(uri));
	assert_true(strcmp("https://[::1]:44343/dns-query", uri) == 0);
}

/* PROXY */
ISC_LOOP_TEST_IMPL(proxy_doh_noop_POST) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	doh_noop(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_noop_GET) {
	atomic_store(&use_PROXY, true);
	doh_noop(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_noresponse_POST) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	doh_noresponse(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_noresponse_GET) {
	atomic_store(&use_PROXY, true);
	doh_noresponse(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_timeout_recovery_POST) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	doh_timeout_recovery(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_timeout_recovery_GET) {
	atomic_store(&use_PROXY, true);
	doh_timeout_recovery(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_POST) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_GET) {
	atomic_store(&use_PROXY, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_POST_TLS) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_POST_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_GET_quota) {
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_one_POST_TLS_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_POST) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_GET) {
	;
	atomic_store(&use_PROXY, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_POST_TLS) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_POST_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_GET_quota) {
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_POST_TLS_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_two_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_POST) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_GET) {
	atomic_store(&use_PROXY, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_POST_TLS) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_POST_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_GET_quota) {
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_POST_TLS_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxy_doh_recv_send_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

/* PROXY over TLS */

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_one_POST_TLS) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_one_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_one_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_one_POST_TLS_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_one(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_two_POST_TLS) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_two_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_two_POST_TLS_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_two_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_two(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_send_POST_TLS) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_send_GET_TLS) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_send_POST_TLS_quota) {
	atomic_store(&POST, true);
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_LOOP_TEST_IMPL(proxytls_doh_recv_send_GET_TLS_quota) {
	atomic_store(&use_TLS, true);
	atomic_store(&use_PROXY, true);
	atomic_store(&use_PROXY_over_TLS, true);
	atomic_store(&check_listener_quota, true);
	doh_recv_send(arg);
}

ISC_TEST_LIST_START

ISC_TEST_ENTRY_CUSTOM(mock_doh_uv_tcp_bind, setup_test, teardown_test)
ISC_TEST_ENTRY(doh_parse_GET_query_string)
ISC_TEST_ENTRY(doh_base64url_to_base64)
ISC_TEST_ENTRY(doh_base64_to_base64url)
ISC_TEST_ENTRY(doh_path_validation)
ISC_TEST_ENTRY(doh_connect_makeuri)
ISC_TEST_ENTRY_CUSTOM(doh_noop_POST, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(doh_noop_GET, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(doh_noresponse_POST, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(doh_noresponse_GET, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(doh_timeout_recovery_POST, setup_test,
		      doh_timeout_recovery_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_timeout_recovery_GET, setup_test,
		      doh_timeout_recovery_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_POST, setup_test, doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_GET, setup_test, doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_POST_TLS, setup_test, doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_GET_TLS, setup_test, doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_POST_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_GET_quota, setup_test, doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_POST_TLS_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_one_GET_TLS_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_POST, setup_test, doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_GET, setup_test, doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_POST_TLS, setup_test, doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_GET_TLS, setup_test, doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_POST_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_GET_quota, setup_test, doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_POST_TLS_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_two_GET_TLS_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_GET, setup_test, doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_POST, setup_test, doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_GET_TLS, setup_test, doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_POST_TLS, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_GET_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_POST_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_GET_TLS_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_recv_send_POST_TLS_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(doh_bad_connect_uri, setup_test,
		      doh_bad_connect_uri_teardown)
/* PROXY */
ISC_TEST_ENTRY_CUSTOM(proxy_doh_noop_POST, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_noop_GET, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_noresponse_POST, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_noresponse_GET, setup_test, teardown_test)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_timeout_recovery_POST, setup_test,
		      doh_timeout_recovery_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_timeout_recovery_GET, setup_test,
		      doh_timeout_recovery_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_POST, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_GET, setup_test, doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_POST_TLS, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_GET_TLS, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_POST_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_GET_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_POST_TLS_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_one_GET_TLS_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_POST, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_GET, setup_test, doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_POST_TLS, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_GET_TLS, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_POST_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_GET_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_POST_TLS_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_two_GET_TLS_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_GET, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_POST, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_GET_TLS, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_POST_TLS, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_GET_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_POST_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_GET_TLS_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxy_doh_recv_send_POST_TLS_quota, setup_test,
		      doh_recv_send_teardown)
/* PROXY over TLS */
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_one_POST_TLS, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_one_GET_TLS, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_one_POST_TLS_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_one_GET_TLS_quota, setup_test,
		      doh_recv_one_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_two_POST_TLS, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_two_GET_TLS, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_two_POST_TLS_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_two_GET_TLS_quota, setup_test,
		      doh_recv_two_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_send_GET_TLS, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_send_POST_TLS, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_send_GET_TLS_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_ENTRY_CUSTOM(proxytls_doh_recv_send_POST_TLS_quota, setup_test,
		      doh_recv_send_teardown)
ISC_TEST_LIST_END

ISC_TEST_MAIN
