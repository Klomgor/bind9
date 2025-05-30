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

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <isc/dir.h>
#include <isc/file.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/overflow.h>
#include <isc/result.h>
#include <isc/serial.h>
#include <isc/stdio.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/compress.h>
#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/fixedname.h>
#include <dns/journal.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/soa.h>

/*! \file
 * \brief Journaling.
 *
 * A journal file consists of
 *
 *   \li A fixed-size header of type journal_rawheader_t.
 *
 *   \li The index.  This is an unordered array of index entries
 *     of type journal_rawpos_t giving the locations
 *     of some arbitrary subset of the journal's addressable
 *     transactions.  The index entries are used as hints to
 *     speed up the process of locating a transaction with a given
 *     serial number.  Unused index entries have an "offset"
 *     field of zero.  The size of the index can vary between
 *     journal files, but does not change during the lifetime
 *     of a file.  The size can be zero.
 *
 *   \li The journal data.  This  consists of one or more transactions.
 *     Each transaction begins with a transaction header of type
 *     journal_rawxhdr_t.  The transaction header is followed by a
 *     sequence of RRs, similar in structure to an IXFR difference
 *     sequence (RFC1995).  That is, the pre-transaction SOA,
 *     zero or more other deleted RRs, the post-transaction SOA,
 *     and zero or more other added RRs.  Unlike in IXFR, each RR
 *     is prefixed with a 32-bit length.
 *
 *     The journal data part grows as new transactions are
 *     appended to the file.  Only those transactions
 *     whose serial number is current-(2^31-1) to current
 *     are considered "addressable" and may be pointed
 *     to from the header or index.  They may be preceded
 *     by old transactions that are no longer addressable,
 *     and they may be followed by transactions that were
 *     appended to the journal but never committed by updating
 *     the "end" position in the header.  The latter will
 *     be overwritten when new transactions are added.
 */

/**************************************************************************/
/*
 * Miscellaneous utilities.
 */

/*%
 * It would be non-sensical (or at least obtuse) to use FAIL() with an
 * ISC_R_SUCCESS code, but the test is there to keep the Solaris compiler
 * from complaining about "end-of-loop code not reached".
 */
#define FAIL(code)                           \
	do {                                 \
		result = (code);             \
		if (result != ISC_R_SUCCESS) \
			goto failure;        \
	} while (0)

#define CHECK(op)                            \
	do {                                 \
		result = (op);               \
		if (result != ISC_R_SUCCESS) \
			goto failure;        \
	} while (0)

#define JOURNAL_SERIALSET 0x01U

static isc_result_t
index_to_disk(dns_journal_t *);

static uint32_t
decode_uint32(unsigned char *p) {
	return ((uint32_t)p[0] << 24) + ((uint32_t)p[1] << 16) +
	       ((uint32_t)p[2] << 8) + ((uint32_t)p[3] << 0);
}

static void
encode_uint32(uint32_t val, unsigned char *p) {
	p[0] = (uint8_t)(val >> 24);
	p[1] = (uint8_t)(val >> 16);
	p[2] = (uint8_t)(val >> 8);
	p[3] = (uint8_t)(val >> 0);
}

isc_result_t
dns_db_createsoatuple(dns_db_t *db, dns_dbversion_t *ver, isc_mem_t *mctx,
		      dns_diffop_t op, dns_difftuple_t **tp) {
	isc_result_t result;
	dns_dbnode_t *node;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_fixedname_t fixed;
	dns_name_t *zonename;

	zonename = dns_fixedname_initname(&fixed);
	dns_name_copy(dns_db_origin(db), zonename);

	node = NULL;
	result = dns_db_findnode(db, zonename, false, &node);
	if (result != ISC_R_SUCCESS) {
		goto nonode;
	}

	dns_rdataset_init(&rdataset);
	result = dns_db_findrdataset(db, node, ver, dns_rdatatype_soa, 0,
				     (isc_stdtime_t)0, &rdataset, NULL);
	if (result != ISC_R_SUCCESS) {
		goto freenode;
	}

	result = dns_rdataset_first(&rdataset);
	if (result != ISC_R_SUCCESS) {
		goto freenode;
	}

	dns_rdataset_current(&rdataset, &rdata);
	dns_rdataset_getownercase(&rdataset, zonename);

	dns_difftuple_create(mctx, op, zonename, rdataset.ttl, &rdata, tp);

	dns_rdataset_disassociate(&rdataset);
	dns_db_detachnode(db, &node);
	return result;

freenode:
	dns_db_detachnode(db, &node);
nonode:
	UNEXPECTED_ERROR("missing SOA");
	return result;
}

/* Journaling */

/*%
 * On-disk representation of a "pointer" to a journal entry.
 * These are used in the journal header to locate the beginning
 * and end of the journal, and in the journal index to locate
 * other transactions.
 */
typedef struct {
	unsigned char serial[4]; /*%< SOA serial before update. */
	/*
	 * XXXRTH  Should offset be 8 bytes?
	 * XXXDCL ... probably, since off_t is 8 bytes on many OSs.
	 * XXXAG  ... but we will not be able to seek >2G anyway on many
	 *            platforms as long as we are using fseek() rather
	 *            than lseek().
	 */
	unsigned char offset[4]; /*%< Offset from beginning of file. */
} journal_rawpos_t;

/*%
 * The header is of a fixed size, with some spare room for future
 * extensions.
 */
#define JOURNAL_HEADER_SIZE 64 /* Bytes. */

typedef enum {
	XHDR_VERSION1 = 1,
	XHDR_VERSION2 = 2,
} xhdr_version_t;

/*%
 * The on-disk representation of the journal header.
 * All numbers are stored in big-endian order.
 */
typedef union {
	struct {
		/*% File format version ID. */
		unsigned char format[16];
		/*% Position of the first addressable transaction */
		journal_rawpos_t begin;
		/*% Position of the next (yet nonexistent) transaction. */
		journal_rawpos_t end;
		/*% Number of index entries following the header. */
		unsigned char index_size[4];
		/*% Source serial number. */
		unsigned char sourceserial[4];
		unsigned char flags;
	} h;
	/* Pad the header to a fixed size. */
	unsigned char pad[JOURNAL_HEADER_SIZE];
} journal_rawheader_t;

/*%
 * The on-disk representation of the transaction header, version 2.
 * There is one of these at the beginning of each transaction.
 */
typedef struct {
	unsigned char size[4];	  /*%< In bytes, excluding header. */
	unsigned char count[4];	  /*%< Number of records in transaction */
	unsigned char serial0[4]; /*%< SOA serial before update. */
	unsigned char serial1[4]; /*%< SOA serial after update. */
} journal_rawxhdr_t;

/*%
 * Old-style raw transaction header, version 1, used for backward
 * compatibility mode.
 */
typedef struct {
	unsigned char size[4];
	unsigned char serial0[4];
	unsigned char serial1[4];
} journal_rawxhdr_ver1_t;

/*%
 * The on-disk representation of the RR header.
 * There is one of these at the beginning of each RR.
 */
typedef struct {
	unsigned char size[4]; /*%< In bytes, excluding header. */
} journal_rawrrhdr_t;

/*%
 * The in-core representation of the journal header.
 */
typedef struct {
	uint32_t serial;
	off_t offset;
} journal_pos_t;

#define POS_VALID(pos)	    ((pos).offset != 0)
#define POS_INVALIDATE(pos) ((pos).offset = 0, (pos).serial = 0)

typedef struct {
	unsigned char format[16];
	journal_pos_t begin;
	journal_pos_t end;
	uint32_t index_size;
	uint32_t sourceserial;
	bool serialset;
} journal_header_t;

/*%
 * The in-core representation of the transaction header.
 */
typedef struct {
	uint32_t size;
	uint32_t count;
	uint32_t serial0;
	uint32_t serial1;
} journal_xhdr_t;

/*%
 * The in-core representation of the RR header.
 */
typedef struct {
	uint32_t size;
} journal_rrhdr_t;

/*%
 * Initial contents to store in the header of a newly created
 * journal file.
 *
 * The header starts with the magic string ";BIND LOG V9.2\n"
 * to identify the file as a BIND 9 journal file.  An ASCII
 * identification string is used rather than a binary magic
 * number to be consistent with BIND 8 (BIND 8 journal files
 * are ASCII text files).
 */

static journal_header_t journal_header_ver1 = {
	";BIND LOG V9\n", { 0, 0 }, { 0, 0 }, 0, 0, 0
};
static journal_header_t initial_journal_header = {
	";BIND LOG V9.2\n", { 0, 0 }, { 0, 0 }, 0, 0, 0
};

#define JOURNAL_EMPTY(h) ((h)->begin.offset == (h)->end.offset)

typedef enum {
	JOURNAL_STATE_INVALID,
	JOURNAL_STATE_READ,
	JOURNAL_STATE_WRITE,
	JOURNAL_STATE_TRANSACTION,
	JOURNAL_STATE_INLINE
} journal_state_t;

struct dns_journal {
	unsigned int magic; /*%< JOUR */
	isc_mem_t *mctx;    /*%< Memory context */
	journal_state_t state;
	xhdr_version_t xhdr_version; /*%< Expected transaction header version */
	bool header_ver1;	     /*%< Transaction header compatibility
				      *   mode is allowed */
	bool recovered;		     /*%< A recoverable error was found
				      *   while reading the journal */
	char *filename;		     /*%< Journal file name */
	FILE *fp;		     /*%< File handle */
	off_t offset;		     /*%< Current file offset */
	journal_xhdr_t curxhdr;	     /*%< Current transaction header */
	journal_header_t header;     /*%< In-core journal header */
	unsigned char *rawindex;     /*%< In-core buffer for journal index
				      * in on-disk format */
	journal_pos_t *index;	     /*%< In-core journal index */

	/*% Current transaction state (when writing). */
	struct {
		unsigned int n_soa;   /*%< Number of SOAs seen */
		unsigned int n_rr;    /*%< Number of RRs to write */
		journal_pos_t pos[2]; /*%< Begin/end position */
	} x;

	/*% Iteration state (when reading). */
	struct {
		/* These define the part of the journal we iterate over. */
		journal_pos_t bpos; /*%< Position before first, */
		journal_pos_t cpos; /*%< before current, */
		journal_pos_t epos; /*%< and after last transaction */
		/* The rest is iterator state. */
		uint32_t current_serial; /*%< Current SOA serial */
		isc_buffer_t source;	 /*%< Data from disk */
		isc_buffer_t target;	 /*%< Data from _fromwire check */
		dns_decompress_t dctx;	 /*%< Dummy decompression ctx */
		dns_name_t name;	 /*%< Current domain name */
		dns_rdata_t rdata;	 /*%< Current rdata */
		uint32_t ttl;		 /*%< Current TTL */
		unsigned int xsize;	 /*%< Size of transaction data */
		unsigned int xpos;	 /*%< Current position in it */
		isc_result_t result;	 /*%< Result of last call */
	} it;
};

#define DNS_JOURNAL_MAGIC    ISC_MAGIC('J', 'O', 'U', 'R')
#define DNS_JOURNAL_VALID(t) ISC_MAGIC_VALID(t, DNS_JOURNAL_MAGIC)

