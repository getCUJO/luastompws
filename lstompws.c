/* 
 * Copyright (c) 2018 - 2019, CUJO LLC.
 * 
 * Licensed under the MIT license:
 * 
 *     http://www.opensource.org/licenses/mit-license.php
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdbool.h>
#include <assert.h>
#include <stdio.h>

#include <lua.h>
#include <lauxlib.h>

#include <libwebsockets.h>
#include <stomp.h>

#define LUASTOMPWS_WSPROT_STOMP	"v11.stomp"
#define LUASTOMPWS_METATAB_CONN	"libwebsocket_context*"

typedef struct stompws_Connection {
	lua_State *L;

	struct lws *ws_socket;
	struct lws_context *ws_context;

	/* Used by 'libwebsocket' library until the context is destroyed. */
	/* See 'lws_context_destroy'. */
	struct lws_protocols ws_protocols[2];

	void *ws_recvbuffer;
	size_t ws_recvbufsz;

	stomp_session_t *stomp_session;
} stompws_Connection;

static stompws_Connection*
tolstompws(lua_State *L)
{
	return luaL_checkudata(L, 1, LUASTOMPWS_METATAB_CONN);
}

static void
pushstompheaders(lua_State *L, size_t count, const struct stomp_hdr *headers)
{
	lua_createtable(L, 0, count);
	for (size_t i = 0; i < count; ++i) {
		lua_pushstring(L, headers[i].val);
		lua_setfield(L, -2, headers[i].key);
	}
}

/*
** Message handler used to run all chunks
*/
static int
msghandler(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);

	if (msg == NULL) {  /* is error object not a string? */
		/* does it have a metamethod AND does it produce a string? */
		if (luaL_callmeta(L, 1, "__tostring") &&
		    lua_type(L, -1) == LUA_TSTRING)
			return 1;  /* that is the message */
		else
			msg = lua_pushfstring(L, "(error object is a %s value)",
			                         luaL_typename(L, 1));
	}
	luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
	return 1;  /* return the traceback */
}

static int
stomp_reportcallback(lua_State *L, const char *name, int status)
{
	if (status != LUA_OK) {
		const char *errmsg = lua_tostring(L, -1);
		fprintf(stderr, "STOMP %s callback error: %s", name, errmsg);
		lua_pop(L, 1);  /* remove message */
	}
	return status;
}

#define STOMP_CALLBACK(FUNC, NAME, BODY)                                       \
static void                                                                    \
FUNC(stomp_session_t *session, void *msg_ptr, void *conn_ptr)                  \
{                                                                              \
	struct stomp_ctx_message *msg = msg_ptr;                               \
	stompws_Connection *conn = conn_ptr;                                   \
	lua_State *L = conn->L;                                                \
	lua_pushcfunction(L, msghandler);   /* push error handler */           \
	lua_getuservalue(L, 1);   /* push callback function */                 \
	lua_pushliteral(L, NAME);                                              \
	lua_pushvalue(L, 1);   /* push the connection */                       \
	pushstompheaders(L, msg->hdrc, msg->hdrs);                             \
	if (BODY) lua_pushlstring(L, msg->body, msg->body_len);                \
	else      lua_pushnil(L);                                              \
	stomp_reportcallback(L, NAME, lua_pcall(L, 4, 0, -6));                 \
	lua_pop(L, 1);                                                         \
}

STOMP_CALLBACK(stomp_connectedcallback, "connected", false)
STOMP_CALLBACK(stomp_receiptcallback  , "receipt"  , false)
STOMP_CALLBACK(stomp_errorcallback    , "error"    , true)
STOMP_CALLBACK(stomp_messagecallback  , "message"  , true)

#undef STOMP_CALLBACK

static int
send_handler(lua_State *L, stompws_Connection *conn, int nresults)
{
	if (nresults < 4) {
		fprintf(stderr, "STOMP callback wrong number of values "
				"for send command\n");
		return -1;
	}
	if (!lua_isstring(L, -nresults + 1)) {
		fprintf(stderr, "STOMP callback expected string as "
				"first value\n");
		return -1;
	}
	if (!lua_isstring(L, -nresults + 2)) {
		fprintf(stderr, "STOMP callback expected string as "
				"second value\n");
		return -1;
	}

	struct stomp_hdr header = {
		"destination", lua_tostring(L, -nresults + 1)
	};

	size_t bodysz;
	const char *body = lua_tolstring(L, -nresults + 2, &bodysz);

	/* stomp bug:
	 * this might fail silently if the socket buffer is full, and it will
	 * leave a partial frame written in the output stream, which might be
	 * unrecoverable and will require the application to close and reopen
	 * the STOMP connection in order to be able to send more messages.
	 */
	return stomp_send(conn->stomp_session, 1, &header, body, bodysz);
}

