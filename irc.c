#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <penny/print.h>
#include <penny/penny.h>
#include <penny/mem.h>
#include <penny/sprint.h>

#include <ccan/container_of/container_of.h>
#include <ccan/net/net.h>
#include <ccan/err/err.h>
#include <ccan/compiler/compiler.h>
#include <ccan/str/str.h>
#include <ccan/array_size/array_size.h>
#include <ccan/list/list.h>
#include <ccan/pr_debug/pr_debug.h>

#include <ev.h>

/* types of warnings
 * - malformed protocol input
 * - buffer overflow outputs
 * - informational buffer & message printouts
 * - information about the parsed message
 */

#include "irc.h"
int irc_cmd(struct irc_connection *c,
		char const *msg, size_t msg_len)
{
	ssize_t r = write(c->w.fd, msg, msg_len);
	if ((size_t)r != msg_len)
		return (r < 0)?r:-1;

	if (debug_is(2)) {
		printf("< %zd ", r);
		print_bytes_as_cstring(msg, r, stdout);
		putchar('\n');
	}

	return 0;
}

int irc_cmd_fmt(struct irc_connection *c, char const *str, ...)
{
	char buf[1024];
	va_list va;
	va_start(va, str);
	ssize_t sz = vsnprintf(buf, sizeof(buf) - 2, str, va);
	if (sz < 0)
		return -1;
	if ((size_t)sz >= sizeof(buf) - 2) {
		pr_debug(1, "oversized irc_cmd_fmt, dropping.");
		return -1;
	}
	va_end(va);
	buf[sz++] = '\r';
	buf[sz++] = '\n';

	return irc_cmd(c, buf, sz);
}

int irc_cmd_privmsg_va(struct irc_connection *c,
		char const *dest, size_t dest_len,
		char const *msg_fmt, va_list va)
{
	char buf[1024];
	size_t sz = snprintf(buf, sizeof(buf), "PRIVMSG %.*s :", (int)dest_len, dest);
	if (sz >= sizeof(buf) - 2)
		goto overflow;

	sz += vsnprintf(buf + sz, sizeof(buf) - sz, msg_fmt, va);
	if (sz >= sizeof(buf) - 2)
		goto overflow;

	buf[sz++] = '\r';
	buf[sz++] = '\n';

	return irc_cmd(c, buf, sz);
overflow:
	pr_debug(1, "oversized irc_cmd_privmsg_fmt, dropping.");
	return -1;
}

int irc_cmd_privmsg_fmt(struct irc_connection *c,
		char const *dest, size_t dest_len,
		char const *msg_fmt, ...)
{
	va_list va;
	va_start(va, msg_fmt);
	int r = irc_cmd_privmsg_va(c, dest, dest_len, msg_fmt, va);
	va_end(va);
	return r;
}

#define PARSE_ARG_BODY do {						\
	if (*start == ':') {						\
		args[arg_pos].data = start + 1;				\
		args[arg_pos].len  = len - 1;				\
		return arg_pos + 1;					\
	}								\
									\
	char *end_of_curr = memchr(start + 1, ' ', len - 1);		\
	if (!end_of_curr) {						\
		args[arg_pos].data = start;				\
		args[arg_pos].len  = len;				\
		return arg_pos + 1;					\
	}								\
									\
	size_t len_of_curr = end_of_curr - start;			\
									\
	args[arg_pos].data = start;					\
	args[arg_pos].len  = len_of_curr;				\
									\
	char *start_of_next =						\
		memnchr(end_of_curr + 1, ' ', len - len_of_curr - 1);	\
	len -= start_of_next - start;					\
	start = start_of_next;						\
	arg_pos++;							\
} while(0)

int irc_parse_last_args(char const *start, size_t len, struct arg *args,
		size_t max_args)
{
	size_t arg_pos = 0;
	while (start && len) {
		if (arg_pos >= max_args) {
			/* discard the first argument and keep going */
			size_t cpy_size = sizeof(*args) * (max_args - 1);
			memmove(args, args + 1, cpy_size);
			arg_pos--;
		}

		PARSE_ARG_BODY;
	}

	if (len == 0)
		return arg_pos;

	return -1;
}

int irc_parse_args(char const *start, size_t len, struct arg *args,
		size_t max_args)
{
	size_t arg_pos = 0;
	while (start && len && arg_pos < max_args) {
		PARSE_ARG_BODY;
	}

	if (len == 0)
		return arg_pos;

	return -1;
}

static char *irc_parse_prefix(char *start, size_t len, char **prefix, size_t *prefix_len)
{
	if (len <= 0 || *start != ':') {
		*prefix = NULL;
		*prefix_len = 0;
		return start;
	}

	*prefix = start + 1;

	/* the pkt starts with a nick or server name */
	char *next = memchr(start + 1, ' ', len - 1);
	if (!next) {
		pr_debug(0, "invalid packet: couldn't locate a space after the first ':name'");
		return NULL;
	}

	*prefix_len = next - *prefix;

	/* point to the thing after the space */
	/* next + 1 :: skip the space we found with memchr */
	return memnchr(next + 1, ' ', len - (next + 1 - start));
}