static void
journal_pos_decode(journal_rawpos_t *raw, journal_pos_t *cooked) {
	cooked->serial = decode_uint32(raw->serial);
	cooked->offset = decode_uint32(raw->offset);
}

static void
journal_pos_encode(journal_rawpos_t *raw, journal_pos_t *cooked) {
	encode_uint32(cooked->serial, raw->serial);
	encode_uint32(cooked->offset, raw->offset);
}

static void
journal_header_decode(journal_rawheader_t *raw, journal_header_t *cooked) {
	INSIST(sizeof(cooked->format) == sizeof(raw->h.format));

	memmove(cooked->format, raw->h.format, sizeof(cooked->format));
	journal_pos_decode(&raw->h.begin, &cooked->begin);
	journal_pos_decode(&raw->h.end, &cooked->end);
	cooked->index_size = decode_uint32(raw->h.index_size);
	cooked->sourceserial = decode_uint32(raw->h.sourceserial);
	cooked->serialset = ((raw->h.flags & JOURNAL_SERIALSET) != 0);
}

static void
journal_header_encode(journal_header_t *cooked, journal_rawheader_t *raw) {
	unsigned char flags = 0;

	INSIST(sizeof(cooked->format) == sizeof(raw->h.format));

	memset(raw->pad, 0, sizeof(raw->pad));
	memmove(raw->h.format, cooked->format, sizeof(raw->h.format));
	journal_pos_encode(&raw->h.begin, &cooked->begin);
	journal_pos_encode(&raw->h.end, &cooked->end);
	encode_uint32(cooked->index_size, raw->h.index_size);
	encode_uint32(cooked->sourceserial, raw->h.sourceserial);
	if (cooked->serialset) {
		flags |= JOURNAL_SERIALSET;
	}
	raw->h.flags = flags;
}

/*
 * Journal file I/O subroutines, with error checking and reporting.
 */
static isc_result_t
journal_seek(dns_journal_t *j, uint32_t offset) {
	isc_result_t result;

	result = isc_stdio_seek(j->fp, (off_t)offset, SEEK_SET);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: seek: %s", j->filename,
			      isc_result_totext(result));
		return ISC_R_UNEXPECTED;
	}
	j->offset = offset;
	return ISC_R_SUCCESS;
}

static isc_result_t
journal_read(dns_journal_t *j, void *mem, size_t nbytes) {
	isc_result_t result;

	result = isc_stdio_read(mem, 1, nbytes, j->fp, NULL);
	if (result != ISC_R_SUCCESS) {
		if (result == ISC_R_EOF) {
			return ISC_R_NOMORE;
		}
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: read: %s", j->filename,
			      isc_result_totext(result));
		return ISC_R_UNEXPECTED;
	}
	j->offset += (off_t)nbytes;
	return ISC_R_SUCCESS;
}

static isc_result_t
journal_write(dns_journal_t *j, void *mem, size_t nbytes) {
	isc_result_t result;

	result = isc_stdio_write(mem, 1, nbytes, j->fp, NULL);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: write: %s", j->filename,
			      isc_result_totext(result));
		return ISC_R_UNEXPECTED;
	}
	j->offset += (off_t)nbytes;
	return ISC_R_SUCCESS;
}

static isc_result_t
journal_fsync(dns_journal_t *j) {
	isc_result_t result;

	result = isc_stdio_flush(j->fp);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: flush: %s", j->filename,
			      isc_result_totext(result));
		return ISC_R_UNEXPECTED;
	}
	result = isc_stdio_sync(j->fp);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: fsync: %s", j->filename,
			      isc_result_totext(result));
		return ISC_R_UNEXPECTED;
	}
	return ISC_R_SUCCESS;
}

/*
 * Read/write a transaction header at the current file position.
 */
static isc_result_t
journal_read_xhdr(dns_journal_t *j, journal_xhdr_t *xhdr) {
	isc_result_t result;

	j->it.cpos.offset = j->offset;

	switch (j->xhdr_version) {
	case XHDR_VERSION1: {
		journal_rawxhdr_ver1_t raw;
		result = journal_read(j, &raw, sizeof(raw));
		if (result != ISC_R_SUCCESS) {
			return result;
		}
		xhdr->size = decode_uint32(raw.size);
		xhdr->count = 0;
		xhdr->serial0 = decode_uint32(raw.serial0);
		xhdr->serial1 = decode_uint32(raw.serial1);
		j->curxhdr = *xhdr;
		return ISC_R_SUCCESS;
	}

	case XHDR_VERSION2: {
		journal_rawxhdr_t raw;
		result = journal_read(j, &raw, sizeof(raw));
		if (result != ISC_R_SUCCESS) {
			return result;
		}
		xhdr->size = decode_uint32(raw.size);
		xhdr->count = decode_uint32(raw.count);
		xhdr->serial0 = decode_uint32(raw.serial0);
		xhdr->serial1 = decode_uint32(raw.serial1);
		j->curxhdr = *xhdr;
		return ISC_R_SUCCESS;
	}

	default:
		return ISC_R_NOTIMPLEMENTED;
	}
}

static isc_result_t
journal_write_xhdr(dns_journal_t *j, uint32_t size, uint32_t count,
		   uint32_t serial0, uint32_t serial1) {
	if (j->header_ver1) {
		journal_rawxhdr_ver1_t raw;
		encode_uint32(size, raw.size);
		encode_uint32(serial0, raw.serial0);
		encode_uint32(serial1, raw.serial1);
		return journal_write(j, &raw, sizeof(raw));
	} else {
		journal_rawxhdr_t raw;
		encode_uint32(size, raw.size);
		encode_uint32(count, raw.count);
		encode_uint32(serial0, raw.serial0);
		encode_uint32(serial1, raw.serial1);
		return journal_write(j, &raw, sizeof(raw));
	}
}

/*
 * Read an RR header at the current file position.
 */

static isc_result_t
journal_read_rrhdr(dns_journal_t *j, journal_rrhdr_t *rrhdr) {
	journal_rawrrhdr_t raw;
	isc_result_t result;

	result = journal_read(j, &raw, sizeof(raw));
	if (result != ISC_R_SUCCESS) {
		return result;
	}
	rrhdr->size = decode_uint32(raw.size);
	return ISC_R_SUCCESS;
}

static isc_result_t
journal_file_create(isc_mem_t *mctx, bool downgrade, const char *filename) {
	FILE *fp = NULL;
	isc_result_t result;
	journal_header_t header;
	journal_rawheader_t rawheader;
	int index_size = 56; /* XXX configurable */
	int size;
	void *mem = NULL; /* Memory for temporary index image. */

	INSIST(sizeof(journal_rawheader_t) == JOURNAL_HEADER_SIZE);

	result = isc_stdio_open(filename, "wb", &fp);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: create: %s", filename,
			      isc_result_totext(result));
		return ISC_R_UNEXPECTED;
	}

	if (downgrade) {
		header = journal_header_ver1;
	} else {
		header = initial_journal_header;
	}
	header.index_size = index_size;
	journal_header_encode(&header, &rawheader);

	size = sizeof(journal_rawheader_t) +
	       ISC_CHECKED_MUL(index_size, sizeof(journal_rawpos_t));

	mem = isc_mem_cget(mctx, 1, size);
	memmove(mem, &rawheader, sizeof(rawheader));

	result = isc_stdio_write(mem, 1, (size_t)size, fp, NULL);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: write: %s", filename,
			      isc_result_totext(result));
		(void)isc_stdio_close(fp);
		(void)isc_file_remove(filename);
		isc_mem_put(mctx, mem, size);
		return ISC_R_UNEXPECTED;
	}
	isc_mem_put(mctx, mem, size);

	result = isc_stdio_close(fp);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: close: %s", filename,
			      isc_result_totext(result));
		(void)isc_file_remove(filename);
		return ISC_R_UNEXPECTED;
	}

	return ISC_R_SUCCESS;
}

static isc_result_t
journal_open(isc_mem_t *mctx, const char *filename, bool writable, bool create,
	     bool downgrade, dns_journal_t **journalp) {
	FILE *fp = NULL;
	isc_result_t result;
	journal_rawheader_t rawheader;
	dns_journal_t *j;

	REQUIRE(journalp != NULL && *journalp == NULL);

	j = isc_mem_get(mctx, sizeof(*j));
	*j = (dns_journal_t){ .state = JOURNAL_STATE_INVALID,
			      .filename = isc_mem_strdup(mctx, filename),
			      .xhdr_version = XHDR_VERSION2 };
	isc_mem_attach(mctx, &j->mctx);

	result = isc_stdio_open(j->filename, writable ? "rb+" : "rb", &fp);
	if (result == ISC_R_FILENOTFOUND) {
		if (create) {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_DEBUG(1),
				      "journal file %s does not exist, "
				      "creating it",
				      j->filename);
			CHECK(journal_file_create(mctx, downgrade, filename));
			/*
			 * Retry.
			 */
			result = isc_stdio_open(j->filename, "rb+", &fp);
		} else {
			FAIL(ISC_R_NOTFOUND);
		}
	}
	if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: open: %s", j->filename,
			      isc_result_totext(result));
		FAIL(ISC_R_UNEXPECTED);
	}

	j->fp = fp;

	/*
	 * Set magic early so that seek/read can succeed.
	 */
	j->magic = DNS_JOURNAL_MAGIC;

	CHECK(journal_seek(j, 0));
	CHECK(journal_read(j, &rawheader, sizeof(rawheader)));

	if (memcmp(rawheader.h.format, journal_header_ver1.format,
		   sizeof(journal_header_ver1.format)) == 0)
	{
		/*
		 * The file header says it's the old format, but it
		 * still might have the new xhdr format because we
		 * forgot to change the format string when we introduced
		 * the new xhdr.  When we first try to read it, we assume
		 * it uses the new xhdr format. If that fails, we'll be
		 * called a second time with compat set to true, in which
		 * case we can lower xhdr_version to 1 if we find a
		 * corrupt transaction.
		 */
		j->header_ver1 = true;
	} else if (memcmp(rawheader.h.format, initial_journal_header.format,
			  sizeof(initial_journal_header.format)) == 0)
	{
		/*
		 * File header says this is format version 2; all
		 * transactions have to match.
		 */
		j->header_ver1 = false;
	} else {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "%s: journal format not recognized", j->filename);
		FAIL(ISC_R_UNEXPECTED);
	}
	journal_header_decode(&rawheader, &j->header);

	/*
	 * If there is an index, read the raw index into a dynamically
	 * allocated buffer and then convert it into a cooked index.
	 */
	if (j->header.index_size != 0) {
		unsigned int i;
		unsigned int rawbytes;
		unsigned char *p;

		rawbytes = ISC_CHECKED_MUL(j->header.index_size,
					   sizeof(journal_rawpos_t));
		j->rawindex = isc_mem_get(mctx, rawbytes);

		CHECK(journal_read(j, j->rawindex, rawbytes));

		j->index = isc_mem_cget(mctx, j->header.index_size,
					sizeof(journal_pos_t));

		p = j->rawindex;
		for (i = 0; i < j->header.index_size; i++) {
			j->index[i].serial = decode_uint32(p);
			p += 4;
			j->index[i].offset = decode_uint32(p);
			p += 4;
		}
		INSIST(p == j->rawindex + rawbytes);
	}
	j->offset = -1; /* Invalid, must seek explicitly. */

	/*
	 * Initialize the iterator.
	 */
	dns_name_init(&j->it.name);
	dns_rdata_init(&j->it.rdata);

	/*
	 * Set up empty initial buffers for unchecked and checked
	 * wire format RR data.  They will be reallocated
	 * later.
	 */
	isc_buffer_init(&j->it.source, NULL, 0);
	isc_buffer_init(&j->it.target, NULL, 0);
	j->it.dctx = DNS_DECOMPRESS_NEVER;

	j->state = writable ? JOURNAL_STATE_WRITE : JOURNAL_STATE_READ;

	*journalp = j;
	return ISC_R_SUCCESS;