static int
heartbeat_handler(lua_State *L, stompws_Connection *conn, int nresults)
{
	return stomp_send_heartbeat(conn->stomp_session);
}

static int
callback_handler(const char *cmd, lua_State *L,
                 stompws_Connection *conn, int nresults)
{
	struct entry {
		const char *cmd;
		int (*handler)(lua_State *, stompws_Connection *, int);
	};

	const struct entry handlers[] = {
		{ "send"     , send_handler },
		{ "heartbeat", heartbeat_handler },
	};
	int size = sizeof(handlers) / sizeof(*handlers);

	for (const struct entry *i = handlers; i < handlers + size; ++i) {
		if (!strcmp(cmd, i->cmd)) {
			return i->handler(L, conn, nresults);
		}
	}

	fprintf(stderr, "STOMP callback invalid command \"%s\"\n", cmd);
	return -1;
}

static bool
send_callback(lua_State *L, stompws_Connection *conn, int nresults)
{
	if (nresults < 2) {
		if (nresults == 1) {
			fprintf(stderr, "STOMP callback returned wrong number "
			                "of values: %d\n", nresults);
		}
		return false;
	}
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "STOMP callback expected function as last "
		                "return value\n");
		return false;
	}
	if (!lua_isstring(L, -nresults)) {
		fprintf(stderr, "STOMP callback expected string as "
		                "first value\n");
		return false;
	}

	const char *cmd = lua_tostring(L, -nresults);
	int res = callback_handler(cmd, L, conn, nresults);

	lua_pushcfunction(L, msghandler);
	lua_pushvalue(L, -2); // -1 - 1 (because we pushed msghandler)
	lua_pushinteger(L, res);
	int status = lua_pcall(L, 1, 0, -3);
	if (status != LUA_OK) {
		const char *errmsg = lua_tostring(L, -1);
		fprintf(stderr, "STOMP complete callback error: %s\n", errmsg);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	return res < 0;
}

static void
failure_callback(lua_State *L, const char *msg)
{
	lua_pushcfunction(L, msghandler);
	lua_getuservalue(L, 1);   /* push callback function */
	lua_pushliteral(L, "failure");
	lua_pushvalue(L, 1);   /* push the connection */
	lua_pushstring(L, msg);
	stomp_reportcallback(L, "failure", lua_pcall(L, 3, 0, 2));
	lua_pop(L, 1);
}

static void
established_handler(lua_State *L, stompws_Connection *conn)
{
	struct stomp_hdr headers[2] = {
		{ "accept-version", "1.1" },
		{ "heart-beat", "10000,10000" },
	};
	int err = stomp_connect(conn->stomp_session, conn->ws_socket,
	                        2, headers);
	if (err) {
		failure_callback(L, "connection failure");
	}
}

static void
close_handler(lua_State *L, stompws_Connection *conn, bool error)
{
	stomp_session_free(conn->stomp_session);
	conn->stomp_session = NULL;
	conn->ws_socket = NULL;

	lua_pushcfunction(L, msghandler);
	lua_getuservalue(L, 1);
	lua_pushliteral(L, "closed");
	lua_pushvalue(L, 1);
	if (error) {
		lua_pushstring(L, "connection error");
	} else {
		lua_pushnil(L);
	}
	stomp_reportcallback(L, "closed", lua_pcall(L, 3, 0, 2));
	lua_pop(L, 1);
}

static void
receive_handler(lua_State *L, stompws_Connection *conn, void *in, size_t len)
{
	lua_pushcfunction(L, msghandler);
	lua_getuservalue(L, 1);
	lua_pushliteral(L, "receive");
	lua_pushvalue(L, 1);
	stomp_reportcallback(L, "receive", lua_pcall(L, 2, 0, 2));
	lua_pop(L, 1);

	if (len > (SIZE_MAX - conn->ws_recvbufsz)) {
		len = SIZE_MAX - conn->ws_recvbufsz;
	}
	void *buffer = realloc(conn->ws_recvbuffer, conn->ws_recvbufsz + len);
	if (!buffer) {
		failure_callback(L, "no memory for data");
		return;
	}

	memcpy(buffer + conn->ws_recvbufsz, in, len);
	conn->ws_recvbuffer = buffer;
	conn->ws_recvbufsz += len;

	if (lws_remaining_packet_payload(conn->ws_socket) == 0 &&
	    lws_is_final_fragment(conn->ws_socket) != 0) {
		stomp_recv_cmd(conn->stomp_session,
			       conn->ws_recvbuffer, conn->ws_recvbufsz);
		free(conn->ws_recvbuffer);
		conn->ws_recvbuffer = NULL;
		conn->ws_recvbufsz = 0;
	}
}

