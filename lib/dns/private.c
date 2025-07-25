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

#include <stdbool.h>

#include <isc/base64.h>
#include <isc/result.h>
#include <isc/string.h>
#include <isc/types.h>
#include <isc/util.h>

#include <dns/nsec3.h>
#include <dns/private.h>

/*
 * We need to build the relevant chain if there exists a NSEC/NSEC3PARAM
 * at the apex; normally only one or the other of NSEC/NSEC3PARAM will exist.
 *
 * If a NSEC3PARAM RRset exists then we will need to build a NSEC chain
 * if all the NSEC3PARAM records (and associated chains) are slated for
 * destruction and we have not been told to NOT build the NSEC chain.
 *
 * If the NSEC set exist then check to see if there is a request to create
 * a NSEC3 chain.
 *
 * If neither NSEC/NSEC3PARAM RRsets exist at the origin and the private
 * type exists then we need to examine it to determine if NSEC3 chain has
 * been requested to be built otherwise a NSEC chain needs to be built.
 */

#define REMOVE(x)  (((x) & DNS_NSEC3FLAG_REMOVE) != 0)
#define CREATE(x)  (((x) & DNS_NSEC3FLAG_CREATE) != 0)
#define INITIAL(x) (((x) & DNS_NSEC3FLAG_INITIAL) != 0)
#define NONSEC(x)  (((x) & DNS_NSEC3FLAG_NONSEC) != 0)

#define CHECK(x)                             \
	do {                                 \
		result = (x);                \
		if (result != ISC_R_SUCCESS) \
			goto failure;        \
	} while (0)

/*
 * Work out if 'param' should be ignored or not (i.e. it is in the process
 * of being removed).
 *
 * Note: we 'belt-and-braces' here by also checking for a CREATE private
 * record and keep the param record in this case.
 */

static bool
ignore(dns_rdata_t *param, dns_rdataset_t *privateset) {
	DNS_RDATASET_FOREACH (privateset) {
		unsigned char buf[DNS_NSEC3PARAM_BUFFERSIZE];
		dns_rdata_t private = DNS_RDATA_INIT;
		dns_rdata_t rdata = DNS_RDATA_INIT;

		dns_rdataset_current(privateset, &private);
		if (!dns_nsec3param_fromprivate(&private, &rdata, buf,
						sizeof(buf)))
		{
			continue;
		}
		/*
		 * We are going to create a new NSEC3 chain so it
		 * doesn't matter if we are removing this one.
		 */
		if (CREATE(rdata.data[1])) {
			return false;
		}
		if (rdata.data[0] != param->data[0] ||
		    rdata.data[2] != param->data[2] ||
		    rdata.data[3] != param->data[3] ||
		    rdata.data[4] != param->data[4] ||
		    memcmp(&rdata.data[5], &param->data[5], param->data[4]))
		{
			continue;
		}
		/*
		 * The removal of this NSEC3 chain does NOT cause a
		 * NSEC chain to be created so we don't need to tell
		 * the caller that it will be removed.
		 */
		if (NONSEC(rdata.data[1])) {
			return false;
		}
		return true;
	}
	return false;
}