failure:
	j->magic = 0;
	if (j->rawindex != NULL) {
		isc_mem_cput(j->mctx, j->rawindex, j->header.index_size,
			     sizeof(journal_rawpos_t));
	}
	if (j->index != NULL) {
		isc_mem_cput(j->mctx, j->index, j->header.index_size,
			     sizeof(journal_pos_t));
	}
	isc_mem_free(j->mctx, j->filename);
	if (j->fp != NULL) {
		(void)isc_stdio_close(j->fp);
	}
	isc_mem_putanddetach(&j->mctx, j, sizeof(*j));
	return result;
}

isc_result_t
dns_journal_open(isc_mem_t *mctx, const char *filename, unsigned int mode,
		 dns_journal_t **journalp) {
	isc_result_t result;
	size_t namelen;
	char backup[1024];
	bool writable, create;

	create = ((mode & DNS_JOURNAL_CREATE) != 0);
	writable = ((mode & (DNS_JOURNAL_WRITE | DNS_JOURNAL_CREATE)) != 0);

	result = journal_open(mctx, filename, writable, create, false,
			      journalp);
	if (result == ISC_R_NOTFOUND) {
		namelen = strlen(filename);
		if (namelen > 4U && strcmp(filename + namelen - 4, ".jnl") == 0)
		{
			namelen -= 4;
		}

		result = snprintf(backup, sizeof(backup), "%.*s.jbk",
				  (int)namelen, filename);
		if (result >= sizeof(backup)) {
			return ISC_R_NOSPACE;
		}
		result = journal_open(mctx, backup, writable, writable, false,
				      journalp);
	}
	return result;
}

/*
 * A comparison function defining the sorting order for
 * entries in the IXFR-style journal file.
 *
 * The IXFR format requires that deletions are sorted before
 * additions, and within either one, SOA records are sorted
 * before others.
 *
 * Also sort the non-SOA records by type as a courtesy to the
 * server receiving the IXFR - it may help reduce the amount of
 * rdataset merging it has to do.
 */
static int
ixfr_order(const void *av, const void *bv) {
	dns_difftuple_t const *const *ap = av;
	dns_difftuple_t const *const *bp = bv;
	dns_difftuple_t const *a = *ap;
	dns_difftuple_t const *b = *bp;
	int r;
	int bop = 0, aop = 0;

	switch (a->op) {
	case DNS_DIFFOP_DEL:
	case DNS_DIFFOP_DELRESIGN:
		aop = 1;
		break;
	case DNS_DIFFOP_ADD:
	case DNS_DIFFOP_ADDRESIGN:
		aop = 0;
		break;
	default:
		UNREACHABLE();
	}

	switch (b->op) {
	case DNS_DIFFOP_DEL:
	case DNS_DIFFOP_DELRESIGN:
		bop = 1;
		break;
	case DNS_DIFFOP_ADD:
	case DNS_DIFFOP_ADDRESIGN:
		bop = 0;
		break;
	default:
		UNREACHABLE();
	}

	r = bop - aop;
	if (r != 0) {
		return r;
	}

	r = (b->rdata.type == dns_rdatatype_soa) -
	    (a->rdata.type == dns_rdatatype_soa);
	if (r != 0) {
		return r;
	}

	r = (a->rdata.type - b->rdata.type);
	return r;
}

static isc_result_t
maybe_fixup_xhdr(dns_journal_t *j, journal_xhdr_t *xhdr, uint32_t serial,
		 off_t offset) {
	isc_result_t result = ISC_R_SUCCESS;

	/*
	 * Handle mixture of version 1 and version 2
	 * transaction headers in a version 1 journal.
	 */
	if (xhdr->serial0 != serial ||
	    isc_serial_le(xhdr->serial1, xhdr->serial0))
	{
		if (j->xhdr_version == XHDR_VERSION1 && xhdr->serial1 == serial)
		{
			isc_log_write(
				DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
				ISC_LOG_DEBUG(3),
				"%s: XHDR_VERSION1 -> XHDR_VERSION2 at %u",
				j->filename, serial);
			j->xhdr_version = XHDR_VERSION2;
			CHECK(journal_seek(j, offset));
			CHECK(journal_read_xhdr(j, xhdr));
			j->recovered = true;
		} else if (j->xhdr_version == XHDR_VERSION2 &&
			   xhdr->count == serial)
		{
			isc_log_write(
				DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
				ISC_LOG_DEBUG(3),
				"%s: XHDR_VERSION2 -> XHDR_VERSION1 at %u",
				j->filename, serial);
			j->xhdr_version = XHDR_VERSION1;
			CHECK(journal_seek(j, offset));
			CHECK(journal_read_xhdr(j, xhdr));
			j->recovered = true;
		}
	}

	/*
	 * Handle <size, serial0, serial1, 0> transaction header.
	 */
	if (j->xhdr_version == XHDR_VERSION1) {
		uint32_t value;

		CHECK(journal_read(j, &value, sizeof(value)));
		if (value != 0L) {
			CHECK(journal_seek(j, offset + 12));
		} else {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_DEBUG(3),
				      "%s: XHDR_VERSION1 count zero at %u",
				      j->filename, serial);
			j->xhdr_version = XHDR_VERSION2;
			j->recovered = true;
		}
	} else if (j->xhdr_version == XHDR_VERSION2 && xhdr->count == serial &&
		   xhdr->serial1 == 0U &&
		   isc_serial_gt(xhdr->serial0, xhdr->count))
	{
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_DEBUG(3),
			      "%s: XHDR_VERSION2 count zero at %u", j->filename,
			      serial);
		xhdr->serial1 = xhdr->serial0;
		xhdr->serial0 = xhdr->count;
		xhdr->count = 0;
		j->recovered = true;
	}

failure:
	return result;
}

/*
 * Advance '*pos' to the next journal transaction.
 *
 * Requires:
 *	*pos refers to a valid journal transaction.
 *
 * Ensures:
 *	When ISC_R_SUCCESS is returned,
 *	*pos refers to the next journal transaction.
 *
 * Returns one of:
 *
 *    ISC_R_SUCCESS
 *    ISC_R_NOMORE 	*pos pointed at the last transaction
 *    Other results due to file errors are possible.
 */
static isc_result_t
journal_next(dns_journal_t *j, journal_pos_t *pos) {
	isc_result_t result;
	journal_xhdr_t xhdr;
	size_t hdrsize;

	REQUIRE(DNS_JOURNAL_VALID(j));

	result = journal_seek(j, pos->offset);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	if (pos->serial == j->header.end.serial) {
		return ISC_R_NOMORE;
	}

	/*
	 * Read the header of the current transaction.
	 * This will return ISC_R_NOMORE if we are at EOF.
	 */
	result = journal_read_xhdr(j, &xhdr);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	if (j->header_ver1) {
		CHECK(maybe_fixup_xhdr(j, &xhdr, pos->serial, pos->offset));
	}

	/*
	 * Check serial number consistency.
	 */
	if (xhdr.serial0 != pos->serial ||
	    isc_serial_le(xhdr.serial1, xhdr.serial0))
	{
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "%s: journal file corrupt: "
			      "expected serial %u, got %u",
			      j->filename, pos->serial, xhdr.serial0);
		return ISC_R_UNEXPECTED;
	}

	/*
	 * Check for offset wraparound.
	 */
	hdrsize = (j->xhdr_version == XHDR_VERSION2)
			  ? sizeof(journal_rawxhdr_t)
			  : sizeof(journal_rawxhdr_ver1_t);

	if ((off_t)(pos->offset + hdrsize + xhdr.size) < pos->offset) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "%s: offset too large",
			      j->filename);
		return ISC_R_UNEXPECTED;
	}

	pos->offset += hdrsize + xhdr.size;
	pos->serial = xhdr.serial1;
	return ISC_R_SUCCESS;

failure:
	return result;
}

/*
 * If the index of the journal 'j' contains an entry "better"
 * than '*best_guess', replace '*best_guess' with it.
 *
 * "Better" means having a serial number closer to 'serial'
 * but not greater than 'serial'.
 */
static void
index_find(dns_journal_t *j, uint32_t serial, journal_pos_t *best_guess) {
	unsigned int i;
	if (j->index == NULL) {
		return;
	}
	for (i = 0; i < j->header.index_size; i++) {
		if (POS_VALID(j->index[i]) &&
		    DNS_SERIAL_GE(serial, j->index[i].serial) &&
		    DNS_SERIAL_GT(j->index[i].serial, best_guess->serial))
		{
			*best_guess = j->index[i];
		}
	}
}

/*
 * Add a new index entry.  If there is no room, make room by removing
 * the odd-numbered entries and compacting the others into the first
 * half of the index.  This decimates old index entries exponentially
 * over time, so that the index always contains a much larger fraction
 * of recent serial numbers than of old ones.  This is deliberate -
 * most index searches are for outgoing IXFR, and IXFR tends to request
 * recent versions more often than old ones.
 */
static void
index_add(dns_journal_t *j, journal_pos_t *pos) {
	unsigned int i;

	if (j->index == NULL) {
		return;
	}

	/*
	 * Search for a vacant position.
	 */
	for (i = 0; i < j->header.index_size; i++) {
		if (!POS_VALID(j->index[i])) {
			break;
		}
	}
	if (i == j->header.index_size) {
		unsigned int k = 0;
		/*
		 * Found no vacant position.  Make some room.
		 */
		for (i = 0; i < j->header.index_size; i += 2) {
			j->index[k++] = j->index[i];
		}
		i = k; /* 'i' identifies the first vacant position. */
		while (k < j->header.index_size) {
			POS_INVALIDATE(j->index[k]);
			k++;
		}
	}
	INSIST(i < j->header.index_size);
	INSIST(!POS_VALID(j->index[i]));

	/*
	 * Store the new index entry.
	 */
	j->index[i] = *pos;
}

/*
 * Invalidate any existing index entries that could become
 * ambiguous when a new transaction with number 'serial' is added.
 */
