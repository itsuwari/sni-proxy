/* Copyright (c) 2015, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#include "ev.h"
#include "ucl.h"
#include "util.h"

struct ssl_session {
	const ucl_object_t *backends;
	int fd;
	ev_io io;
	struct ev_loop *loop;
	char *hostname;
	enum {
		ssl_state_init = 0,
		ssl_state_alert,
		ssl_state_alert_sent,
	} state;
};

#if defined(__GNUC__)
#  define _PACKED __attribute__ ((packed))
#else
#  define _PACKED
#endif

#if !defined(__GNUC__)
#  ifdef __IBMC__
#    pragma pack(1)
#  else
#    pragma pack(push, 1)
#  endif
#endif

static const unsigned char tls_magic[3] = {0x16, 0x3, 0x1};
static const unsigned int tls_greeting = 0x1;
static const unsigned int sni_type = 0x0;
static const unsigned int sni_host = 0x0;
static const unsigned int tls_alert = 0x15;
static const unsigned int tls_alert_level = 0x2;
static const unsigned int tls_alert_description = 0x40;

struct ssl_header {
	unsigned char tls_magic[3];
	uint8_t len[2];
	uint8_t type;
	uint8_t greeting_len[3];
	uint16_t version;
	uint8_t random[32];
} _PACKED;

struct sni_ext {
	uint8_t slen[2];
	uint8_t type;
	uint8_t hlen[2];
	uint8_t host[1]; /* Extended */
} _PACKED;

struct ssl_alert {
	uint8_t type;
	uint8_t version[2];
	uint8_t len[2];
	uint8_t level;
	uint8_t description;
} _PACKED;

static inline unsigned int
int_3byte_le(const unsigned char *p) {
	return
	(((unsigned int)(p[0])      ) |
	 ((unsigned int)(p[1]) <<  8) |
	 ((unsigned int)(p[2]) << 16));
}

static inline unsigned int
int_2byte_le(const unsigned char *p) {
	return
	(((unsigned int)(p[0])      ) |
	 ((unsigned int)(p[1]) <<  8));
}

static void
terminate_session(struct ssl_session *ssl)
{
	ev_io_stop(ssl->loop, &ssl->io);
	close(ssl->fd);
	free(ssl->hostname);
	free(ssl);
}

static void
alert_cb(EV_P_ ev_io *w, int revents)
{
	struct ssl_alert alert;
	struct ssl_session *ssl = w->data;

	if (ssl->state == ssl_state_alert) {
		alert.type = tls_alert;
		alert.version[0] = 0x03;
		alert.version[0] = 0x01;
		alert.len[1] = 2;
		alert.level = tls_alert_level;
		alert.description = tls_alert_description;
		ssl->state = ssl_state_alert_sent;

		write(ssl->fd, &alert, sizeof(alert));
	}
	else {
		terminate_session(ssl);
	}
}

static void
send_alert(struct ssl_session *ssl)
{
	ssl->state = ssl_state_alert;
	ev_io_init(&ssl->io, alert_cb, ssl->fd, EV_WRITE);
	ev_io_start(ssl->loop, &ssl->io);
}

static int
parse_extension(struct ssl_session *ssl, const unsigned char *pos, int remain)
{
	unsigned int tlen, type, hlen;
	const struct sni_ext *sni;

	if (remain < 0) {
		return -1;
	}
	if (remain < 4) {
		return 0;
	}

	type = int_2byte_le(pos);
	tlen = int_2byte_le(pos + 2);

	if (tlen > remain) {
		return -1;
	}

	if (type == sni_type) {
		if (tlen < sizeof(*sni)) {
			return -1;
		}

		sni = (const struct sni_ext *)pos + 4;
		if (int_2byte_le(sni->slen) != tlen - 2 ||
			sni->type != sni_host ||
			int_2byte_le(sni->hlen) != tlen - 3) {
			return -1;
		}

		hlen = int_2byte_le(sni->hlen);
		ssl->hostname = xmalloc(hlen + 1);
		memcpy(ssl->hostname, sni->host, hlen);
		ssl->hostname[hlen] = '\0';
	}

	return tlen + 4;
}

