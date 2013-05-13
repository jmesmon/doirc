#ifndef IRC_H_
#define IRC_H_

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <ev.h>

enum irc_num_cmds {
#define RPL(name, value) RPL_##name = value,
#include "irc_spec.h"
#undef RPL
};

static const char *irc_num_cmds[] = {
#define RPL(name, value) [value] = #name,
#include "irc_spec.h"
#undef RPL
};

enum irc_user_mode {
	IRC_UM_i = 1 << 0,
	IRC_UM_w = 1 << 1,
	IRC_UM_s = 1 << 2,
	IRC_UM_o = 1 << 3,
};

enum irc_channel_user_mode {
	IRC_CUM_v = 1 << 0,
	IRC_CUM_o = 1 << 1,
};

enum irc_channel_mode {
	IRC_CM_s = 1 << 0,
};

struct arg {
	const char *data;
	size_t len;
};

struct irc_connection;
struct irc_conn_cb {
	int (*privmsg)(struct irc_connection *c,
			char const *source, size_t source_len,
			char const *dest, size_t dest_len,
			char const *msg, size_t msg_len);

	int (*mode)(struct irc_connection *c,
			char const *source, size_t source_len,
			struct arg *args, int arg_ct);

	/* channel lifecycle */
	int (*join)(struct irc_connection *c,
			char const *ch, size_t ch_len);
	int (*part)(struct irc_connection *c,
			char const *ch, size_t ch_len);

	int (*ping)(struct irc_connection *c);

	/* connection lifecycle */
	int (*connect)(struct irc_connection *c);
	int (*disconnect)(struct irc_connection *c);

};

struct irc_connection {
	ev_io w;

	/* network connection */
	const char *server;
	const char *port;
	struct addrinfo *addr;

	/* irc proto connection */
	const char *nick;
	const char *realname;
	const char *user;
	const char *pass;

	struct irc_conn_cb cb;
#if 0
	/* state while connected */
	enum irc_user_mode user_mode;
	struct list_head channels;
#endif

	/* buffers */
	size_t in_pos;
	char in_buf[1024];
};

/* for use in callbacks */
int irc_cmd(struct irc_connection *c, char const *str, ...);
int irc_cmd_privmsg(struct irc_connection *c,
		char const *dest, size_t dest_len,
		char const *msg,  size_t msg_len);
#define irc_cmd_join_(c, n) irc_cmd_join(c, n, strlen(n))
int irc_cmd_join(struct irc_connection *c,
		char const *name, size_t name_len);
int irc_cmd_invite(struct irc_connection *c,
		char const *nick, size_t nick_len,
		char const *chan, size_t chan_len);

bool irc_user_is_me(struct irc_connection *c, const char *start, size_t len);

int irc_clear_channel_user_mode(struct irc_connection *c,
		const char *channel, size_t channel_len,
		const char *name, size_t name_len,
		enum irc_channel_user_mode mode);
int irc_set_channel_user_mode(struct irc_connection *c,
		const char *channel, size_t channel_len,
		const char *name, size_t name_len,
		enum irc_channel_user_mode mode);

/* manage a connection */
int irc_connect(struct irc_connection *c);
void irc_disconnect(struct irc_connection *c);
bool irc_is_connected(struct irc_connection *c);

#endif