#if 0
void irc_address_parts(const char *addr, size_t addr_len,
		const char **nick, size_t *nick_len,
		const char **user, size_t *user_len,
		const char **host, size_t *host_len)
{
	const char *maybe_nick_end = memchr(addr, '!', addr_len);
	if (maybe_nick_end) {
		*nick = addr;
		*nick_len = maybe_nick_end - addr;
	} else {
		*nick = NULL;
		*nick_len = 0;
	}

	/* FIXME: unfinished */
}
#endif

bool irc_user_is_me(struct irc_connection *c, const char *start, size_t len)

{
	return memeq(c->user, strlen(c->user), start, len);
}

int irc_set_channel_user_mode(struct irc_connection *c,
		const char *channel, size_t channel_len,
		const char *name, size_t name_len,
		enum irc_channel_user_mode mode)
{
	if (!(mode & IRC_CUM_o))
		return 0;
	irc_cmd_fmt(c, "MODE %.*s +o %.*s", (int)channel_len, channel,
			(int)name_len, name);
	return 0;
}

int irc_clear_channel_user_mode(struct irc_connection *c,
		const char *channel, size_t channel_len,
		const char *name, size_t name_len,
		enum irc_channel_user_mode mode)
{
	if (!(mode & IRC_CUM_o))
		return 0;
	irc_cmd_fmt(c, "MODE %.*s -o %.*s", (int)channel_len, channel,
			(int)name_len, name);
	return 0;
}

int irc_cmd_invite(struct irc_connection *c,
		char const *nick, size_t nick_len,
		char const *chan, size_t chan_len)
{
	irc_cmd_fmt(c, "INVITE %.*s %.*s", (int)nick_len, nick, (int)chan_len, chan);
	return 0;
}

static uint32_t op_hash_num(unsigned num)
{
	return tommy_hash_u32(IRC_OP_NUM, &num, sizeof(num));
}

static uint32_t op_hash_str(const char *str, size_t str_len)
{
	return tommy_hash_u32(IRC_OP_STR, str, str_len);
}

static uint32_t op_hash(struct irc_operation *op)
{
	if (op->type == IRC_OP_NUM)
		return op_hash_num(op->num);
	else
		return op_hash_str(op->str, op->str_len);
}

void irc_add_operation(struct irc_connection *c, struct irc_operation *op)
{
	tommy_hashlin_insert(&c->operations, &op->node, op, op_hash(op));
}

int irc_create_operation_num(struct irc_connection *c,
		unsigned num, irc_op_cb cb)
{
	struct irc_operation *op = malloc(sizeof(*op));
	if (!op)
		return -1;
	*op = (struct irc_operation) {
		.type = IRC_OP_NUM,
		.num  = num,
		.cb = cb,
	};
	irc_add_operation(c, op);
	return 0;
}

int irc_create_operation_str_(struct irc_connection *c,
		const char *str, size_t str_len, irc_op_cb cb)
{
	struct irc_operation *op = malloc(sizeof(*op));
	if (!op)
		return -1;
	*op = (struct irc_operation) {
		.type = IRC_OP_STR,
		.str = str,
		.str_len = str_len,
		.cb = cb,
	};
	irc_add_operation(c, op);
	return 0;
}

/* encode state suitable for passing via an argument to a program */
size_t irc_dump_state(struct irc_connection *c, char *buf, size_t len)
{
#define R (used > len ? used - len : 0)
	size_t used = 0;
	used += snprintf(&buf[used], R, "{.fd=%d,.server=", c->w.fd);
	used += sprint_cstring(&buf[used], R, c->server);
	used += snprintf(&buf[used], R, ",.port=");
	used += sprint_cstring(&buf[used], R, c->port);
	used += snprintf(&buf[used], R, ".buffer=");
	used += sprint_bytes_as_cstring(&buf[used], R, c->in_buf, c->in_pos);
	used += snprintf(&buf[used], R, "}");
	return used;
#undef R
}

int irc_load_state(struct irc_connection *c, const char *buf, size_t len)
{
	return -1;
}

static int compare_arg_to_op_str(const void *arg_, const void *op_)
{
	const struct arg *arg = arg_;
	const struct irc_operation *op = op_;

	if (op->type != IRC_OP_STR)
		return 1;
	return !memeq(op->str, op->str_len, arg->data, arg->len);
}

static int compare_num_to_op_num(const void *num_, const void *op_)
{
	unsigned num = (uintptr_t)num_;
	const struct irc_operation *op = op_;

	if (op->type != IRC_OP_NUM)
		return 1;
	return op->num != num;
}