isc_result_t
dns_private_chains(dns_db_t *db, dns_dbversion_t *ver,
		   dns_rdatatype_t privatetype, bool *build_nsec,
		   bool *build_nsec3) {
	dns_dbnode_t *node;
	dns_rdataset_t nsecset, nsec3paramset, privateset;
	bool nsec3chain;
	bool signing;
	isc_result_t result;
	unsigned char buf[DNS_NSEC3PARAM_BUFFERSIZE];
	unsigned int count;

	node = NULL;
	dns_rdataset_init(&nsecset);
	dns_rdataset_init(&nsec3paramset);
	dns_rdataset_init(&privateset);

	CHECK(dns_db_getoriginnode(db, &node));

	result = dns_db_findrdataset(db, node, ver, dns_rdatatype_nsec, 0,
				     (isc_stdtime_t)0, &nsecset, NULL);

	if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND) {
		goto failure;
	}

	result = dns_db_findrdataset(db, node, ver, dns_rdatatype_nsec3param, 0,
				     (isc_stdtime_t)0, &nsec3paramset, NULL);
	if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND) {
		goto failure;
	}

	if (dns_rdataset_isassociated(&nsecset) &&
	    dns_rdataset_isassociated(&nsec3paramset))
	{
		SET_IF_NOT_NULL(build_nsec, true);
		SET_IF_NOT_NULL(build_nsec3, true);
		goto success;
	}

	if (privatetype != (dns_rdatatype_t)0) {
		result = dns_db_findrdataset(db, node, ver, privatetype, 0,
					     (isc_stdtime_t)0, &privateset,
					     NULL);
		if (result != ISC_R_SUCCESS && result != ISC_R_NOTFOUND) {
			goto failure;
		}
	}

	/*
	 * Look to see if we also need to be creating a NSEC3 chain.
	 */
	if (dns_rdataset_isassociated(&nsecset)) {
		SET_IF_NOT_NULL(build_nsec, true);
		SET_IF_NOT_NULL(build_nsec3, false);
		if (!dns_rdataset_isassociated(&privateset)) {
			goto success;
		}
		DNS_RDATASET_FOREACH (&privateset) {
			dns_rdata_t private = DNS_RDATA_INIT;
			dns_rdata_t rdata = DNS_RDATA_INIT;

			dns_rdataset_current(&privateset, &private);
			if (!dns_nsec3param_fromprivate(&private, &rdata, buf,
							sizeof(buf)))
			{
				continue;
			}
			if (REMOVE(rdata.data[1])) {
				continue;
			}
			if (build_nsec3 != NULL) {
				*build_nsec3 = true;
			}
			break;
		}
		goto success;
	}

	if (dns_rdataset_isassociated(&nsec3paramset)) {
		SET_IF_NOT_NULL(build_nsec3, true);
		SET_IF_NOT_NULL(build_nsec, false);
		if (!dns_rdataset_isassociated(&privateset)) {
			goto success;
		}
		/*
		 * If we are in the process of building a new NSEC3 chain
		 * then we don't need to build a NSEC chain.
		 */
		DNS_RDATASET_FOREACH (&privateset) {
			dns_rdata_t private = DNS_RDATA_INIT;
			dns_rdata_t rdata = DNS_RDATA_INIT;

			dns_rdataset_current(&privateset, &private);
			if (!dns_nsec3param_fromprivate(&private, &rdata, buf,
							sizeof(buf)))
			{
				continue;
			}
			if (CREATE(rdata.data[1])) {
				goto success;
			}
		}

		/*
		 * Check to see if there will be a active NSEC3CHAIN once
		 * the changes queued complete.
		 */
		count = 0;
		DNS_RDATASET_FOREACH (&nsec3paramset) {
			dns_rdata_t rdata = DNS_RDATA_INIT;

			/*
			 * If there is more that one NSEC3 chain present then
			 * we don't need to construct a NSEC chain.
			 */
			if (++count > 1) {
				goto success;
			}
			dns_rdataset_current(&nsec3paramset, &rdata);
			if (ignore(&rdata, &privateset)) {
				continue;
			}
			/*
			 * We still have a good NSEC3 chain or we are
			 * not creating a NSEC chain as NONSEC is set.
			 */
			goto success;
		}

		/*
		 * The last NSEC3 chain is being removed and does not have
		 * have NONSEC set.
		 */
		if (build_nsec != NULL) {
			*build_nsec = true;
		}
		goto success;
	}

	SET_IF_NOT_NULL(build_nsec, false);
	SET_IF_NOT_NULL(build_nsec3, false);
	if (!dns_rdataset_isassociated(&privateset)) {
		goto success;
	}

	signing = false;
	nsec3chain = false;

	DNS_RDATASET_FOREACH (&privateset) {
		dns_rdata_t rdata = DNS_RDATA_INIT;
		dns_rdata_t private = DNS_RDATA_INIT;

		dns_rdataset_current(&privateset, &private);
		if (!dns_nsec3param_fromprivate(&private, &rdata, buf,
						sizeof(buf)))
		{
			/*
			 * Look for record that says we are signing the
			 * zone with a key.
			 */
			if (private.length == 5 && private.data[0] != 0 &&
			    private.data[3] == 0 && private.data[4] == 0)
			{
				signing = true;
			}
		} else {
			if (CREATE(rdata.data[1])) {
				nsec3chain = true;
			}
		}
	}

	if (signing) {
		if (nsec3chain) {
			if (build_nsec3 != NULL) {
				*build_nsec3 = true;
			}
		} else {
			if (build_nsec != NULL) {
				*build_nsec = true;
			}
		}
	}

success:
	result = ISC_R_SUCCESS;
failure:
	if (dns_rdataset_isassociated(&nsecset)) {
		dns_rdataset_disassociate(&nsecset);
	}
	if (dns_rdataset_isassociated(&nsec3paramset)) {
		dns_rdataset_disassociate(&nsec3paramset);
	}
	if (dns_rdataset_isassociated(&privateset)) {
		dns_rdataset_disassociate(&privateset);
	}
	if (node != NULL) {
		dns_db_detachnode(db, &node);
	}
	return result;
}

