include $(top_srcdir)/Makefile.top

AM_CPPFLAGS +=			\
	$(LIBISC_CFLAGS)	\
	$(LIBDNS_CFLAGS)	\
	$(LIBNS_CFLAGS)		\
	$(LIBUV_CFLAGS)		\
	-I$(top_srcdir)/lib/isc	\
	-I$(top_srcdir)/lib/dns

LDADD +=			\
	$(LIBISC_LIBS)		\
	$(LIBDNS_LIBS)		\
	$(LIBNS_LIBS)		\
	$(LIBUV_LIBS)

check_PROGRAMS =		\
	notify_test		\
	plugin_test		\
	query_test

notify_test_SOURCES =		\
	notify_test.c		\
	netmgr_wrap.c

query_test_SOURCES =		\
	query_test.c		\
	netmgr_wrap.c

EXTRA_DIST = testdata

include $(top_srcdir)/Makefile.tests