static void
index_invalidate(dns_journal_t *j, uint32_t serial) {
	unsigned int i;
	if (j->index == NULL) {
		return;
	}
	for (i = 0; i < j->header.index_size; i++) {
		if (!DNS_SERIAL_GT(serial, j->index[i].serial)) {
			POS_INVALIDATE(j->index[i]);
		}
	}
}

/*
 * Try to find a transaction with initial serial number 'serial'
 * in the journal 'j'.
 *
 * If found, store its position at '*pos' and return ISC_R_SUCCESS.
 *
 * If 'serial' is current (= the ending serial number of the
 * last transaction in the journal), set '*pos' to
 * the position immediately following the last transaction and
 * return ISC_R_SUCCESS.
 *
 * If 'serial' is within the range of addressable serial numbers
 * covered by the journal but that particular serial number is missing
 * (from the journal, not just from the index), return ISC_R_NOTFOUND.
 *
 * If 'serial' is outside the range of addressable serial numbers
 * covered by the journal, return ISC_R_RANGE.
 *
 */
static isc_result_t
journal_find(dns_journal_t *j, uint32_t serial, journal_pos_t *pos) {
	isc_result_t result;
	journal_pos_t current_pos;

	REQUIRE(DNS_JOURNAL_VALID(j));

	if (DNS_SERIAL_GT(j->header.begin.serial, serial)) {
		return ISC_R_RANGE;
	}
	if (DNS_SERIAL_GT(serial, j->header.end.serial)) {
		return ISC_R_RANGE;
	}
	if (serial == j->header.end.serial) {
		*pos = j->header.end;
		return ISC_R_SUCCESS;
	}

	current_pos = j->header.begin;
	index_find(j, serial, &current_pos);

	while (current_pos.serial != serial) {
		if (DNS_SERIAL_GT(current_pos.serial, serial)) {
			return ISC_R_NOTFOUND;
		}
		result = journal_next(j, &current_pos);
		if (result != ISC_R_SUCCESS) {
			return result;
		}
	}
	*pos = current_pos;
	return ISC_R_SUCCESS;
}

isc_result_t
dns_journal_begin_transaction(dns_journal_t *j) {
	uint32_t offset;
	isc_result_t result;

	REQUIRE(DNS_JOURNAL_VALID(j));
	REQUIRE(j->state == JOURNAL_STATE_WRITE ||
		j->state == JOURNAL_STATE_INLINE);

	/*
	 * Find the file offset where the new transaction should
	 * be written, and seek there.
	 */
	if (JOURNAL_EMPTY(&j->header)) {
		offset = sizeof(journal_rawheader_t) +
			 ISC_CHECKED_MUL(j->header.index_size,
					 sizeof(journal_rawpos_t));
	} else {
		offset = j->header.end.offset;
	}
	j->x.pos[0].offset = offset;
	j->x.pos[1].offset = offset; /* Initial value, will be incremented. */
	j->x.n_soa = 0;

	CHECK(journal_seek(j, offset));

	/*
	 * Write a dummy transaction header of all zeroes to reserve
	 * space.  It will be filled in when the transaction is
	 * finished.
	 */
	CHECK(journal_write_xhdr(j, 0, 0, 0, 0));
	j->x.pos[1].offset = j->offset;

	j->state = JOURNAL_STATE_TRANSACTION;
	result = ISC_R_SUCCESS;
failure:
	return result;
}

isc_result_t
dns_journal_writediff(dns_journal_t *j, dns_diff_t *diff) {
	isc_buffer_t buffer;
	void *mem = NULL;
	uint64_t size = 0;
	uint32_t rrcount = 0;
	isc_result_t result;
	isc_region_t used;

	REQUIRE(DNS_DIFF_VALID(diff));
	REQUIRE(j->state == JOURNAL_STATE_TRANSACTION);

	isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
		      ISC_LOG_DEBUG(3), "writing to journal");
	(void)dns_diff_print(diff, NULL);

	/*
	 * Pass 1: determine the buffer size needed, and
	 * keep track of SOA serial numbers.
	 */
	ISC_LIST_FOREACH (diff->tuples, t, link) {
		if (t->rdata.type == dns_rdatatype_soa) {
			if (j->x.n_soa < 2) {
				j->x.pos[j->x.n_soa].serial =
					dns_soa_getserial(&t->rdata);
			}
			j->x.n_soa++;
		}
		size += sizeof(journal_rawrrhdr_t);
		size += t->name.length; /* XXX should have access macro? */
		size += 10;
		size += t->rdata.length;
	}

	if (size >= DNS_JOURNAL_SIZE_MAX) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "dns_journal_writediff: %s: journal entry "
			      "too big to be stored: %" PRIu64 " bytes",
			      j->filename, size);
		return ISC_R_NOSPACE;
	}

	mem = isc_mem_get(j->mctx, size);

	isc_buffer_init(&buffer, mem, size);

	/*
	 * Pass 2.  Write RRs to buffer.
	 */
	ISC_LIST_FOREACH (diff->tuples, t, link) {
		/*
		 * Write the RR header.
		 */
		isc_buffer_putuint32(&buffer,
				     t->name.length + 10 + t->rdata.length);
		/*
		 * Write the owner name, RR header, and RR data.
		 */
		isc_buffer_putmem(&buffer, t->name.ndata, t->name.length);
		isc_buffer_putuint16(&buffer, t->rdata.type);
		isc_buffer_putuint16(&buffer, t->rdata.rdclass);
		isc_buffer_putuint32(&buffer, t->ttl);
		isc_buffer_putuint16(&buffer, (uint16_t)t->rdata.length);
		INSIST(isc_buffer_availablelength(&buffer) >= t->rdata.length);
		isc_buffer_putmem(&buffer, t->rdata.data, t->rdata.length);

		rrcount++;
	}

	isc_buffer_usedregion(&buffer, &used);
	INSIST(used.length == size);

	j->x.pos[1].offset += used.length;
	j->x.n_rr = rrcount;

	/*
	 * Write the buffer contents to the journal file.
	 */
	CHECK(journal_write(j, used.base, used.length));

	result = ISC_R_SUCCESS;

failure:
	if (mem != NULL) {
		isc_mem_put(j->mctx, mem, size);
	}
	return result;
}

isc_result_t
dns_journal_commit(dns_journal_t *j) {
	isc_result_t result;
	journal_rawheader_t rawheader;
	uint64_t total;

	REQUIRE(DNS_JOURNAL_VALID(j));
	REQUIRE(j->state == JOURNAL_STATE_TRANSACTION ||
		j->state == JOURNAL_STATE_INLINE);

	/*
	 * Just write out a updated header.
	 */
	if (j->state == JOURNAL_STATE_INLINE) {
		CHECK(journal_fsync(j));
		journal_header_encode(&j->header, &rawheader);
		CHECK(journal_seek(j, 0));
		CHECK(journal_write(j, &rawheader, sizeof(rawheader)));
		CHECK(journal_fsync(j));
		j->state = JOURNAL_STATE_WRITE;
		return ISC_R_SUCCESS;
	}

	/*
	 * Perform some basic consistency checks.
	 */
	if (j->x.n_soa != 2) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "%s: malformed transaction: %d SOAs", j->filename,
			      j->x.n_soa);
		return ISC_R_UNEXPECTED;
	}
	if (!DNS_SERIAL_GT(j->x.pos[1].serial, j->x.pos[0].serial)) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "%s: malformed transaction: serial number "
			      "did not increase",
			      j->filename);
		return ISC_R_UNEXPECTED;
	}
	if (!JOURNAL_EMPTY(&j->header)) {
		if (j->x.pos[0].serial != j->header.end.serial) {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_ERROR,
				      "malformed transaction: "
				      "%s last serial %u != "
				      "transaction first serial %u",
				      j->filename, j->header.end.serial,
				      j->x.pos[0].serial);
			return ISC_R_UNEXPECTED;
		}
	}

	/*
	 * We currently don't support huge journal entries.
	 */
	total = j->x.pos[1].offset - j->x.pos[0].offset;
	if (total >= DNS_JOURNAL_SIZE_MAX) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "transaction too big to be stored in journal: "
			      "%" PRIu64 "b (max is %" PRIu64 "b)",
			      total, (uint64_t)DNS_JOURNAL_SIZE_MAX);
		return ISC_R_UNEXPECTED;
	}

	/*
	 * Some old journal entries may become non-addressable
	 * when we increment the current serial number.  Purge them
	 * by stepping header.begin forward to the first addressable
	 * transaction.  Also purge them from the index.
	 */
	if (!JOURNAL_EMPTY(&j->header)) {
		while (!DNS_SERIAL_GT(j->x.pos[1].serial,
				      j->header.begin.serial))
		{
			CHECK(journal_next(j, &j->header.begin));
		}
		index_invalidate(j, j->x.pos[1].serial);
	}
#ifdef notyet
	if (DNS_SERIAL_GT(last_dumped_serial, j->x.pos[1].serial)) {
		force_dump(...);
	}
#endif /* ifdef notyet */

	/*
	 * Commit the transaction data to stable storage.
	 */
	CHECK(journal_fsync(j));

	if (j->state == JOURNAL_STATE_TRANSACTION) {
		off_t offset;
		offset = (j->x.pos[1].offset - j->x.pos[0].offset) -
			 (j->header_ver1 ? sizeof(journal_rawxhdr_ver1_t)
					 : sizeof(journal_rawxhdr_t));
		/*
		 * Update the transaction header.
		 */
		CHECK(journal_seek(j, j->x.pos[0].offset));
		CHECK(journal_write_xhdr(j, offset, j->x.n_rr,
					 j->x.pos[0].serial,
					 j->x.pos[1].serial));
	}

	/*
	 * Update the journal header.
	 */
	if (JOURNAL_EMPTY(&j->header)) {
		j->header.begin = j->x.pos[0];
	}
	j->header.end = j->x.pos[1];
	journal_header_encode(&j->header, &rawheader);
	CHECK(journal_seek(j, 0));
	CHECK(journal_write(j, &rawheader, sizeof(rawheader)));

	/*
	 * Update the index.
	 */
	index_add(j, &j->x.pos[0]);

	/*
	 * Convert the index into on-disk format and write
	 * it to disk.
	 */
	CHECK(index_to_disk(j));

	/*
	 * Commit the header to stable storage.
	 */
	CHECK(journal_fsync(j));

	/*
	 * We no longer have a transaction open.
	 */
	j->state = JOURNAL_STATE_WRITE;

	result = ISC_R_SUCCESS;

failure:
	return result;
}

isc_result_t
dns_journal_write_transaction(dns_journal_t *j, dns_diff_t *diff) {
	isc_result_t result;

	CHECK(dns_diff_sort(diff, ixfr_order));
	CHECK(dns_journal_begin_transaction(j));
	CHECK(dns_journal_writediff(j, diff));
	CHECK(dns_journal_commit(j));
	result = ISC_R_SUCCESS;
failure:
	return result;
}

