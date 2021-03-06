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
#include "timetrace.h"
#include "lint.h"
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

SSLState ssl_state = SSL_CLOSED;
static BIO *bio = NULL;
static SSL_CTX *ctx = NULL;
static SSL *ssl = NULL;


static void ssl_alert_callback(const SSL *s, int where, int ret) {
	const char *str;
	int w = where & ~SSL_ST_MASK;

	if (w & SSL_ST_CONNECT)
		return;

	/*	if (w & SSL_ST_CONNECT)
			str = "SSL_connect";
		else */if (w & SSL_ST_ACCEPT)
		str = "SSL_accept";
	else
		str = "undefined";

	if (where & SSL_CB_LOOP) {
		printf("Alert: %s:%s\n", str, SSL_state_string_long(s));
		fflush(0);
	}
	else if (where & SSL_CB_ALERT) {
		str = (where & SSL_CB_READ) ? "read" : "write";
		printf("Alert: SSL3 alert %s:%s:%s\n", str,
			   SSL_alert_type_string_long(ret),
			   SSL_alert_desc_string_long(ret));
		fflush(0);
	}
	else if (where & SSL_CB_EXIT) {
		if (ret == 0) {
			printf("Alert: %s:failed in %s\n",
				   str, SSL_state_string_long(s));
			fflush(0);
		}
		else if (ret < 0) {
			printf("Alert: %s:error in %s\n",
				   str, SSL_state_string_long(s));
			fflush(0);
		}
	}
}

int ssl_status_check(void) {
	if (ssl == NULL)
		return 0;

	fd_set readfds;
	FD_ZERO(&readfds);
	int fd = SSL_get_fd(ssl);
	FD_SET(fd, &readfds);
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 1;

	int rv = select(fd + 1, &readfds, NULL, NULL, &timeout);
	if (rv <= 0)
		return 0;

	if (FD_ISSET(fd, &readfds)) {
		printf("incoming data\n");
		return 1;
	}

	return 0;
}

void ssl_init(void) {
	SSL_load_error_strings();
	SSL_library_init();
	ERR_load_BIO_strings();
	OpenSSL_add_all_algorithms();
}

static char *certlist[] = {
	"/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu
	"/etc/ssl/certs/ca-bundle.crt", // Fedora/CentOS
	NULL
};

char *get_cert_file(void) {
	if (arg_certfile)
		return arg_certfile;

	int i = 0;
	while (certlist[i]) {
		struct stat s;
		if (stat(certlist[i], &s) == 0)
			return certlist[i];
		i++;
	}

	return NULL;
}

void ssl_open(void) {
	assert(ssl_state == SSL_CLOSED);
	DnsServer *srv = server_get();
	assert(srv);

	if (arg_fallback_only)
		return;

	if (ctx == NULL) {
		ctx = SSL_CTX_new(TLS_client_method());
		char *certfile = get_cert_file();
		if (certfile == NULL) {
			if(! SSL_CTX_load_verify_locations(ctx, NULL, "/etc/ssl/certs")) {
				rlogprintf("Error: cannot find SSL certificates in /etc/ssl/certs\n");
				exit(1);
			}
		}
		else {
			if(! SSL_CTX_load_verify_locations(ctx, certfile, NULL)) {
				rlogprintf("Error: cannot find SSL certificate %s\n", certfile);
				exit(1);
			}
		}
	}

	bio = BIO_new_ssl_connect(ctx);
	BIO_get_ssl(bio, &ssl);
	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

	// set connection and SNI
	BIO_set_conn_hostname(bio, srv->address);
	if (srv->sni)
		SSL_set_tlsext_host_name(ssl, srv->host);

	if(BIO_do_connect(bio) <= 0) {
//		rlogprintf("Error: cannot connect SSL.\n");
		return;
	}

	int val;
	if ((val = SSL_get_verify_result(ssl)) != X509_V_OK) {
		rlogprintf("Error: cannot handle certificate verification (error %d), shutting down...\n", val);
		return;	// give the program a chance to switch to fallback
	}

	// set alert callback
	SSL_set_info_callback(ssl, ssl_alert_callback);


	ssl_state = SSL_OPEN;
	rlogprintf("SSL connection opened\n");
}

void ssl_close(void) {
	if (ssl) {
		int rv = SSL_shutdown(ssl);
		if (rv == 0)
			SSL_shutdown(ssl);
		SSL_free(ssl);
	}

	ssl = NULL;
	ssl_state = SSL_CLOSED;
	rlogprintf("SSL connection closed\n");
}

