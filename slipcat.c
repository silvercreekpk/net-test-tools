/*
 * Copyright © 2017-2018, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <arpa/inet.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "slipcat.h"
#include "libslip.h"
#include "queue.h"

#define BUF_MAXSIZE 8192
#define S_IN_SIZE sizeof(struct sockaddr_in)

typedef enum {
	SL_OP_UP,
	SL_OP_DOWN
} sl_op_t;

typedef struct {
	uint8_t *data;
	ssize_t len;
	uint8_t buf[BUF_MAXSIZE];
} sl_data_t;

typedef struct sl sl_t;

typedef int (sl_cb_t)(sl_t *n, sl_op_t d, sl_data_t **data);

struct sl {
	int fd;
	sl_cb_t *cb;
	void *user_data;
	char *name;
	S_QUEUE_ENTRY(sl) e;
};

static int opt_debug = 1;
static int opt_tcp;
static int opt_af_unix;
static int opt_slip;
static int opt_udp;
static int opt_trace;

static char *opt_tcp_src_addr;
static int opt_tcp_src_port;
static char *opt_af_unix_path;
static char *opt_udp_src_addr;
static int opt_udp_src_port;
static char *opt_udp_dst_addr;
static int opt_udp_dst_port;
static char *opt_trace_addr;
static int opt_trace_port;

static S_QUEUE(sl) sl_queue;

sl_data_t *data_new(void)
{
	sl_data_t *d = malloc(sizeof(*d));

	d->data = d->buf;
	d->len = BUF_MAXSIZE;

	return d;
}

sl_data_t *data_new_from_bytes(uint8_t *data, ssize_t data_len)
{
	sl_data_t *d = data_new();

	memcpy(d->data, data, d->len = data_len);

	return d;
}

void data_free(sl_data_t **d)
{
	free(*d);

	*d = NULL;
}

int fd_is_readable(int fd)
{
	uint8_t byte;
	ssize_t r = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
	if (r == 0)
		E("recv");

	return (r > 0);
}

void *s_in(const char *addr, uint16_t port)
{
	static struct sockaddr_in sin;

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(addr);
	sin.sin_port = htons(port);

	return &sin;
}

sl_t *sl_new(const char *name, sl_cb_t *cb)
{
	sl_t *s = calloc(1, sizeof(sl_t));

	s->name = strdup(name);

	s->cb = cb;

	S_QUEUE_INSERT_TAIL(&sl_queue, s, e);

	return s;
}

int sl_send(sl_t *s, sl_op_t op, sl_data_t **d)
{
	int ret = FALSE;

	while (s) {

		ret = s->cb(s, op, d);

		if (ret != TRUE)
			break;

		s = (SL_OP_UP == op) ? S_QUEUE_NEXT(s, e) : S_QUEUE_PREV(s, e);
	}

	return ret;
}

int af_unix_init(void)
{
	int s;
	int v = 1;
	struct stat st;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		E("socket");

	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
			&v, sizeof(v)) == -1)
		E("setsockopt");

	if (stat(opt_af_unix_path, &st) != -1) {
		if (unlink(opt_af_unix_path))
			E("unlink");
	}

	{
		struct sockaddr_un su;
		socklen_t su_len = sizeof(struct sockaddr_un);;

		memset(&su, 0, sizeof(su));

		su.sun_family = AF_UNIX;
		strncpy(su.sun_path, opt_af_unix_path,
			strlen(opt_af_unix_path));

		if (bind(s, (void *) &su, su_len) == -1)
			E("bind");
	}

	for (;;) {
		int fd;
		struct sockaddr_in sin;
		socklen_t sin_size = sizeof(sin);
		if (listen(s, 1) == -1)
			E("listen");
		memset(&sin, 0, sizeof(sin));
		if ((fd = accept(s, (void *) &sin, &sin_size)) == -1)
			E("accept");
		close(s);
		return fd;
	}
}

int af_unix(sl_t *s, sl_op_t op, sl_data_t **data)
{
	sl_data_t *d = *data;
	switch (op) {
	case SL_OP_UP:
		if ((d->len = read(s->fd, d->data, 1)) < 0)
			W("read");
		break;
	case SL_OP_DOWN:
		if ((write(s->fd, d->data, d->len)) < 0)
			W("write");
		break;
	}
	return TRUE;
}

int slip(sl_t *s, sl_op_t op, sl_data_t **data)
{
	sl_data_t *d = *data;
	int ret = FALSE;
	switch (op) {
	case SL_OP_UP: {
		uint8_t *out;
		ssize_t out_len;

		if (libslip_input(s->user_data, *(d->data), &out, &out_len)) {

			sl_data_t *o = data_new_from_bytes(out, out_len);

			data_free(data);

			*data = o;

			ret = TRUE;
		}
		break;
	}
	case SL_OP_DOWN: {
		sl_data_t *out = data_new();

		libslip_output(d->data, d->len, out->data, &out->len);

		data_free(data);

		*data = out;

		ret = TRUE;

		break;
	}}
	return ret;
}

int tcp(sl_t *s, sl_op_t op, sl_data_t **data)
{
	sl_data_t *d = *data;
	switch (op) {
	case SL_OP_UP:
		D("");
		if ((d->len = read(s->fd, d->data, 1)) < 0)
			W("read");
		break;
	case SL_OP_DOWN:
		if ((write(s->fd, d->data, d->len)) < 0)
			W("write");
		break;
	}
	return TRUE;
}

int tcp_init(const char *addr, int port)
{
	int s;
	int v = 1;

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1)
		E("socket");

	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == -1)
		E("setsockopt");

	if (bind(s, s_in(addr, port), S_IN_SIZE) == -1)
		E("bind");

	for (;;) {
		int fd;
		struct sockaddr_in sin;
		socklen_t sin_size = sizeof(sin);
		if (listen(s, 1) == -1)
			E("listen");
		memset(&sin, 0, sizeof(sin));
		if ((fd = accept(s, (void *) &sin, &sin_size)) == -1)
			E("accept");
		close(s);
		return fd;
	}
}

int udp_init(const char *addr, int port)
{
	int s;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		E("socket");

	if (addr && bind(s, s_in(addr, port), S_IN_SIZE) == -1)
		E("bind");

	return s;
}

int udp(sl_t *s, sl_op_t op, sl_data_t **data)
{
	sl_data_t *d = *data;
	switch (op) {
	case SL_OP_UP:
		if (sendto(s->fd, d->data, d->len, 0,
				s_in(opt_udp_dst_addr, opt_udp_dst_port),
				S_IN_SIZE) < 0)
			W("sendto");
		break;
	case SL_OP_DOWN:
		if ((d->len = read(s->fd, d->data, d->len)) < 0)
			W("read");
		break;
	}
	return TRUE;
}

int trace(sl_t *s, sl_op_t op, sl_data_t **data)
{
	sl_data_t *d = *data;

	if (sendto(s->fd, d->data, d->len, 0,
			s_in(opt_trace_addr, opt_trace_port), S_IN_SIZE) < 0)
		W("sendto");

	return TRUE;
}

static void sl_config(void)
{
	sl_t *s;

	if (opt_tcp) {
		s = sl_new("tcp", tcp);
		s->fd = tcp_init(opt_tcp_src_addr, opt_tcp_src_port);
		D("fd=%d", s->fd);
	}

	if (opt_af_unix) {
		s = sl_new("af_unix", af_unix);
		s->fd = af_unix_init();
		D("fd=%d", s->fd);
	}

	if (opt_slip) {
		s = sl_new("slip", slip);
		s->user_data = libslip_init();
	}

	if (opt_trace) {
		s = sl_new("trace", trace);
		s->fd = udp_init(NULL, opt_trace_port);
	}

	if (opt_udp) {
		s = sl_new("udp", udp);
		s->fd = udp_init(opt_udp_src_addr, opt_udp_src_port);
	}
}

static void sl_data_flow(sl_op_t op)
{
	sl_t *s = (op == SL_OP_UP) ? S_QUEUE_HEAD(&sl_queue) :
						S_QUEUE_TAIL(&sl_queue);
	sl_data_t *data;

	if (!fd_is_readable(s->fd))
		return;

	data = data_new();

	sl_send(s, op, &data);

	data_free(&data);
}

static void defaults_config(void)
{
	opt_af_unix = 1;
	opt_af_unix_path = "/tmp/slip.sock";

	opt_slip = 1;

	opt_trace = 1;
	opt_trace_addr = "127.0.0.1";
	opt_trace_port = 5555;

	opt_udp = 1;
	opt_udp_src_addr = "127.0.0.1";
	opt_udp_src_port = 7771;
	opt_udp_dst_addr = "127.0.0.1";
	opt_udp_dst_port = 7777;
}

static void configuration_print(void)
{
	P("af_unix: %s", opt_af_unix ? "Enabled" : "Disabled");

	if (opt_af_unix) {
		P("af_unix: path=%s", opt_af_unix_path);
	}

	P("tcp: %s", opt_tcp ? "Enabled" : "Disabled");

	if (opt_tcp) {
		P("tcp: tcp_src_addr=%s, tcp_src_port=%hu",
			opt_tcp_src_addr, opt_tcp_src_port);
	}

	P("slip: %s", opt_slip ? "Enabled" : "Disabled");
	P("trace: %s", opt_trace ? "Enabled" : "Disabled");

	if (opt_trace) {
		P("trace: addr=%s, port=%hu", opt_trace_addr, opt_trace_port);
	}

	P("udp: %s", opt_udp ? "Enabled" : "Disabled");

	if (opt_udp) {
		P("udp: src_addr=%s, src_port=%hu",
			opt_udp_src_addr, opt_udp_src_port);
		P("udp: dst_addr=%s, dst_port=%hu",
			opt_udp_dst_addr, opt_udp_dst_port);
	}
}

/* TODO: Enhance the configuration (see TODO.txt) */