static int
writable_handler(lua_State *L, stompws_Connection *conn)
{
	lua_pushcfunction(L, msghandler); /* error handler */

	bool close = false;
	int level = lua_gettop(L);

	lua_getuservalue(L, 1); /* callback function */
	lua_pushliteral(L, "send");
	lua_pushvalue(L, 1); /* the connection */
	int status = lua_pcall(L, 2, LUA_MULTRET, 2);
	if (status == LUA_OK) {
		int nresults = lua_gettop(L) - level;
		close = send_callback(L, conn, nresults);
		lua_pop(L, nresults);
	} else {
		const char *errmsg = lua_tostring(L, -1);
		fprintf(stderr, "STOMP send callback error: %s\n", errmsg);
		lua_pop(L, 1);  /* remove message */
	}
	lua_pop(L, 1);
	return close ? -1 : 0;
}

static int
pollfd_handler(lua_State *L, stompws_Connection *conn,
               enum lws_callback_reasons reason,
               const struct lws_pollargs *arg)
{
	lua_pushcfunction(L, msghandler); /* error handler */
	lua_getuservalue(L, 1);   /* push callback function */

	const int level = lua_gettop(L);
	switch (reason) {
	case LWS_CALLBACK_ADD_POLL_FD:
		lua_pushliteral(L, "add_fd");
		lua_pushvalue(L, 1);   /* push the connection */
		lua_pushinteger(L, arg->fd);
		lua_pushboolean(L, arg->events & LWS_POLLIN);
		lua_pushboolean(L, arg->events & LWS_POLLOUT);
		break;
	case LWS_CALLBACK_DEL_POLL_FD:
		lua_pushliteral(L, "del_fd");
		lua_pushvalue(L, 1);   /* push the connection */
		lua_pushinteger(L, arg->fd);
		break;
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
		lua_pushliteral(L, "change_mode_fd");
		lua_pushvalue(L, 1);   /* push the connection */
		lua_pushinteger(L, arg->fd);

		lua_pushboolean(L, arg->events & LWS_POLLIN);
		lua_pushboolean(L, arg->events & LWS_POLLOUT);

		lua_pushboolean(L, arg->prev_events & LWS_POLLIN);
		lua_pushboolean(L, arg->prev_events & LWS_POLLOUT);
		break;
	}

	const int nargs = lua_gettop(L) - level;
	stomp_reportcallback(L, "pollfd", lua_pcall(L, nargs, 0, 2));
	lua_pop(L, 1);
}

static int
websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                   void *user, void *in, size_t len)
{
	struct lws_context *ctx = lws_get_context(wsi);
	assert(ctx);
	stompws_Connection *conn = lws_context_user(ctx);
	assert(conn);

	lua_State *L = conn->L;
	assert(L);
	assert(luaL_checkudata(L, 1, LUASTOMPWS_METATAB_CONN));

	switch (reason) {
		case LWS_CALLBACK_CLIENT_ESTABLISHED:
			established_handler(L, conn);
			break;
		case LWS_CALLBACK_CLOSED:
		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
			close_handler(L, conn, reason != LWS_CALLBACK_CLOSED);
			break;
		case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
			fprintf(stderr, "websocket_callback peer closed\n");
			break;
		case LWS_CALLBACK_CLIENT_RECEIVE:
			receive_handler(L, conn, in, len);
			break;
		case LWS_CALLBACK_CLIENT_WRITEABLE:
			return writable_handler(L, conn);
		case LWS_CALLBACK_ADD_POLL_FD:
		case LWS_CALLBACK_DEL_POLL_FD:
		case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
			pollfd_handler(L, conn, reason,
			               (const struct lws_pollargs *)in);
			break;
		case LWS_CALLBACK_PROTOCOL_INIT:
		case LWS_CALLBACK_PROTOCOL_DESTROY:
		case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
		case LWS_CALLBACK_GET_THREAD_ID:
		case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
		case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
		case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
		case LWS_CALLBACK_WSI_CREATE:
		case LWS_CALLBACK_WSI_DESTROY:
		case LWS_CALLBACK_LOCK_POLL:
		case LWS_CALLBACK_UNLOCK_POLL:
			break;
		default:
			fprintf(stderr, "websocket_callback unhandled "
			                "socket:%p reason:%d user:%p in:%p "
			                "len:%llu\n",
			        wsi, reason, user, in, len);
			break;
	}
	return 0;
}