// returns the length of the response,0 if failed
int ssl_rxtx_dns(uint8_t *msg, int cnt) {
	assert(msg);

	DnsServer *srv = server_get();
	assert(srv);

	if (ssl == NULL || ssl_state != SSL_OPEN)
		return 0;

	assert(bio);
	assert(ctx);
	assert(ssl);

	char buf[MAXBUF];
	sprintf(buf, srv->request, cnt);
	int len = strlen(buf);
	assert(cnt < MAXBUF - len);

	memcpy(buf + len, msg, cnt);
	len += cnt;

	if (arg_debug)
		printf("(%d) *** SSL transaction ***\n", arg_id);

	int lentx;
	if((lentx = BIO_write(bio, buf, len)) <= 0) {
		if(! BIO_should_retry(bio)) {
			rlogprintf("Error: failed SSL write, retval %d\n", lentx);
			goto errout;
		}
		if((lentx = BIO_write(bio, buf, len)) <= 0) {
			rlogprintf("Error: failed SSL write, retval %d\n", lentx);
			goto errout;
		}
	}

	if (arg_debug)
		printf("(%d) SSL write %d/%d bytes\n", arg_id, len, lentx);

	len = BIO_read(bio, buf, MAXBUF);
	if(len <= 0) {
		if(! BIO_should_retry(bio)) {
			rlogprintf("Error: failed SSL read, retval %d\n", len);
			goto errout;
		}
		len = BIO_read(bio, buf, MAXBUF);
		if(len <= 0) {
			rlogprintf("Error: failed SSL read, retval %d\n", len);
			goto errout;
		}
	}
	buf[len] = '\0';

	// check 200 OK
	char *ptr = strstr(buf, "200 OK");
	if (!ptr) {
		rlogprintf("Warning: HTTP error, 200 OK not received\n");
		printf("**************\n%s\n**************\n", buf);
		fflush(0);
		goto errout;
	}

	// look for the end of http header
	ptr = strstr(buf, "\r\n\r\n");
	if (!ptr) {
		rlogprintf("Warning: cannot parse HTTPS response, didn't recieve a full http header\n");
		printf("**************\n%s\n**************\n", buf);
		fflush(0);
		goto errout;
	}
	ptr += 4; // length of "\r\n\r\n"
	ptrdiff_t hlen = ptr - buf; // +4 is the length of \r\n\r\n
	*(ptr - 1) = 0;
	if (arg_debug)
		printf("(%d) http header:\n%s", arg_id, buf);

	// look for Content-Length:
	char *contlen = "Content-Length: ";
	ptr = strcasestr(buf, contlen);
	int datalen = 0;
	if (!ptr) {
		rlogprintf("Warning: cannot parse HTTPS response, content-length missing\n");
		print_mem((uint8_t *) buf, len);
		goto errout;
	}
	else {
		ptr += strlen(contlen);
		sscanf(ptr, "%d", &datalen);
		if (datalen == 0) // we got a "Content-lenght: 0"; this is probably a HTTP error
			return 0;
	}

	// do we need to read more data?
	int totallen = (int) hlen + datalen;
	if (arg_debug)
		printf("(%d) SSL read len %d, totallen %d, datalen %d\n",
		       arg_id, len, totallen, datalen);
	if (totallen >= MAXBUF) {
		rlogprintf("Warning: cannot parse HTTPS response, invalid length\n");
		print_mem((uint8_t *) buf, len);
		goto errout;
	}

	while (len < totallen) {
		int rv = BIO_read(bio, buf + len, totallen - len);
		if (arg_debug)
			printf("(%d) SSL read + %d\n", arg_id, rv);
		if(rv <= 0) {
			if(! BIO_should_retry(bio)) {
				rlogprintf("Error: failed SSL read\n");
				goto errout;
			}
			rv = BIO_read(bio, buf, MAXBUF);
			if (arg_debug)
				printf("(%d) SSL read + %d\n", arg_id, rv);
			if(rv <= 0) {
				rlogprintf("Error: SSL connection is probably closed\n");
				goto errout;
			}
		}

		len += rv;
	}

	// copy the response in buf
	memcpy(msg, buf + len - datalen, datalen);
	if (arg_debug) {
		printf("(%d) DNS data:\n", arg_id);
		print_mem((uint8_t *) buf, datalen);
		printf("(%d) *** SSL transaction end ***\n", arg_id);
	}

	//
	// partial response parsing
	//
	if (lint_rx(msg, datalen)) {
		if (lint_error() == DNSERR_NXDOMAIN) {
			// NXDOMAIN or similar received, cache for 10 minutes
			cache_set_reply(msg, datalen, CACHE_TTL_ERROR);
			return datalen;
		}

		// serveral adblocker/family services return addresses of 0.0.0.0 or 127.0.0.1 for blocked domains
		const char *str = lint_err2str();
		if (strstr(str, "0.0.0.0") || strstr(str, "127.0.0.1")) {
			// set NXDOMAIN bytes in the packet
			msg[3] = 3;
			rlogprintf("%s refused by service provider\n", cache_get_name());
			return datalen;
		}
		else
			rlogprintf("Error: %s %s\n", lint_err2str(), cache_get_name());
		return 0;
	}

	// cache the response and exit
	cache_set_reply(msg, datalen, arg_cache_ttl);
	return datalen;

errout:
	ssl_close();
	return 0;
}

