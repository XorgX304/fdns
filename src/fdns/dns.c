/*
 * Copyright (C) 2019-2020 FDNS Authors
 *
 * This file is part of fdns project
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "fdns.h"
#include "lint.h"
#include "timetrace.h"

// build a NXDOMAIN package on top of the existing dns request
inline static void build_response_nxdomain(uint8_t *pkt) {
	// lenptr remains unchanged
	pkt[2] = 0x81;
	pkt[3] = 0x83;
}

// attempt to extract the domain name and run it through the filter
uint8_t *dns_parser(uint8_t *buf, ssize_t *lenptr, DnsDestination *dest) {
	assert(buf);
	assert(lenptr);
	uint8_t *pkt = buf;
	uint8_t *last = pkt + *lenptr - 1;	// pointer to last byte in the packet
	*dest = DEST_SSL;

	DnsHeader *h = lint_header(&pkt, last);
	if (!h) {
		rlogprintf("Error LANrx: %s, dropped\n", lint_err2str());
		*dest = DEST_DROP;
		return NULL;
	}

	// check flags
	if (h->flags & 0x8000) {
		rlogprintf("Error LANrx: this is not a DNS query, dropped\n");
		*dest = DEST_DROP;
		return NULL;
	}
	if (h->flags & 0x7800) {
		rlogprintf("Error LANrx:  invalid DNS flags %4x, dropped\n", h->flags);
		*dest = DEST_DROP;
		return NULL;
	}

	// we allow exactly one question
	if (h->questions != 1 || h->answer != 0 || h->authority || h->additional != 0) {
		rlogprintf("Error LANrx: invalid DNS section counts: %x %x %x %x, dropped\n",
			 h->questions, h->answer, h->authority,  h->additional);
		*dest = DEST_DROP;
		return NULL;
	}

	unsigned delta;
	DnsQuestion *q = lint_question(&pkt,  last);
	if (!q) {
		rlogprintf("Error LANrx: %s, dropped\n", lint_err2str());
		*dest = DEST_DROP;
		return NULL;
	}

	// check packet lentght
//printf("domain #%s#, pkg %p, last %p\n", q->domain, pkt, last); fflush(0);
	if (pkt != last + 1) {
		rlogprintf("Error LANrx: invalid packet lenght, dropped\n");
		*dest = DEST_DROP;
		return NULL;
	}

	// clear cache name
	cache_set_name("", 0);

	//******************************
	// query type
	//******************************
	if (arg_allow_all_queries == 0) {
		// type A requests
		if (q->type == 1);

		// AAAA requests
		else if (q->type == 0x1c) {
			if (!arg_ipv6) {
				// stats.rx already incremented by the caller
				stats.rx--;
				// stats.drop incremented automatically in drop_nxdomain
				stats.drop--;
				goto drop_nxdomain;
			}
		}

		// respond NXDOMAIN to PTR in order to fix apps as ping
		else if (q->type == 0x0c) {
			rlogprintf("Request: %s (PTR), dropped\n", q->domain);
			goto drop_nxdomain;
		}

		// drop all the rest
		else {
			char *type = NULL;
			switch (q->type) {
				case 2: type = "NS"; break;
				case 5: type = "CNAME"; break;
				case 6: type = "SOA"; break;
				case 10: type = "NULL"; break;
				case 15: type = "MX"; break;
				case 16: type = "TXT"; break;
				case 25: type = "KEY"; break;
				case 29: type = "LOC"; break;
				case 33: type = "SRV"; break;
				case 255: type = "ANY"; break;
				case 256: type = "URI";break;
				case 65399: type = "PRIVATE"; break;
			}
			if (type)
				rlogprintf("Error LANrx: RR type %s rejected, %s\n", type, q->domain);
			else
				rlogprintf("Error LANrx: RR type %u rejected, %s\n", q->type, q->domain);
			*dest = DEST_DROP; // just let him try again, no NXDOMAIN set out
			return NULL;
		}
	}

	//*****************************
	// whitelist
	//*****************************
	if (whitelist_active()) {
		if (whitelist_blocked(q->domain)) {
			rlogprintf("Request: whitelist  %s%s, dropped\n", q->domain, (q->type == 0x1c) ? " (ipv6)" : "");
			goto drop_nxdomain;
		}
	}

	//*****************************
	// trackers/adblock filtering
	//*****************************
	if (!arg_nofilter) {
		const char *label = filter_blocked(q->domain, 0);
		if (label) {
			rlogprintf("Request: %s  %s%s, dropped\n", label, q->domain, (q->type == 0x1c) ? " (ipv6)" : "");
			goto drop_nxdomain;
		}
	}

	//*****************************
	// drop browser search domains
	// these are requests sent by the browser when you try to search from the URL line
	// RFC 7085 - several dotless domains on record; we should not drop them (todo)
	//*****************************
	if (strchr(q->domain, '.') == NULL) {
		rlogprintf("Request: search %s%s, dropped\n", q->domain, (q->type == 0x1c) ? " (ipv6)" : "");
		goto drop_nxdomain;
	}


	//*****************************
	// cache - only domains smaller than CACHE_NAME_LEN
	//*****************************
	if (q->len <= CACHE_NAME_LEN) {
//printf("******* %u %s\n", q->len, q->domain);
		// check cache
		uint8_t *rv = cache_check(h->id, q->domain, lenptr, (q->type == 0x1c) ? 1 : 0);
		if (rv) {
			stats.cached++;
			rlogprintf("Request: %s%s, cached\n", q->domain, (q->type == 0x1c) ? " (ipv6)" : "");
			*dest = DEST_LOCAL;
			return rv;
		}

		// set the stage for caching the reply
		cache_set_name(q->domain, (q->type == 0x1c) ? 1 : 0);
	}

	//*****************************
	// forwarder
	//*****************************
	if (forwarder_check(q->domain, q->dlen)) {
		rlogprintf("Request: %s%s, forwarded\n", q->domain, (q->type == 0x1c) ? " (ipv6)" : "");
		*dest = DEST_FORWARDING;
		stats.fwd++;
		return NULL;
	}

	rlogprintf("Request: %s%s, %s\n", q->domain, (q->type == 0x1c) ? " (ipv6)" : "",
		   (ssl_state == SSL_OPEN) ? "encrypted" : "not encrypted");

	*dest = DEST_SSL;
	return NULL;

drop_nxdomain:
	stats.drop++;
	build_response_nxdomain(buf);
	*dest = DEST_LOCAL;
	return buf;
}

void dns_keepalive(void) {
	if (arg_debug)
		printf("(%d) send keepalive\n", arg_id);
	uint8_t msg[33] = { // www.example.com
		0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00, 0x03, 0x77, 0x77, 0x77,
		0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65,  0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00, 0x01, 0x00,
		0x01
	};
	uint8_t buf[MAXBUF];
	memcpy(buf, msg, sizeof(msg));
	ssl_rxtx_dns(buf, sizeof(msg));
}