static int process_pkt(struct irc_connection *c, char *start, size_t len)
{
	if (!len)
		return -EMSGSIZE;

	char *prefix;
	size_t prefix_len;
	char *command = irc_parse_prefix(start, len, &prefix, &prefix_len);

	if (!command)
		return -EINVAL;

	char *remain = memchr(command + 1, ' ', len - (command + 1 - start));
	size_t command_len = remain - command;
	/* skip duplicate spaces */
	remain = memnchr(remain + 1, ' ', len - (remain + 1 - start));
	size_t remain_len = len - (remain - start);

	pr_debug(1, "prefix=\"%.*s\", command=\"%.*s\", remain=\"%.*s\"",
			(int)prefix_len, prefix,
			(int)command_len, command,
			(int)remain_len, remain);
	if (command_len == 3 && isdigit(*command)
				&& isdigit(*(command+1))
				&& isdigit(*(command+2))) {
		unsigned cmd_val = (*(command) - '0') * 100
			+ (*(command + 1) - '0') * 10
			+ (*(command + 2) - '0');

		struct irc_operation *op = tommy_hashlin_search(&c->operations,
				compare_num_to_op_num,
				(void *)(uintptr_t)cmd_val, op_hash_num(cmd_val));
		if (op)
			return op->cb(c, op, prefix, prefix_len, remain, remain_len);
	}

	/* otherwise, it must be a string command */
	struct arg s = {
		.data = command,
		.len  = command_len,
	};
	struct irc_operation *op = tommy_hashlin_search(&c->operations,
					compare_arg_to_op_str, &s,
					op_hash_str(command, command_len));
	if (op)
		return op->cb(c, op, prefix, prefix_len, remain, remain_len);

	warnx("unknown command: %.*s", (int)command_len,
			command);
	return -EINVAL;
}

/* general fmt of messages */
/* :server_from number_status yournick :junk */
static void conn_cb(EV_P_ ev_io *w, int revents)
{
	struct irc_connection *c = container_of(w, typeof(*c), w);

	ssize_t max_read = sizeof(c->in_buf) - c->in_pos;
	char *c_buf = c->in_buf + c->in_pos;

	if (max_read <= 0)
		warnx("buffer overflow, discarding.");

	ssize_t r = read(w->fd, c_buf, max_read);
	if (r == 0) {
		fputs("server closed link, exiting...\n", stdout);
		ev_io_stop(EV_A_ w);
		ev_break(EV_A_ EVBREAK_ALL);
		return;
	}

	if (r == -1) {
		warn("failed to read");
		return;
	}

	c->in_pos += r;

	if (debug_is(4)) {
		printf("R %zd ", c->in_pos);
		print_bytes_as_cstring(c->in_buf, c->in_pos, stdout);
		putchar('\n');
		printf("B %zd ", c->in_pos);
		print_bytes_as_cstring(c->in_buf, c->in_pos, stdout);
		putchar('\n');
	}

	char *end   = memmem(c_buf, r, "\r\n", 2);
	char *start = c->in_buf;
	size_t buf_len = c->in_pos;
	for (;;) {
		if (!end)
			break;

		size_t len = end - start;

		r = process_pkt(c, start, len);
		if (r) {
			printf("> %zd ", len);
			print_bytes_as_cstring(start, len, stdout);
			putchar('\n');
		}

		start = end + 2;
		buf_len -= len + 2;
		end = memmem(start, buf_len, "\r\n", 2);
	}

	memmove(c->in_buf, start, buf_len);
	c->in_pos = buf_len;

	if (debug_is(4)) {
		printf("B %zd ", c->in_pos);
		print_bytes_as_cstring(c->in_buf, c->in_pos, stdout);
		putchar('\n');
	}
}

int irc_cmd_join(struct irc_connection *c,
		char const *name, size_t name_len)
{
	return irc_cmd_fmt(c, "JOIN %.*s", (int)name_len, name);
}

bool irc_is_connected(struct irc_connection *c)
{
	return ev_is_active(&c->w);
}

static void irc_proto_connect(struct irc_connection *c)
{
	if (c->pass)
		irc_cmd_fmt(c, "PASS %s", c->pass);
	irc_cmd_fmt(c, "NICK %s", c->nick);
	irc_cmd_fmt(c, "USER %s hostname servername :%s",
			c->user, c->realname);
}

/*
 * return:
 *	a non-negative file descriptor on success
 *	-1 if dns resolution failed
 *	-2 if connection failed
 */
static int irc_net_connect(struct irc_connection *c)
{
	/* TODO: use cached dns info? */
	struct addrinfo *res = net_client_lookup(c->server, c->port,
			AF_INET, SOCK_STREAM);
	if (!res)
		return -1;

	int fd = net_connect(res);
	if (fd == -1)
		return -2;

	freeaddrinfo(res);

	return fd;
}

static void irc_ev_init(struct irc_connection *c, int fd)
{
	/* FIXME: avoid depending on libev */
	ev_io_init(&c->w, conn_cb, fd, EV_READ);
	ev_io_start(EV_DEFAULT_ &c->w);
}

void irc_init(struct irc_connection *c)
{
	tommy_hashlin_init(&c->operations);
}

void irc_connect_fd(struct irc_connection *c, int fd)
{
	irc_ev_init(c, fd);
	irc_proto_connect(c);
}

int irc_connect(struct irc_connection *c)
{
	int fd = irc_net_connect(c);
	if (fd < 0)
		return fd;
	irc_connect_fd(c, fd);
	return fd;
}