void
dns_journal_destroy(dns_journal_t **journalp) {
	dns_journal_t *j = NULL;

	REQUIRE(journalp != NULL);
	REQUIRE(DNS_JOURNAL_VALID(*journalp));

	j = *journalp;
	*journalp = NULL;

	j->it.result = ISC_R_FAILURE;
	dns_name_invalidate(&j->it.name);
	if (j->rawindex != NULL) {
		isc_mem_cput(j->mctx, j->rawindex, j->header.index_size,
			     sizeof(journal_rawpos_t));
	}
	if (j->index != NULL) {
		isc_mem_cput(j->mctx, j->index, j->header.index_size,
			     sizeof(journal_pos_t));
	}
	if (j->it.target.base != NULL) {
		isc_mem_put(j->mctx, j->it.target.base, j->it.target.length);
	}
	if (j->it.source.base != NULL) {
		isc_mem_put(j->mctx, j->it.source.base, j->it.source.length);
	}
	if (j->filename != NULL) {
		isc_mem_free(j->mctx, j->filename);
	}
	if (j->fp != NULL) {
		(void)isc_stdio_close(j->fp);
	}
	j->magic = 0;
	isc_mem_putanddetach(&j->mctx, j, sizeof(*j));
}

/*
 * Roll the open journal 'j' into the database 'db'.
 * A new database version will be created.
 */

/* XXX Share code with incoming IXFR? */

isc_result_t
dns_journal_rollforward(dns_journal_t *j, dns_db_t *db, unsigned int options) {
	isc_buffer_t source; /* Transaction data from disk */
	isc_buffer_t target; /* Ditto after _fromwire check */
	uint32_t db_serial;  /* Database SOA serial */
	uint32_t end_serial; /* Last journal SOA serial */
	isc_result_t result;
	dns_dbversion_t *ver = NULL;
	journal_pos_t pos;
	dns_diff_t diff;
	unsigned int n_soa = 0;
	unsigned int n_put = 0;
	dns_diffop_t op;

	REQUIRE(DNS_JOURNAL_VALID(j));
	REQUIRE(DNS_DB_VALID(db));

	dns_diff_init(j->mctx, &diff);

	/*
	 * Set up empty initial buffers for unchecked and checked
	 * wire format transaction data.  They will be reallocated
	 * later.
	 */
	isc_buffer_init(&source, NULL, 0);
	isc_buffer_init(&target, NULL, 0);

	/*
	 * Create the new database version.
	 */
	CHECK(dns_db_newversion(db, &ver));

	/*
	 * Get the current database SOA serial number.
	 */
	CHECK(dns_db_getsoaserial(db, ver, &db_serial));

	/*
	 * Locate a journal entry for the current database serial.
	 */
	CHECK(journal_find(j, db_serial, &pos));

	end_serial = dns_journal_last_serial(j);

	/*
	 * If we're reading a version 1 file, scan all the transactions
	 * to see if the journal needs rewriting: if any outdated
	 * transaction headers are found, j->recovered will be set.
	 */
	if (j->header_ver1) {
		uint32_t start_serial = dns_journal_first_serial(j);

		CHECK(dns_journal_iter_init(j, start_serial, db_serial, NULL));
		for (result = dns_journal_first_rr(j); result == ISC_R_SUCCESS;
		     result = dns_journal_next_rr(j))
		{
			continue;
		}
	}

	if (db_serial == end_serial) {
		CHECK(DNS_R_UPTODATE);
	}

	CHECK(dns_journal_iter_init(j, db_serial, end_serial, NULL));
	for (result = dns_journal_first_rr(j); result == ISC_R_SUCCESS;
	     result = dns_journal_next_rr(j))
	{
		dns_name_t *name = NULL;
		dns_rdata_t *rdata = NULL;
		dns_difftuple_t *tuple = NULL;
		uint32_t ttl;

		dns_journal_current_rr(j, &name, &ttl, &rdata);

		if (rdata->type == dns_rdatatype_soa) {
			n_soa++;
			if (n_soa == 2) {
				db_serial = j->it.current_serial;
			}
		}

		if (n_soa == 3) {
			n_soa = 1;
		}
		if (n_soa == 0) {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_ERROR,
				      "%s: journal file corrupt: missing "
				      "initial SOA",
				      j->filename);
			FAIL(ISC_R_UNEXPECTED);
		}
		if ((options & DNS_JOURNALOPT_RESIGN) != 0) {
			op = (n_soa == 1) ? DNS_DIFFOP_DELRESIGN
					  : DNS_DIFFOP_ADDRESIGN;
		} else {
			op = (n_soa == 1) ? DNS_DIFFOP_DEL : DNS_DIFFOP_ADD;
		}

		dns_difftuple_create(diff.mctx, op, name, ttl, rdata, &tuple);
		dns_diff_append(&diff, &tuple);

		if (++n_put > 100) {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_DEBUG(3),
				      "%s: applying diff to database (%u)",
				      j->filename, db_serial);
			(void)dns_diff_print(&diff, NULL);
			CHECK(dns_diff_apply(&diff, db, ver));
			dns_diff_clear(&diff);
			n_put = 0;
		}
	}
	if (result == ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}
	CHECK(result);

	if (n_put != 0) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_DEBUG(3),
			      "%s: applying final diff to database (%u)",
			      j->filename, db_serial);
		(void)dns_diff_print(&diff, NULL);
		CHECK(dns_diff_apply(&diff, db, ver));
		dns_diff_clear(&diff);
	}

failure:
	if (ver != NULL) {
		dns_db_closeversion(db, &ver,
				    result == ISC_R_SUCCESS ? true : false);
	}

	if (source.base != NULL) {
		isc_mem_put(j->mctx, source.base, source.length);
	}
	if (target.base != NULL) {
		isc_mem_put(j->mctx, target.base, target.length);
	}

	dns_diff_clear(&diff);

	INSIST(ver == NULL);

	return result;
}

isc_result_t
dns_journal_print(isc_mem_t *mctx, uint32_t flags, const char *filename,
		  FILE *file) {
	dns_journal_t *j = NULL;
	isc_buffer_t source;   /* Transaction data from disk */
	isc_buffer_t target;   /* Ditto after _fromwire check */
	uint32_t start_serial; /* Database SOA serial */
	uint32_t end_serial;   /* Last journal SOA serial */
	isc_result_t result;
	dns_diff_t diff;
	unsigned int n_soa = 0;
	unsigned int n_put = 0;
	bool printxhdr = ((flags & DNS_JOURNAL_PRINTXHDR) != 0);

	REQUIRE(filename != NULL);

	result = dns_journal_open(mctx, filename, DNS_JOURNAL_READ, &j);
	if (result == ISC_R_NOTFOUND) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_DEBUG(3), "no journal file");
		return DNS_R_NOJOURNAL;
	} else if (result != ISC_R_SUCCESS) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR, "journal open failure: %s: %s",
			      isc_result_totext(result), filename);
		return result;
	}

	if (printxhdr) {
		fprintf(file, "Journal format = %sHeader version = %d\n",
			j->header.format + 1, j->header_ver1 ? 1 : 2);
		fprintf(file, "Start serial = %u\n", j->header.begin.serial);
		fprintf(file, "End serial = %u\n", j->header.end.serial);
		fprintf(file, "Index (size = %u):\n", j->header.index_size);
		for (uint32_t i = 0; i < j->header.index_size; i++) {
			if (j->index[i].offset == 0) {
				fputc('\n', file);
				break;
			}
			fprintf(file, "%lld", (long long)j->index[i].offset);
			fputc((i + 1) % 8 == 0 ? '\n' : ' ', file);
		}
	}
	if (j->header.serialset) {
		fprintf(file, "Source serial = %u\n", j->header.sourceserial);
	}
	dns_diff_init(j->mctx, &diff);

	/*
	 * Set up empty initial buffers for unchecked and checked
	 * wire format transaction data.  They will be reallocated
	 * later.
	 */
	isc_buffer_init(&source, NULL, 0);
	isc_buffer_init(&target, NULL, 0);

	start_serial = dns_journal_first_serial(j);
	end_serial = dns_journal_last_serial(j);

	CHECK(dns_journal_iter_init(j, start_serial, end_serial, NULL));

	for (result = dns_journal_first_rr(j); result == ISC_R_SUCCESS;
	     result = dns_journal_next_rr(j))
	{
		dns_name_t *name = NULL;
		dns_rdata_t *rdata = NULL;
		dns_difftuple_t *tuple = NULL;
		static uint32_t i = 0;
		bool print = false;
		uint32_t ttl;

		dns_journal_current_rr(j, &name, &ttl, &rdata);

		if (rdata->type == dns_rdatatype_soa) {
			n_soa++;
			if (n_soa == 3) {
				n_soa = 1;
			}
			if (n_soa == 1) {
				print = printxhdr;
			}
		}
		if (n_soa == 0) {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_ERROR,
				      "%s: journal file corrupt: missing "
				      "initial SOA",
				      j->filename);
			FAIL(ISC_R_UNEXPECTED);
		}

		if (print) {
			fprintf(file,
				"Transaction: version %d offset %lld size %u "
				"rrcount %u start %u end %u\n",
				j->xhdr_version, (long long)j->it.cpos.offset,
				j->curxhdr.size, j->curxhdr.count,
				j->curxhdr.serial0, j->curxhdr.serial1);
			if (j->it.cpos.offset > j->index[i].offset) {
				fprintf(file,
					"ERROR: Offset mismatch, "
					"expected %lld\n",
					(long long)j->index[i].offset);
			} else if (j->it.cpos.offset == j->index[i].offset) {
				i++;
			}
		}
		dns_difftuple_create(
			diff.mctx, n_soa == 1 ? DNS_DIFFOP_DEL : DNS_DIFFOP_ADD,
			name, ttl, rdata, &tuple);
		dns_diff_append(&diff, &tuple);

		if (++n_put > 100 || printxhdr) {
			result = dns_diff_print(&diff, file);
			dns_diff_clear(&diff);
			n_put = 0;
			if (result != ISC_R_SUCCESS) {
				break;
			}
		}
	}
	if (result == ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}
	CHECK(result);

	if (n_put != 0) {
		result = dns_diff_print(&diff, file);
		dns_diff_clear(&diff);
	}
	goto cleanup;

failure:
	isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
		      ISC_LOG_ERROR, "%s: cannot print: journal file corrupt",
		      j->filename);

cleanup:
	if (source.base != NULL) {
		isc_mem_put(j->mctx, source.base, source.length);
	}
	if (target.base != NULL) {
		isc_mem_put(j->mctx, target.base, target.length);
	}

	dns_diff_clear(&diff);
	dns_journal_destroy(&j);

	return result;
}

/**************************************************************************/
/*
 * Miscellaneous accessors.
 */
bool
dns_journal_empty(dns_journal_t *j) {
	return JOURNAL_EMPTY(&j->header);
}

bool
dns_journal_recovered(dns_journal_t *j) {
	return j->recovered;
}

uint32_t
dns_journal_first_serial(dns_journal_t *j) {
	return j->header.begin.serial;
}

uint32_t
dns_journal_last_serial(dns_journal_t *j) {
	return j->header.end.serial;
}