typedef struct stomp_CallbackInfo {
	const char selector;
	enum stomp_cb_type type;
	stomp_cb_t callback;
} stomp_CallbackInfo;

stomp_CallbackInfo stomp_SupportedCallbacks[] = {
	{ 'c', SCB_CONNECTED, stomp_connectedcallback },
	{ 'e', SCB_ERROR    , stomp_errorcallback },
	{ 'm', SCB_MESSAGE  , stomp_messagecallback },
	{ 'r', SCB_RECEIPT  , stomp_receiptcallback },
	{ 0  , 0            , NULL }
};

/*
 * connection = stompws.connect(usessl,
 *                              address,
 *                              port,
 *                              path,
 *                              hostname,
 *                              origin,
 *                              callback, -- function (kind, ...) end
 *                              cbflags, -- "cemr"
 *                              [capath])
 */
static int
stompws_connect(lua_State *L)
{
	stomp_CallbackInfo *cbinfo;
	int usessl = lua_toboolean(L, 1);
	const char *address = luaL_checkstring(L, 2);
	lua_Integer port = luaL_checkinteger(L, 3);
	const char *path = luaL_checkstring(L, 4);
	const char *hostname = luaL_checkstring(L, 5);
	const char *origin = luaL_checkstring(L, 6);
	const char *cbflags = luaL_checkstring(L, 8);
	const char *capath = luaL_optstring(L, 9, NULL);

	luaL_argcheck(L, lua_isfunction(L, 7), 7, "callback function expected");
	luaL_argcheck(L, port >= 0 && port < 65536, 3, "invalid port number");

	struct lws_context_creation_info ws_args;

	stompws_Connection *conn =
		lua_newuserdata(L, sizeof(stompws_Connection));
	memset(conn, 0, sizeof(stompws_Connection));
	lua_pushvalue(L, 7);  /* push callback function */
	lua_setuservalue(L, -2);  /* associate it to the conn. userdata */

	luaL_getmetatable(L, LUASTOMPWS_METATAB_CONN);
	lua_setmetatable(L, -2);

	lua_replace(L, 1);  /* replaces calback function with conn. userdata */

	conn->L = L;

	conn->stomp_session = stomp_session_new(conn);
	if (conn->stomp_session == NULL)
		luaL_error(L, "unable to create STOMP session");

	for (cbinfo = stomp_SupportedCallbacks; cbinfo->callback; ++cbinfo)
		if (strchr(cbflags, cbinfo->selector) != NULL)
			stomp_callback_set(conn->stomp_session, cbinfo->type, cbinfo->callback);

	conn->ws_protocols[0].name = LUASTOMPWS_WSPROT_STOMP;
	conn->ws_protocols[0].callback = websocket_callback;
	conn->ws_protocols[0].rx_buffer_size = 65536;
	conn->ws_protocols[1].name = NULL;

	memset(&ws_args, 0, sizeof(struct lws_context_creation_info));
	ws_args.port = CONTEXT_PORT_NO_LISTEN;
	ws_args.protocols = conn->ws_protocols;
	ws_args.extensions = NULL;
	ws_args.uid = -1;
	ws_args.gid = -1;
	ws_args.ka_time = 5;
	ws_args.ka_probes = 3;
	ws_args.ka_interval = 10;
	ws_args.ssl_ca_filepath = capath;
	ws_args.user = conn;
	ws_args.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

	conn->ws_context = lws_create_context(&ws_args);
	if (conn->ws_context == NULL) {
		stomp_session_free(conn->stomp_session);
		luaL_error(L, "unable to create WebSocket context");
	}

	struct lws_client_connect_info info = {
		.context = conn->ws_context,
		.address = address,
		.port = (int)port,
		.ssl_connection = usessl ? (capath ? 1 : 2) : 0,
		.path = path,
		.host = hostname,
		.origin = origin,
		.protocol = LUASTOMPWS_WSPROT_STOMP,
		.ietf_version_or_minus_one = -1, /* default protocol */
		.userdata = NULL,
	};
	conn->ws_socket = lws_client_connect_via_info(&info);

	if (conn->ws_socket == NULL) {
		/* also frees 'stomp_session', see 'websocket_callback' */
		lws_context_destroy(conn->ws_context);
		assert(conn->stomp_session == NULL);
		luaL_error(L, "unable to connect");
	}

	conn->L = NULL;

	lua_pushvalue(L, 1);
	return 1;
}