static void
parse_ssl_greeting(struct ssl_session *ssl, const char *buf, int len)
{
	const char *p = buf;
	int remain = len, ret;
	unsigned int tlen;
	const struct ssl_header *sslh;

	ev_io_stop(ssl->loop, &ssl->io);

	if (len <= sizeof(struct ssl_header)) {
		send_alert(ssl);
		return;
	}

	sslh = (const struct ssl_header *)p;

	/* Not an SSL packet */
	if (memcmp(sslh->tls_magic, tls_magic, sizeof(tls_magic)) != 0 ||
		sslh->type != tls_greeting ||
		int_2byte_le(sslh->len) != len - 5 ||
		int_3byte_le(sslh->greeting_len) != len - 5 - 4) {
		goto err;
	}

	p = p + sizeof(*sslh);
	remain -= sizeof(*sslh);

	/* Session id */
	tlen = *p;
	if (tlen >= remain + 4) {
		goto err;
	}
	p = p + tlen + 1;
	remain -= tlen + 1;

	/* Cipher suite */
	tlen = int_2byte_le(p);
	if (tlen >= remain + 4) {
		goto err;
	}
	p = p + tlen + 2;
	remain -= tlen + 2;

	/* Compression methods */
	tlen = *p;
	if (tlen >= remain + 4) {
		goto err;
	}
	p = p + tlen + 1;
	remain -= tlen + 1;

	/* Now extensions */
	tlen = int_2byte_le(p);
	if (tlen > remain) {
		goto err;
	}

	p += 2;
	remain -= 2;

	while ((ret = parse_extension(ssl, p, remain)) > 0) {
		p += ret;
		remain -= ret;
	}

	if (ret == 0) {
		printf("got hostname: %s\n", ssl->hostname);
		send_alert(ssl);
		return;
	}
err:
	send_alert(ssl);
}

static void
greet_cb(EV_P_ ev_io *w, int revents)
{
	unsigned char buf[8192];
	int r;
	struct ssl_session *ssl = w->data;

	r = read(w->fd, buf, sizeof (buf));

	if (r <= 0) {
		terminate_session(ssl);
	}
	else {
		parse_ssl_greeting(ssl, buf, r);
	}
}

static int
accept_from_socket(int sock)
{
	int nfd, serrno, ofl;

	if ((nfd = accept (sock, NULL, 0)) == -1) {
		if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK) {
			return 0;
		}

		return -1;
	}

	ofl = fcntl(sock, F_GETFL, 0);

	if (fcntl(sock, F_SETFL, ofl | O_NONBLOCK) == -1) {
		close(sock);

		goto out;
	}

	/* Set close on exec */
	if (fcntl (nfd, F_SETFD, FD_CLOEXEC) == -1) {
		fprintf(stderr, "fcntl failed: %d, '%s'\n", errno, strerror (errno));
		goto out;
	}

	return (nfd);

out:
	serrno = errno;
	close (nfd);
	errno = serrno;

	return (-1);

}

static void
accept_cb(EV_P_ ev_io *w, int revents)
{
	int nfd;
	struct ssl_session *ssl;

	if ((nfd = accept_from_socket(w->fd)) > 0) {
		ssl = xmalloc0(sizeof(*ssl));
		ssl->io.data = ssl;
		ssl->backends = w->data;
		ssl->loop = loop;
		ssl->fd = nfd;
		ev_io_init(&ssl->io, greet_cb, nfd, EV_READ);
		ev_io_start(loop, &ssl->io);
	}
	else {
		fprintf(stderr, "accept failed: %d, '%s'\n", errno, strerror (errno));
	}
}

static int
listen_on(const struct sockaddr *sa, socklen_t slen)
{
	int sock, on, ofl;

	sock = socket(sa->sa_family, SOCK_STREAM, 0);

	if (sock == -1) {
		return -1;
	}

	if (fcntl(sock, F_SETFD, FD_CLOEXEC) == -1) {
		close(sock);

		return -1;
	}

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void *)&on, sizeof (int));
	ofl = fcntl(sock, F_GETFL, 0);

	if (fcntl(sock, F_SETFL, ofl | O_NONBLOCK) == -1) {
		close(sock);

		return -1;
	}

	if (bind(sock, sa, slen) == -1) {
		close(sock);

		return -1;
	}

	if (listen(sock, -1) == -1) {
		close(sock);

		return -1;
	}

	return sock;
}

bool
start_listen(struct ev_loop *loop, int port, const ucl_object_t *backends)
{
	struct addrinfo ai, *res, *cur_ai;
	int sock, ret;
	ev_io *watcher;

	memset(&ai, 0, sizeof(ai));
	ai.ai_family = AF_UNSPEC;
	ai.ai_flags = AI_PASSIVE|AI_NUMERICSERV;
	ai.ai_socktype = SOCK_STREAM;

	if ((ret = getaddrinfo(NULL, port_to_str(port), &ai, &res)) != 0) {
		fprintf(stderr, "getaddrinfo: *:%d: %s\n", port, gai_strerror(ret));
		return false;
	}

	cur_ai = res;

	while (cur_ai != NULL) {
		sock = listen_on(cur_ai->ai_addr, cur_ai->ai_addrlen);

		if (sock == -1) {
			fprintf(stderr, "socket listen: %s\n", strerror(errno));
			return false;
		}

		watcher = xmalloc0(sizeof(*watcher));
		watcher->data = (void *)backends;
		ev_io_init(watcher, accept_cb, sock, EV_READ);
		ev_io_start(loop, watcher);
		cur_ai = cur_ai->ai_next;
	}

	return true;
}