void
dns_journal_set_sourceserial(dns_journal_t *j, uint32_t sourceserial) {
	REQUIRE(j->state == JOURNAL_STATE_WRITE ||
		j->state == JOURNAL_STATE_INLINE ||
		j->state == JOURNAL_STATE_TRANSACTION);

	j->header.sourceserial = sourceserial;
	j->header.serialset = true;
	if (j->state == JOURNAL_STATE_WRITE) {
		j->state = JOURNAL_STATE_INLINE;
	}
}

bool
dns_journal_get_sourceserial(dns_journal_t *j, uint32_t *sourceserial) {
	REQUIRE(sourceserial != NULL);

	if (!j->header.serialset) {
		return false;
	}
	*sourceserial = j->header.sourceserial;
	return true;
}

/**************************************************************************/
/*
 * Iteration support.
 *
 * When serving an outgoing IXFR, we transmit a part the journal starting
 * at the serial number in the IXFR request and ending at the serial
 * number that is current when the IXFR request arrives.  The ending
 * serial number is not necessarily at the end of the journal:
 * the journal may grow while the IXFR is in progress, but we stop
 * when we reach the serial number that was current when the IXFR started.
 */

static isc_result_t
read_one_rr(dns_journal_t *j);

/*
 * Make sure the buffer 'b' is has at least 'size' bytes
 * allocated, and clear it.
 *
 * Requires:
 *	Either b->base is NULL, or it points to b->length bytes of memory
 *	previously allocated by isc_mem_get().
 */

static void
size_buffer(isc_mem_t *mctx, isc_buffer_t *b, unsigned int size) {
	if (b->length < size) {
		void *mem = isc_mem_get(mctx, size);
		if (b->base != NULL) {
			isc_mem_put(mctx, b->base, b->length);
		}
		b->base = mem;
		b->length = size;
	}
	isc_buffer_clear(b);
}

isc_result_t
dns_journal_iter_init(dns_journal_t *j, uint32_t begin_serial,
		      uint32_t end_serial, size_t *xfrsizep) {
	isc_result_t result;

	CHECK(journal_find(j, begin_serial, &j->it.bpos));
	INSIST(j->it.bpos.serial == begin_serial);

	CHECK(journal_find(j, end_serial, &j->it.epos));
	INSIST(j->it.epos.serial == end_serial);

	if (xfrsizep != NULL) {
		journal_pos_t pos = j->it.bpos;
		journal_xhdr_t xhdr;
		uint64_t size = 0;
		uint32_t count = 0;

		/*
		 * We already know the beginning and ending serial
		 * numbers are in the journal. Scan through them,
		 * adding up sizes and RR counts so we can calculate
		 * the IXFR size.
		 */
		do {
			CHECK(journal_seek(j, pos.offset));
			CHECK(journal_read_xhdr(j, &xhdr));

			if (j->header_ver1) {
				CHECK(maybe_fixup_xhdr(j, &xhdr, pos.serial,
						       pos.offset));
			}

			/*
			 * Check that xhdr is consistent.
			 */
			if (xhdr.serial0 != pos.serial ||
			    isc_serial_le(xhdr.serial1, xhdr.serial0))
			{
				CHECK(ISC_R_UNEXPECTED);
			}

			size += xhdr.size;
			count += xhdr.count;

			result = journal_next(j, &pos);
			if (result == ISC_R_NOMORE) {
				result = ISC_R_SUCCESS;
			}
			CHECK(result);
		} while (pos.serial != end_serial);

		/*
		 * For each RR, subtract the length of the RR header,
		 * as this would not be present in IXFR messages.
		 * (We don't need to worry about the transaction header
		 * because that was already excluded from xdr.size.)
		 */
		*xfrsizep = size - (ISC_CHECKED_MUL(
					   count, sizeof(journal_rawrrhdr_t)));
	}

	result = ISC_R_SUCCESS;
failure:
	j->it.result = result;
	return j->it.result;
}

isc_result_t
dns_journal_first_rr(dns_journal_t *j) {
	isc_result_t result;

	/*
	 * Seek to the beginning of the first transaction we are
	 * interested in.
	 */
	CHECK(journal_seek(j, j->it.bpos.offset));
	j->it.current_serial = j->it.bpos.serial;

	j->it.xsize = 0; /* We have no transaction data yet... */
	j->it.xpos = 0;	 /* ...and haven't used any of it. */

	return read_one_rr(j);

failure:
	return result;
}

static isc_result_t
read_one_rr(dns_journal_t *j) {
	isc_result_t result;
	dns_rdatatype_t rdtype;
	dns_rdataclass_t rdclass;
	unsigned int rdlen;
	uint32_t ttl;
	journal_xhdr_t xhdr;
	journal_rrhdr_t rrhdr;
	dns_journal_t save = *j;

	if (j->offset > j->it.epos.offset) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "%s: journal corrupt: possible integer overflow",
			      j->filename);
		return ISC_R_UNEXPECTED;
	}
	if (j->offset == j->it.epos.offset) {
		return ISC_R_NOMORE;
	}
	if (j->it.xpos == j->it.xsize) {
		/*
		 * We are at a transaction boundary.
		 * Read another transaction header.
		 */
		CHECK(journal_read_xhdr(j, &xhdr));
		if (xhdr.size == 0) {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_ERROR,
				      "%s: journal corrupt: empty transaction",
				      j->filename);
			FAIL(ISC_R_UNEXPECTED);
		}

		if (j->header_ver1) {
			CHECK(maybe_fixup_xhdr(j, &xhdr, j->it.current_serial,
					       save.offset));
		}

		if (xhdr.serial0 != j->it.current_serial ||
		    isc_serial_le(xhdr.serial1, xhdr.serial0))
		{
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_ERROR,
				      "%s: journal file corrupt: "
				      "expected serial %u, got %u",
				      j->filename, j->it.current_serial,
				      xhdr.serial0);
			FAIL(ISC_R_UNEXPECTED);
		}

		j->it.xsize = xhdr.size;
		j->it.xpos = 0;
	}
	/*
	 * Read an RR.
	 */
	CHECK(journal_read_rrhdr(j, &rrhdr));
	/*
	 * Perform a sanity check on the journal RR size.
	 * The smallest possible RR has a 1-byte owner name
	 * and a 10-byte header.  The largest possible
	 * RR has 65535 bytes of data, a header, and a maximum-
	 * size owner name, well below 70 k total.
	 */
	if (rrhdr.size < 1 + 10 || rrhdr.size > 70000) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "%s: journal corrupt: impossible RR size "
			      "(%d bytes)",
			      j->filename, rrhdr.size);
		FAIL(ISC_R_UNEXPECTED);
	}

	size_buffer(j->mctx, &j->it.source, rrhdr.size);
	CHECK(journal_read(j, j->it.source.base, rrhdr.size));
	isc_buffer_add(&j->it.source, rrhdr.size);

	/*
	 * The target buffer is made the same size
	 * as the source buffer, with the assumption that when
	 * no compression in present, the output of dns_*_fromwire()
	 * is no larger than the input.
	 */
	size_buffer(j->mctx, &j->it.target, rrhdr.size);

	/*
	 * Parse the owner name.  We don't know where it
	 * ends yet, so we make the entire "remaining"
	 * part of the buffer "active".
	 */
	isc_buffer_setactive(&j->it.source,
			     j->it.source.used - j->it.source.current);
	CHECK(dns_name_fromwire(&j->it.name, &j->it.source, j->it.dctx,
				&j->it.target));

	/*
	 * Check that the RR header is there, and parse it.
	 */
	if (isc_buffer_remaininglength(&j->it.source) < 10) {
		FAIL(DNS_R_FORMERR);
	}

	rdtype = isc_buffer_getuint16(&j->it.source);
	rdclass = isc_buffer_getuint16(&j->it.source);
	ttl = isc_buffer_getuint32(&j->it.source);
	rdlen = isc_buffer_getuint16(&j->it.source);

	if (rdlen > DNS_RDATA_MAXLENGTH) {
		isc_log_write(DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_JOURNAL,
			      ISC_LOG_ERROR,
			      "%s: journal corrupt: impossible rdlen "
			      "(%u bytes)",
			      j->filename, rdlen);
		FAIL(ISC_R_FAILURE);
	}

	/*
	 * Parse the rdata.
	 */
	if (isc_buffer_remaininglength(&j->it.source) != rdlen) {
		FAIL(DNS_R_FORMERR);
	}
	isc_buffer_setactive(&j->it.source, rdlen);
	dns_rdata_reset(&j->it.rdata);
	CHECK(dns_rdata_fromwire(&j->it.rdata, rdclass, rdtype, &j->it.source,
				 j->it.dctx, &j->it.target));
	j->it.ttl = ttl;

	j->it.xpos += sizeof(journal_rawrrhdr_t) + rrhdr.size;
	if (rdtype == dns_rdatatype_soa) {
		/* XXX could do additional consistency checks here */
		j->it.current_serial = dns_soa_getserial(&j->it.rdata);
	}

	result = ISC_R_SUCCESS;

failure:
	j->it.result = result;
	return result;
}

isc_result_t
dns_journal_next_rr(dns_journal_t *j) {
	j->it.result = read_one_rr(j);
	return j->it.result;
}

void
dns_journal_current_rr(dns_journal_t *j, dns_name_t **name, uint32_t *ttl,
		       dns_rdata_t **rdata) {
	REQUIRE(j->it.result == ISC_R_SUCCESS);
	*name = &j->it.name;
	*ttl = j->it.ttl;
	*rdata = &j->it.rdata;
}

/**************************************************************************/
/*
 * Generating diffs from databases
 */

/*
 * Construct a diff containing all the RRs at the current name of the
 * database iterator 'dbit' in database 'db', version 'ver'.
 * Set '*name' to the current name, and append the diff to 'diff'.
 * All new tuples will have the operation 'op'.
 *
 * Requires: 'name' must have buffer large enough to hold the name.
 * Typically, a dns_fixedname_t would be used.
 */
static isc_result_t
get_name_diff(dns_db_t *db, dns_dbversion_t *ver, isc_stdtime_t now,
	      dns_dbiterator_t *dbit, dns_name_t *name, dns_diffop_t op,
	      dns_diff_t *diff) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	dns_rdatasetiter_t *rdsiter = NULL;
	dns_difftuple_t *tuple = NULL;

	result = dns_dbiterator_current(dbit, &node, name);
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	result = dns_db_allrdatasets(db, node, ver, 0, now, &rdsiter);
	if (result != ISC_R_SUCCESS) {
		goto cleanup_node;
	}

	DNS_RDATASETITER_FOREACH (rdsiter) {
		dns_rdataset_t rdataset = DNS_RDATASET_INIT;
		dns_rdatasetiter_current(rdsiter, &rdataset);

		DNS_RDATASET_FOREACH (&rdataset) {
			dns_rdata_t rdata = DNS_RDATA_INIT;
			dns_rdataset_current(&rdataset, &rdata);
			dns_difftuple_create(diff->mctx, op, name, rdataset.ttl,
					     &rdata, &tuple);
			dns_diff_append(diff, &tuple);
		}
		dns_rdataset_disassociate(&rdataset);
	}
	dns_rdatasetiter_destroy(&rdsiter);

	result = ISC_R_SUCCESS;

cleanup_node:
	dns_db_detachnode(db, &node);

	return result;
}