static int
stompws_close(lua_State *L)
{
	stompws_Connection *conn = tolstompws(L);

	int active = (conn->ws_context != NULL);
	if (active) {
		/* also frees 'stomp_session', see 'websocket_callback' */
		conn->L = L;
		lws_context_destroy(conn->ws_context);
		conn->L = NULL;

		assert(conn->stomp_session == NULL);
		conn->ws_context = NULL;
	}
	lua_pushboolean(L, active);
	return 1;
}

static stompws_Connection*
tostompws(lua_State *L)
{
	stompws_Connection *conn = tolstompws(L);
	if (conn->ws_socket == NULL) {
		luaL_error(L, "attempt to use a closed STOMP Web Socket");
	}
	return conn;
}

/*
 * connection:subscribe(name, value)
 */
static int
stompws_subscribe(lua_State *L)
{
	stompws_Connection *conn = tostompws(L);
	struct stomp_hdr header;
	int res;
	header.key = luaL_checkstring(L, 2);
	header.val = luaL_checkstring(L, 3);
	res = stomp_subscribe(conn->stomp_session, 1, &header);
	if (res < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "failed");
		return 2;
	}
	lua_pushinteger(L, res);
	return 1;
}

/*
 * connection:unsubscribe(subscription, name, value)
 */
static int
stompws_unsubscribe(lua_State *L)
{
	stompws_Connection *conn = tostompws(L);
	struct stomp_hdr header;
	int res;
	int subscription = luaL_checkinteger(L, 2);
	header.key = luaL_checkstring(L, 3);
	header.val = luaL_checkstring(L, 4);
	res = stomp_unsubscribe(conn->stomp_session, subscription, 1, &header);
	if (res < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "failed");
		return 2;
	}
	lua_pushinteger(L, 1);
	return 1;
}

/*
 * connection:dispatch(fd, read, write)
 */
static int
stompws_dispatch(lua_State *L)
{
	stompws_Connection *conn = tolstompws(L);

	if (lua_gettop(L) == 1) {
		conn->L = L;
		lws_service_fd(conn->ws_context, NULL);
		conn->L = NULL;
		return 0;
	}

	lua_Integer fd = luaL_checkinteger(L, 2);
	bool read = lua_toboolean(L, 3);
	bool write = lua_toboolean(L, 4);

	/* prepare stack for eventual callback */
	lua_settop(L, 1);

	int events = (read ? LWS_POLLIN : 0) | (write ? LWS_POLLOUT : 0);
	struct lws_pollfd fdp = { fd, events, events };

	conn->L = L;
	int res = lws_service_fd(conn->ws_context, &fdp);
	conn->L = NULL;

	if (res != 0) {
		lua_pushnil(L);
		lua_pushstring(L, res == 1 ? "closed" : "failed");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

/*
 * connection:send()
 */
static int
stompws_send(lua_State *L)
{
	stompws_Connection *conn = tostompws(L);

	/* we need to save old conn->L here and restore later,
	 * because this might reenter
	 */
	lua_State *L_backup = conn->L;
	conn->L = L;
	lua_pushboolean(L, lws_callback_on_writable(conn->ws_socket));
	conn->L = L_backup;

	return 1;
}

/*
 * connection:get_heartbeat()
 */
static int
stompws_get_heartbeat(lua_State *L)
{
	stompws_Connection *conn = tostompws(L);

	if (conn->stomp_session == NULL) {
		return 0;
	}

	lua_pushinteger(L, stomp_get_client_hb(conn->stomp_session));
	lua_pushinteger(L, stomp_get_broker_hb(conn->stomp_session));
	return 2;
}

static const luaL_Reg mth[] = {
	{"__gc", stompws_close},
	{"close", stompws_close},
	{"send", stompws_send},
	{"subscribe", stompws_subscribe},
	{"unsubscribe", stompws_unsubscribe},
	{"dispatch", stompws_dispatch},
	{"get_heartbeat", stompws_get_heartbeat},
	{NULL, NULL}
};

static const luaL_Reg lib[] = {
	{"connect", stompws_connect},
	{NULL,      NULL}
};

LUALIB_API int
luaopen_stompws(lua_State *L)
{
	/* create connection class */
	luaL_newmetatable(L, LUASTOMPWS_METATAB_CONN);
	lua_pushvalue(L, -1);  /* push metatable */
	lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
	luaL_setfuncs(L, mth, 0);  /* add methods to new metatable */
	lua_pop(L, 1);  /* remove new class */
	/* create library table */
	luaL_newlibtable(L, lib);
	luaL_setfuncs(L, lib, 0);
	return 1;
}