isc_result_t
dns_private_totext(dns_rdata_t *private, isc_buffer_t *buf) {
	isc_result_t result;

	if (private->length < 5) {
		return ISC_R_NOTFOUND;
	}

	if (private->data[0] == 0) {
		unsigned char nsec3buf[DNS_NSEC3PARAM_BUFFERSIZE];
		unsigned char newbuf[DNS_NSEC3PARAM_BUFFERSIZE];
		dns_rdata_t rdata = DNS_RDATA_INIT;
		dns_rdata_nsec3param_t nsec3param;
		bool del, init, nonsec;
		isc_buffer_t b;

		if (!dns_nsec3param_fromprivate(private, &rdata, nsec3buf,
						sizeof(nsec3buf)))
		{
			CHECK(ISC_R_FAILURE);
		}

		CHECK(dns_rdata_tostruct(&rdata, &nsec3param, NULL));

		del = ((nsec3param.flags & DNS_NSEC3FLAG_REMOVE) != 0);
		init = ((nsec3param.flags & DNS_NSEC3FLAG_INITIAL) != 0);
		nonsec = ((nsec3param.flags & DNS_NSEC3FLAG_NONSEC) != 0);

		nsec3param.flags &=
			~(DNS_NSEC3FLAG_CREATE | DNS_NSEC3FLAG_REMOVE |
			  DNS_NSEC3FLAG_INITIAL | DNS_NSEC3FLAG_NONSEC);

		if (init) {
			isc_buffer_putstr(buf, "Pending NSEC3 chain ");
		} else if (del) {
			isc_buffer_putstr(buf, "Removing NSEC3 chain ");
		} else {
			isc_buffer_putstr(buf, "Creating NSEC3 chain ");
		}

		dns_rdata_reset(&rdata);
		isc_buffer_init(&b, newbuf, sizeof(newbuf));
		CHECK(dns_rdata_fromstruct(&rdata, dns_rdataclass_in,
					   dns_rdatatype_nsec3param,
					   &nsec3param, &b));

		CHECK(dns_rdata_totext(&rdata, NULL, buf));

		if (del && !nonsec) {
			isc_buffer_putstr(buf, " / creating NSEC chain");
		}
	} else if (private->length == 5) {
		/* Old Form */
		unsigned char alg = private->data[0];
		dns_keytag_t keyid = (private->data[2] | private->data[1] << 8);
		char keybuf[DNS_SECALG_FORMATSIZE + BUFSIZ],
			algbuf[DNS_SECALG_FORMATSIZE];
		bool del = private->data[3];
		bool complete = private->data[4];

		if (del && complete) {
			isc_buffer_putstr(buf, "Done removing signatures for ");
		} else if (del) {
			isc_buffer_putstr(buf, "Removing signatures for ");
		} else if (complete) {
			isc_buffer_putstr(buf, "Done signing with ");
		} else {
			isc_buffer_putstr(buf, "Signing with ");
		}

		dns_secalg_format(alg, algbuf, sizeof(algbuf));
		snprintf(keybuf, sizeof(keybuf), "key %d/%s", keyid, algbuf);
		isc_buffer_putstr(buf, keybuf);
	} else if (private->length == 7) {
		/* New Form - supports private types */
		dns_keytag_t keyid = private->data[2] | (private->data[1] << 8);
		char keybuf[DNS_SECALG_FORMATSIZE + BUFSIZ],
			algbuf[DNS_SECALG_FORMATSIZE];
		bool del = private->data[3];
		bool complete = private->data[4];
		dst_algorithm_t alg = private->data[6] |
				      (private->data[5] << 8);

		if (dst_algorithm_tosecalg(alg) != private->data[0]) {
			return ISC_R_NOTFOUND;
		}

		if (del && complete) {
			isc_buffer_putstr(buf, "Done removing signatures for ");
		} else if (del) {
			isc_buffer_putstr(buf, "Removing signatures for ");
		} else if (complete) {
			isc_buffer_putstr(buf, "Done signing with ");
		} else {
			isc_buffer_putstr(buf, "Signing with ");
		}

		dns_secalg_format(alg, algbuf, sizeof(algbuf));
		snprintf(keybuf, sizeof(keybuf), "key %d/%s", keyid, algbuf);
		isc_buffer_putstr(buf, keybuf);
	} else {
		return ISC_R_NOTFOUND;
	}

	isc_buffer_putuint8(buf, 0);
	result = ISC_R_SUCCESS;
failure:
	return result;
}