/*
 * Comparison function for use by dns_diff_subtract when sorting
 * the diffs to be subtracted.  The sort keys are the rdata type
 * and the rdata itself.  The owner name is ignored, because
 * it is known to be the same for all tuples.
 */
static int
rdata_order(const void *av, const void *bv) {
	dns_difftuple_t const *const *ap = av;
	dns_difftuple_t const *const *bp = bv;
	dns_difftuple_t const *a = *ap;
	dns_difftuple_t const *b = *bp;
	int r;
	r = (b->rdata.type - a->rdata.type);
	if (r != 0) {
		return r;
	}
	r = dns_rdata_compare(&a->rdata, &b->rdata);
	return r;
}

static isc_result_t
dns_diff_subtract(dns_diff_t diff[2], dns_diff_t *r) {
	isc_result_t result;
	dns_difftuple_t *p[2];
	int i, t;
	bool append;
	dns_difftuplelist_t add, del;

	CHECK(dns_diff_sort(&diff[0], rdata_order));
	CHECK(dns_diff_sort(&diff[1], rdata_order));
	ISC_LIST_INIT(add);
	ISC_LIST_INIT(del);

	for (;;) {
		p[0] = ISC_LIST_HEAD(diff[0].tuples);
		p[1] = ISC_LIST_HEAD(diff[1].tuples);
		if (p[0] == NULL && p[1] == NULL) {
			break;
		}

		for (i = 0; i < 2; i++) {
			if (p[!i] == NULL) {
				dns_difftuplelist_t *l = (i == 0) ? &add : &del;
				ISC_LIST_UNLINK(diff[i].tuples, p[i], link);
				ISC_LIST_APPEND(*l, p[i], link);
				goto next;
			}
		}
		t = rdata_order(&p[0], &p[1]);
		if (t < 0) {
			ISC_LIST_UNLINK(diff[0].tuples, p[0], link);
			ISC_LIST_APPEND(add, p[0], link);
			goto next;
		}
		if (t > 0) {
			ISC_LIST_UNLINK(diff[1].tuples, p[1], link);
			ISC_LIST_APPEND(del, p[1], link);
			goto next;
		}
		INSIST(t == 0);
		/*
		 * Identical RRs in both databases; skip them both
		 * if the ttl differs.
		 */
		append = (p[0]->ttl != p[1]->ttl);
		for (i = 0; i < 2; i++) {
			ISC_LIST_UNLINK(diff[i].tuples, p[i], link);
			if (append) {
				dns_difftuplelist_t *l = (i == 0) ? &add : &del;
				ISC_LIST_APPEND(*l, p[i], link);
			} else {
				dns_difftuple_free(&p[i]);
			}
		}
	next:;
	}
	ISC_LIST_APPENDLIST(r->tuples, del, link);
	ISC_LIST_APPENDLIST(r->tuples, add, link);
	result = ISC_R_SUCCESS;
failure:
	return result;
}

static isc_result_t
diff_namespace(dns_db_t *dba, dns_dbversion_t *dbvera, dns_db_t *dbb,
	       dns_dbversion_t *dbverb, unsigned int options,
	       dns_diff_t *resultdiff) {
	dns_db_t *db[2];
	dns_dbversion_t *ver[2];
	dns_dbiterator_t *dbit[2] = { NULL, NULL };
	bool have[2] = { false, false };
	dns_fixedname_t fixname[2];
	isc_result_t result, itresult[2];
	dns_diff_t diff[2];
	int i, t;

	db[0] = dba, db[1] = dbb;
	ver[0] = dbvera, ver[1] = dbverb;

	dns_diff_init(resultdiff->mctx, &diff[0]);
	dns_diff_init(resultdiff->mctx, &diff[1]);

	dns_fixedname_init(&fixname[0]);
	dns_fixedname_init(&fixname[1]);

	result = dns_db_createiterator(db[0], options, &dbit[0]);
	if (result != ISC_R_SUCCESS) {
		return result;
	}
	result = dns_db_createiterator(db[1], options, &dbit[1]);
	if (result != ISC_R_SUCCESS) {
		goto cleanup_iterator;
	}

	itresult[0] = dns_dbiterator_first(dbit[0]);
	itresult[1] = dns_dbiterator_first(dbit[1]);

	for (;;) {
		for (i = 0; i < 2; i++) {
			if (!have[i] && itresult[i] == ISC_R_SUCCESS) {
				CHECK(get_name_diff(
					db[i], ver[i], 0, dbit[i],
					dns_fixedname_name(&fixname[i]),
					i == 0 ? DNS_DIFFOP_ADD
					       : DNS_DIFFOP_DEL,
					&diff[i]));
				itresult[i] = dns_dbiterator_next(dbit[i]);
				have[i] = true;
			}
		}

		if (!have[0] && !have[1]) {
			INSIST(ISC_LIST_EMPTY(diff[0].tuples));
			INSIST(ISC_LIST_EMPTY(diff[1].tuples));
			break;
		}

		for (i = 0; i < 2; i++) {
			if (!have[!i]) {
				ISC_LIST_APPENDLIST(resultdiff->tuples,
						    diff[i].tuples, link);
				INSIST(ISC_LIST_EMPTY(diff[i].tuples));
				have[i] = false;
				goto next;
			}
		}

		t = dns_name_compare(dns_fixedname_name(&fixname[0]),
				     dns_fixedname_name(&fixname[1]));
		if (t < 0) {
			ISC_LIST_APPENDLIST(resultdiff->tuples, diff[0].tuples,
					    link);
			INSIST(ISC_LIST_EMPTY(diff[0].tuples));
			have[0] = false;
			continue;
		}
		if (t > 0) {
			ISC_LIST_APPENDLIST(resultdiff->tuples, diff[1].tuples,
					    link);
			INSIST(ISC_LIST_EMPTY(diff[1].tuples));
			have[1] = false;
			continue;
		}
		INSIST(t == 0);
		CHECK(dns_diff_subtract(diff, resultdiff));
		INSIST(ISC_LIST_EMPTY(diff[0].tuples));
		INSIST(ISC_LIST_EMPTY(diff[1].tuples));
		have[0] = have[1] = false;
	next:;
	}
	if (itresult[0] != ISC_R_NOMORE) {
		FAIL(itresult[0]);
	}
	if (itresult[1] != ISC_R_NOMORE) {
		FAIL(itresult[1]);
	}

	INSIST(ISC_LIST_EMPTY(diff[0].tuples));
	INSIST(ISC_LIST_EMPTY(diff[1].tuples));

failure:
	dns_dbiterator_destroy(&dbit[1]);

cleanup_iterator:
	dns_dbiterator_destroy(&dbit[0]);
	dns_diff_clear(&diff[0]);
	dns_diff_clear(&diff[1]);
	return result;
}

/*
 * Compare the databases 'dba' and 'dbb' and generate a journal
 * entry containing the changes to make 'dba' from 'dbb' (note
 * the order).  This journal entry will consist of a single,
 * possibly very large transaction.
 */
isc_result_t
dns_db_diff(isc_mem_t *mctx, dns_db_t *dba, dns_dbversion_t *dbvera,
	    dns_db_t *dbb, dns_dbversion_t *dbverb, const char *filename) {
	isc_result_t result;
	dns_diff_t diff;

	dns_diff_init(mctx, &diff);

	result = dns_db_diffx(&diff, dba, dbvera, dbb, dbverb, filename);

	dns_diff_clear(&diff);

	return result;
}

isc_result_t
dns_db_diffx(dns_diff_t *diff, dns_db_t *dba, dns_dbversion_t *dbvera,
	     dns_db_t *dbb, dns_dbversion_t *dbverb, const char *filename) {
	isc_result_t result;
	dns_journal_t *journal = NULL;

	if (filename != NULL) {
		result = dns_journal_open(diff->mctx, filename,
					  DNS_JOURNAL_CREATE, &journal);
		if (result != ISC_R_SUCCESS) {
			return result;
		}
	}

	CHECK(diff_namespace(dba, dbvera, dbb, dbverb, DNS_DB_NONSEC3, diff));
	CHECK(diff_namespace(dba, dbvera, dbb, dbverb, DNS_DB_NSEC3ONLY, diff));

	if (journal != NULL) {
		if (ISC_LIST_EMPTY(diff->tuples)) {
			isc_log_write(DNS_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_JOURNAL, ISC_LOG_DEBUG(3),
				      "no changes");
		} else {
			CHECK(dns_journal_write_transaction(journal, diff));
		}
	}

failure:
	if (journal != NULL) {
		dns_journal_destroy(&journal);
	}
	return result;
}

static uint32_t
rrcount(unsigned char *buf, unsigned int size) {
	isc_buffer_t b;
	uint32_t rrsize, count = 0;

	isc_buffer_init(&b, buf, size);
	isc_buffer_add(&b, size);
	while (isc_buffer_remaininglength(&b) > 0) {
		rrsize = isc_buffer_getuint32(&b);
		INSIST(isc_buffer_remaininglength(&b) >= rrsize);
		isc_buffer_forward(&b, rrsize);
		count++;
	}

	return count;
}

static bool
check_delta(unsigned char *buf, size_t size) {
	isc_buffer_t b;
	uint32_t rrsize;

	isc_buffer_init(&b, buf, size);
	isc_buffer_add(&b, size);
	while (isc_buffer_remaininglength(&b) > 0) {
		if (isc_buffer_remaininglength(&b) < 4) {
			return false;
		}
		rrsize = isc_buffer_getuint32(&b);
		/* "." + type + class + ttl + rdlen => 11U */
		if (rrsize < 11U || isc_buffer_remaininglength(&b) < rrsize) {
			return false;
		}
		isc_buffer_forward(&b, rrsize);
	}

	return true;
}