static void options_parse(int *argc, char **argv[])
{
	GError *error = NULL;
	GOptionContext *context = g_option_context_new(NULL);
	GOptionEntry entries[] = {
		{ "debug", 'd', 0, G_OPTION_ARG_INT, &opt_debug,
		  "Enable debug", NULL },

		{ "af-unix", 0, 0, G_OPTION_ARG_INT, &opt_af_unix,
		  "Enable AF_UNIX socket", NULL },
		{ "af-unix-path", 0, 0, G_OPTION_ARG_STRING,
		  &opt_af_unix_path,
		  "AF_UNIX socket pathname", NULL },

		{ "tcp", 0, 0, G_OPTION_ARG_INT, &opt_tcp,
		  "Enable TCP socket", NULL },
		{ "tcp-src-addr", 0, 0, G_OPTION_ARG_STRING, &opt_tcp_src_addr,
		  "TCP source IPv4 address", NULL },
		{ "tcp-src-port", 0, 0, G_OPTION_ARG_INT, &opt_tcp_src_port,
		  "TCP source port", NULL },

		{ "slip", 0, 0, G_OPTION_ARG_INT, &opt_slip,
		  "Enable SLIP protocol module", NULL },

		{ "trace", 0, 0, G_OPTION_ARG_INT, &opt_trace,
		  "Enable trace protocol module", NULL },
		{ "trace-addr", 0, 0, G_OPTION_ARG_STRING, &opt_trace_addr,
		  "Trace IPv4 address", NULL },
		{ "trace-port", 0, 0, G_OPTION_ARG_INT, &opt_trace_port,
		  "Trace UDP port", NULL },

		{ "udp", 0, 0, G_OPTION_ARG_INT, &opt_slip,
		  "Enable UDP socket", NULL },
		{ "udp-src-addr", 0, 0, G_OPTION_ARG_STRING, &opt_udp_src_addr,
		  "UDP source IPv4 address", NULL },
		{ "udp-src-port", 0, 0, G_OPTION_ARG_INT, &opt_udp_src_port,
		  "UDP source port", NULL },
		{ "udp-dst-addr", 0, 0, G_OPTION_ARG_STRING, &opt_udp_dst_addr,
		  "UDP destination IPv4 address", NULL },
		{ "udp-dst-port", 0, 0, G_OPTION_ARG_INT, &opt_udp_dst_port,
		  "UDP destination port", NULL },
		{ NULL }
	};

	g_option_context_add_main_entries(context, entries, NULL);

	if (!g_option_context_parse(context, argc, argv, &error))
		_E("%s", error->message);

	g_option_context_free(context);

	if (opt_tcp && opt_af_unix) {
		_E("TCP and AF_UNIX sockets are mutually exclusive");
	}
}

int main(int argc, char *argv[])
{
	S_QUEUE_INIT(&sl_queue);

	defaults_config();

	options_parse(&argc, &argv);

	configuration_print();

	sl_config();

	for (;;) {
		sl_data_flow(SL_OP_UP);

		sl_data_flow(SL_OP_DOWN);

		usleep(100/*us*/); /* TODO: switch to select() */
	}

	return 0;
}