isc_result_t
dns_journal_compact(isc_mem_t *mctx, char *filename, uint32_t serial,
		    uint32_t flags, uint32_t target_size) {
	unsigned int i;
	journal_pos_t best_guess;
	journal_pos_t current_pos;
	dns_journal_t *j1 = NULL;
	dns_journal_t *j2 = NULL;
	journal_rawheader_t rawheader;
	unsigned int len;
	size_t namelen;
	unsigned char *buf = NULL;
	unsigned int size = 0;
	isc_result_t result;
	unsigned int indexend;
	char newname[PATH_MAX];
	char backup[PATH_MAX];
	bool is_backup = false;
	bool rewrite = false;
	bool downgrade = false;

	REQUIRE(filename != NULL);

	namelen = strlen(filename);
	if (namelen > 4U && strcmp(filename + namelen - 4, ".jnl") == 0) {
		namelen -= 4;
	}

	result = snprintf(newname, sizeof(newname), "%.*s.jnw", (int)namelen,
			  filename);
	RUNTIME_CHECK(result < sizeof(newname));

	result = snprintf(backup, sizeof(backup), "%.*s.jbk", (int)namelen,
			  filename);
	RUNTIME_CHECK(result < sizeof(backup));

	result = journal_open(mctx, filename, false, false, false, &j1);
	if (result == ISC_R_NOTFOUND) {
		is_backup = true;
		result = journal_open(mctx, backup, false, false, false, &j1);
	}
	if (result != ISC_R_SUCCESS) {
		return result;
	}

	/*
	 * Always perform a re-write when processing a version 1 journal.
	 */
	rewrite = j1->header_ver1;

	/*
	 * Check whether we need to rewrite the whole journal
	 * file (for example, to upversion it).
	 */
	if ((flags & DNS_JOURNAL_COMPACTALL) != 0) {
		if ((flags & DNS_JOURNAL_VERSION1) != 0) {
			downgrade = true;
		}
		rewrite = true;
		serial = dns_journal_first_serial(j1);
	} else if (JOURNAL_EMPTY(&j1->header)) {
		dns_journal_destroy(&j1);
		return ISC_R_SUCCESS;
	}

	if (DNS_SERIAL_GT(j1->header.begin.serial, serial) ||
	    DNS_SERIAL_GT(serial, j1->header.end.serial))
	{
		dns_journal_destroy(&j1);
		return ISC_R_RANGE;
	}

	/*
	 * Cope with very small target sizes.
	 */
	indexend = sizeof(journal_rawheader_t) +
		   ISC_CHECKED_MUL(j1->header.index_size,
				   sizeof(journal_rawpos_t));
	if (target_size < DNS_JOURNAL_SIZE_MIN) {
		target_size = DNS_JOURNAL_SIZE_MIN;
	}
	if (target_size < indexend * 2) {
		target_size = target_size / 2 + indexend;
	}

	/*
	 * See if there is any work to do.
	 */
	if (!rewrite && (uint32_t)j1->header.end.offset < target_size) {
		dns_journal_destroy(&j1);
		return ISC_R_SUCCESS;
	}

	CHECK(journal_open(mctx, newname, true, true, downgrade, &j2));
	CHECK(journal_seek(j2, indexend));

	/*
	 * Remove overhead so space test below can succeed.
	 */
	if (target_size >= indexend) {
		target_size -= indexend;
	}

	/*
	 * Find if we can create enough free space.
	 */
	best_guess = j1->header.begin;
	for (i = 0; i < j1->header.index_size; i++) {
		if (POS_VALID(j1->index[i]) &&
		    DNS_SERIAL_GE(serial, j1->index[i].serial) &&
		    ((uint32_t)(j1->header.end.offset - j1->index[i].offset) >=
		     target_size / 2) &&
		    j1->index[i].offset > best_guess.offset)
		{
			best_guess = j1->index[i];
		}
	}

	current_pos = best_guess;
	while (current_pos.serial != serial) {
		CHECK(journal_next(j1, &current_pos));
		if (current_pos.serial == j1->header.end.serial) {
			break;
		}

		if (DNS_SERIAL_GE(serial, current_pos.serial) &&
		    ((uint32_t)(j1->header.end.offset - current_pos.offset) >=
		     (target_size / 2)) &&
		    current_pos.offset > best_guess.offset)
		{
			best_guess = current_pos;
		} else {
			break;
		}
	}

	INSIST(best_guess.serial != j1->header.end.serial);
	if (best_guess.serial != serial) {
		CHECK(journal_next(j1, &best_guess));
		serial = best_guess.serial;
	}

	/*
	 * We should now be roughly half target_size provided
	 * we did not reach 'serial'.  If not we will just copy
	 * all uncommitted deltas regardless of the size.
	 */
	len = j1->header.end.offset - best_guess.offset;
	if (len != 0) {
		CHECK(journal_seek(j1, best_guess.offset));

		/* Prepare new header */
		j2->header.begin.serial = best_guess.serial;
		j2->header.begin.offset = indexend;
		j2->header.sourceserial = j1->header.sourceserial;
		j2->header.serialset = j1->header.serialset;
		j2->header.end.serial = j1->header.end.serial;

		/*
		 * Only use this method if we're rewriting the
		 * journal to fix outdated transaction headers;
		 * otherwise we'll copy the whole journal without
		 * parsing individual deltas below.
		 */
		while (rewrite && len > 0) {
			journal_xhdr_t xhdr;
			off_t offset = j1->offset;
			uint32_t count;

			result = journal_read_xhdr(j1, &xhdr);
			if (rewrite && result == ISC_R_NOMORE) {
				break;
			}
			CHECK(result);

			size = xhdr.size;
			if (size > len) {
				isc_log_write(DNS_LOGCATEGORY_GENERAL,
					      DNS_LOGMODULE_JOURNAL,
					      ISC_LOG_ERROR,
					      "%s: journal file corrupt, "
					      "transaction too large",
					      j1->filename);
				CHECK(ISC_R_FAILURE);
			}
			buf = isc_mem_get(mctx, size);
			result = journal_read(j1, buf, size);

			/*
			 * If we're repairing an outdated journal, the
			 * xhdr format may be wrong.
			 */
			if (rewrite && (result != ISC_R_SUCCESS ||
					!check_delta(buf, size)))
			{
				if (j1->xhdr_version == XHDR_VERSION2) {
					/* XHDR_VERSION2 -> XHDR_VERSION1 */
					j1->xhdr_version = XHDR_VERSION1;
					CHECK(journal_seek(j1, offset));
					CHECK(journal_read_xhdr(j1, &xhdr));
				} else if (j1->xhdr_version == XHDR_VERSION1) {
					/* XHDR_VERSION1 -> XHDR_VERSION2 */
					j1->xhdr_version = XHDR_VERSION2;
					CHECK(journal_seek(j1, offset));
					CHECK(journal_read_xhdr(j1, &xhdr));
				}

				/* Check again */
				isc_mem_put(mctx, buf, size);
				size = xhdr.size;
				if (size > len) {
					isc_log_write(
						DNS_LOGCATEGORY_GENERAL,
						DNS_LOGMODULE_JOURNAL,
						ISC_LOG_ERROR,
						"%s: journal file corrupt, "
						"transaction too large",
						j1->filename);
					CHECK(ISC_R_FAILURE);
				}
				buf = isc_mem_get(mctx, size);
				CHECK(journal_read(j1, buf, size));

				if (!check_delta(buf, size)) {
					CHECK(ISC_R_UNEXPECTED);
				}
			} else {
				CHECK(result);
			}

			/*
			 * Recover from incorrectly written transaction header.
			 * The incorrect header was written as size, serial0,
			 * serial1, and 0.  XHDR_VERSION2 is expecting size,
			 * count, serial0, and serial1.
			 */
			if (j1->xhdr_version == XHDR_VERSION2 &&
			    xhdr.count == serial && xhdr.serial1 == 0U &&
			    isc_serial_gt(xhdr.serial0, xhdr.count))
			{
				xhdr.serial1 = xhdr.serial0;
				xhdr.serial0 = xhdr.count;
				xhdr.count = 0;
			}

			/*
			 * Check that xhdr is consistent.
			 */
			if (xhdr.serial0 != serial ||
			    isc_serial_le(xhdr.serial1, xhdr.serial0))
			{
				CHECK(ISC_R_UNEXPECTED);
			}

			/*
			 * Extract record count from the transaction.  This
			 * is needed when converting from XHDR_VERSION1 to
			 * XHDR_VERSION2, and when recovering from an
			 * incorrectly written XHDR_VERSION2.
			 */
			count = rrcount(buf, size);
			CHECK(journal_write_xhdr(j2, xhdr.size, count,
						 xhdr.serial0, xhdr.serial1));
			CHECK(journal_write(j2, buf, size));

			j2->header.end.offset = j2->offset;

			serial = xhdr.serial1;

			len = j1->header.end.offset - j1->offset;
			isc_mem_put(mctx, buf, size);
		}

		/*
		 * If we're not rewriting transaction headers, we can use
		 * this faster method instead.
		 */
		if (!rewrite) {
			size = ISC_MIN(64 * 1024, len);
			buf = isc_mem_get(mctx, size);
			for (i = 0; i < len; i += size) {
				unsigned int blob = ISC_MIN(size, len - i);
				CHECK(journal_read(j1, buf, blob));
				CHECK(journal_write(j2, buf, blob));
			}

			j2->header.end.offset = indexend + len;
		}

		CHECK(journal_fsync(j2));

		/*
		 * Update the journal header.
		 */
		journal_header_encode(&j2->header, &rawheader);
		CHECK(journal_seek(j2, 0));
		CHECK(journal_write(j2, &rawheader, sizeof(rawheader)));
		CHECK(journal_fsync(j2));

		/*
		 * Build new index.
		 */
		current_pos = j2->header.begin;
		while (current_pos.serial != j2->header.end.serial) {
			index_add(j2, &current_pos);
			CHECK(journal_next(j2, &current_pos));
		}

		/*
		 * Write index.
		 */
		CHECK(index_to_disk(j2));
		CHECK(journal_fsync(j2));

		indexend = j2->header.end.offset;
		POST(indexend);
	}

	/*
	 * Close both journals before trying to rename files.
	 */
	dns_journal_destroy(&j1);
	dns_journal_destroy(&j2);

	/*
	 * With a UFS file system this should just succeed and be atomic.
	 * Any IXFR outs will just continue and the old journal will be
	 * removed on final close.
	 *
	 * With MSDOS / NTFS we need to do a two stage rename, triggered
	 * by EEXIST.  (If any IXFR's are running in other threads, however,
	 * this will fail, and the journal will not be compacted.  But
	 * if so, hopefully they'll be finished by the next time we
	 * compact.)
	 */
	if (rename(newname, filename) == -1) {
		if (errno == EEXIST && !is_backup) {
			result = isc_file_remove(backup);
			if (result != ISC_R_SUCCESS &&
			    result != ISC_R_FILENOTFOUND)
			{
				goto failure;
			}
			if (rename(filename, backup) == -1) {
				goto maperrno;
			}
			if (rename(newname, filename) == -1) {
				goto maperrno;
			}
			(void)isc_file_remove(backup);
		} else {
		maperrno:
			result = ISC_R_FAILURE;
			goto failure;
		}
	}

	result = ISC_R_SUCCESS;

failure:
	(void)isc_file_remove(newname);
	if (buf != NULL) {
		isc_mem_put(mctx, buf, size);
	}
	if (j1 != NULL) {
		dns_journal_destroy(&j1);
	}
	if (j2 != NULL) {
		dns_journal_destroy(&j2);
	}
	return result;
}

static isc_result_t
index_to_disk(dns_journal_t *j) {
	isc_result_t result = ISC_R_SUCCESS;

	if (j->header.index_size != 0) {
		unsigned int i;
		unsigned char *p;
		unsigned int rawbytes;

		rawbytes = ISC_CHECKED_MUL(j->header.index_size,
					   sizeof(journal_rawpos_t));

		p = j->rawindex;
		for (i = 0; i < j->header.index_size; i++) {
			encode_uint32(j->index[i].serial, p);
			p += 4;
			encode_uint32(j->index[i].offset, p);
			p += 4;
		}
		INSIST(p == j->rawindex + rawbytes);

		CHECK(journal_seek(j, sizeof(journal_rawheader_t)));
		CHECK(journal_write(j, j->rawindex, rawbytes));
	}
failure:
	return result;
}
