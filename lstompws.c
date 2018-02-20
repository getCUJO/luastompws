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

#include <stdio.h>

#include <lua.h>
#include <lauxlib.h>

#include <libwebsockets.h>
#include <stomp.h>

#define LUASTOMPWS_WSPROT_STOMP	"v11.stomp"
#define LUASTOMPWS_METATAB_CONN	"libwebsocket_context*"

typedef struct stompws_Connection {
	lua_State *L;

	struct libwebsocket *ws_socket;
	struct libwebsocket_context *ws_context;
	/* Array used by 'libwebsocket' library until the context is destroyed. */
	/* See 'libwebsocket_context_destroy'. */
	struct libwebsocket_protocols ws_protocols[2];
	int ws_connected;

	void *ws_recvbuffer;
	size_t ws_recvbufsz;

	stomp_session_t *stomp_session;
} stompws_Connection;

static void
lua_pushstompheaders(lua_State *L, size_t count, const struct stomp_hdr *headers)
{
	size_t i;

	lua_createtable(L, 0, count);
	for (i = 0; i < count; ++i) {
		lua_pushstring(L, headers[i].val);
		lua_setfield(L, -2, headers[i].key);
	}
}

/*
** Message handler used to run all chunks
*/
static int
msghandler (lua_State *L)
{
	const char *msg = lua_tostring(L, 1);

	if (msg == NULL) {  /* is error object not a string? */
		if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
		    lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
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

#define stomp_callback(name, pusharg3, pusharg4)                   \
	static void                                                      \
	stomp_##name##callback(stomp_session_t *session,                 \
	                      void *msg_pointer,                         \
	                      void *conn_pointer)                        \
	{                                                                \
		struct stomp_ctx_##name *msg = (struct stomp_ctx_##name *)msg_pointer; \
		stompws_Connection *conn = (stompws_Connection *)conn_pointer; \
		lua_State *L = conn->L;                                        \
		                                                               \
		assert(L != NULL);                                             \
		assert(luaL_checkudata(L, 1, LUASTOMPWS_METATAB_CONN));        \
		lua_pushcfunction(L, msghandler);   /* push error handler */   \
		lua_getuservalue(L, 1);   /* push callback function */         \
		lua_pushvalue(L, 1);   /* push the connection */               \
		lua_pushliteral(L, #name);                                     \
		pusharg3;                                                      \
		pusharg4;                                                      \
		stomp_reportcallback(L, #name, lua_pcall(L, 4, 0, -6));        \
		lua_pop(L, 1);                                                 \
	}

struct stomp_ctx_failure {
	const char *errmsg;
};
stomp_callback(failure, (lua_pushnil(L)),
                        (lua_pushstring(L, msg->errmsg)));
stomp_callback(user, (lua_pushnil(L)),
                     (lua_pushnil(L)));
stomp_callback(connected, (lua_pushstompheaders(L, msg->hdrc, msg->hdrs)),
                          (lua_pushnil(L)));
stomp_callback(receipt, (lua_pushstompheaders(L, msg->hdrc, msg->hdrs)),
                        (lua_pushnil(L)));
stomp_callback(error, (lua_pushstompheaders(L, msg->hdrc, msg->hdrs)),
                      (lua_pushlstring(L, msg->body, msg->body_len)));
stomp_callback(message, (lua_pushstompheaders(L, msg->hdrc, msg->hdrs)),
                        (lua_pushlstring(L, msg->body, msg->body_len)));

static int
websocket_callback(struct libwebsocket_context *context,
                   struct libwebsocket *socket,
                   enum libwebsocket_callback_reasons reason,
                   void *ignored,
                   void *dataread,
                   size_t datasize)
{
	stompws_Connection *conn =
		(stompws_Connection *)libwebsocket_context_user(context);

	switch (reason) {
		case LWS_CALLBACK_CLIENT_ESTABLISHED: {
			int err;
			struct stomp_hdr headers[2];
			headers[0].key = "accept-version";
			headers[0].val = "1.1";
			headers[1].key = "heart-beat";
			headers[1].val = "10000,10000";
			err = stomp_connect(conn->stomp_session, conn->ws_socket, 2, headers);
			if (err) {
				struct stomp_ctx_failure fail = { .errmsg = "connection failure" };
				stomp_failurecallback(conn->stomp_session, &fail, conn);
			}
		}	break;
		case LWS_CALLBACK_CLIENT_RECEIVE: {
			void *buffer;
			if (datasize > (SIZE_MAX - conn->ws_recvbufsz))
				datasize = SIZE_MAX - conn->ws_recvbufsz;
			buffer = realloc(conn->ws_recvbuffer, conn->ws_recvbufsz + datasize);
			if (buffer) {
				memcpy(buffer + conn->ws_recvbufsz, dataread, datasize);
				conn->ws_recvbuffer = buffer;
				conn->ws_recvbufsz += datasize;

				if (libwebsockets_remaining_packet_payload(conn->ws_socket) == 0 &&
				    libwebsocket_is_final_fragment(conn->ws_socket) != 0) {
					stomp_recv_cmd(conn->stomp_session,
					               conn->ws_recvbuffer,
					               conn->ws_recvbufsz);
					free(conn->ws_recvbuffer);
					conn->ws_recvbuffer = NULL;
					conn->ws_recvbufsz = 0;
				}
			} else {
				struct stomp_ctx_failure fail = { .errmsg = "no memory for data" };
				stomp_failurecallback(conn->stomp_session, &fail, conn);
			}
		} break;
		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
			struct stomp_ctx_failure fail = { .errmsg = "connection error" };
			stomp_failurecallback(conn->stomp_session, &fail, conn);
		}	break;
		case LWS_CALLBACK_PROTOCOL_DESTROY: {
			stomp_session_free(conn->stomp_session);
			conn->stomp_session = NULL;
		}	break;
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
	{ 'u', SCB_USER     , stomp_usercallback },
	{ 0  , 0            , NULL }
};

/*
 connection = stompws.connect(usessl,
                              address,
                              port,
                              path,
                              hostname,
                              origin,
                              callback, -- function (kind, headers, message) end
                              cbflags, -- "cemru"
                              [capath])
 */
static int
stompws_connect (lua_State *L)
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
	struct libwebsocket_protocols *ws_prot;
	struct libwebsocket_context **ws_ctxt;

	stompws_Connection *conn = lua_newuserdata(L, sizeof(stompws_Connection));
	memset(conn, 0, sizeof(stompws_Connection));
	lua_pushvalue(L, 7);  /* push callback function */
	lua_setuservalue(L, -2);  /* associate the callback to the conn. userdata */
	lua_replace(L, 1);  /* prepare stack for eventual callback */

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
	ws_args.extensions = libwebsocket_get_internal_extensions();
	ws_args.uid = -1;
	ws_args.gid = -1;
	ws_args.ka_time = 5;
	ws_args.ka_probes = 3;
	ws_args.ka_interval = 10;
	ws_args.ssl_ca_filepath = capath;
	ws_args.user = conn;

	conn->ws_context = libwebsocket_create_context(&ws_args);
	if (conn->ws_context == NULL) {
		stomp_session_free(conn->stomp_session);
		luaL_error(L, "unable to create WebSocket context");
	}

	conn->ws_socket = libwebsocket_client_connect(conn->ws_context, address,
		(int)port, usessl ? (capath ? 1 : 2) : 0, path, hostname, origin,
		LUASTOMPWS_WSPROT_STOMP, -1);
	if (conn->ws_socket == NULL) {
		/* also frees 'stomp_session', see 'websocket_callback' */
		libwebsocket_context_destroy(conn->ws_context);
		assert(conn->stomp_session == NULL);
		luaL_error(L, "unable to connect");
	}

	conn->L = NULL;

	lua_settop(L, 1);
	luaL_getmetatable(L, LUASTOMPWS_METATAB_CONN);
	lua_setmetatable(L, -2);
	return 1;
}

#define tolstompws(L)	((stompws_Connection *) \
	luaL_checkudata(L, 1, LUASTOMPWS_METATAB_CONN))

static int
stompws_close (lua_State *L)
{
	stompws_Connection *conn = tolstompws(L);

	int active = (conn->ws_context != NULL);
	if (active) {
		/* prepare stack for eventual callback */
		lua_settop(L, 1);
		conn->L = L;
		/* also frees 'stomp_session', see 'websocket_callback' */
		libwebsocket_context_destroy(conn->ws_context);
		assert(conn->stomp_session == NULL);
		conn->ws_context = NULL;
		conn->L = NULL;
	}
	lua_pushboolean(L, active);
	return 1;
}

static stompws_Connection *tostompws (lua_State *L) {
	stompws_Connection *conn = tolstompws(L);

	if (conn->ws_context == NULL)
		luaL_error(L, "attempt to use a closed STOMP Web Socket");
	return conn;
}

/* connection:subscribe(name, value) */
static int
stompws_subscribe (lua_State *L)
{
	stompws_Connection *conn = tolstompws(L);

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

/* connection:unsubscribe(subscription, name, value) */
static int
stompws_unsubscribe (lua_State *L)
{
	stompws_Connection *conn = tolstompws(L);

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

/* connection:dispatch([timeout]) */
static int
stompws_dispatch (lua_State *L)
{
	int res;
	stompws_Connection *conn = tolstompws(L);
	lua_Integer timeout = luaL_checkinteger(L, 2);

	/* prepare stack for eventual callback */
	lua_settop(L, 1);
	conn->L = L;
	/* shouldn't this be inside 'websocket_callback' */
	stomp_handle_heartbeat(conn->stomp_session);
	res = libwebsocket_service(conn->ws_context, timeout);
	conn->L = NULL;
	if (res != 0) {
		lua_pushnil(L);
		lua_pushstring(L, res == 1 ? "closed" : "failed");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

#define USUAL_HEADER_COUNT	1

/* connection:send(headers, body) */
static int stompws_send (lua_State *L)
{
	stompws_Connection *conn = tolstompws(L);
	size_t bodysz;
	const char *body = luaL_checklstring(L, 3, &bodysz);

	struct stomp_hdr usualhdrs[USUAL_HEADER_COUNT];
	struct stomp_hdr *headers = usualhdrs;
	size_t i;
	int res;
	luaL_argcheck(L, 2, lua_istable(L, 2), "table expected");

	i = 0;
	lua_pushnil(L);  /* first key */
	while (lua_next(L, 2) != 0) {
		luaL_argcheck(L, 2, lua_isstring(L, -1) && lua_isstring(L, -2),
			"must contain only strings");
		lua_pop(L, 1); /* removes 'value'; keeps 'key' for next iteration */
		++i;
	}

	if (i > USUAL_HEADER_COUNT) {
		headers = (struct stomp_hdr *)calloc(i, sizeof(struct stomp_hdr));
		if (headers == NULL) luaL_error(L, "no memory");
	}

	i = 0;
	lua_pushnil(L);  /* first key */
	while (lua_next(L, 2) != 0) {
		headers[i].key = lua_tostring(L, -2);
		headers[i].val = lua_tostring(L, -1);
		lua_pop(L, 1); /* removes 'value'; keeps 'key' for next iteration */
		++i;
	}

	/*
	 * libstomp bug: this call might fail silently if the socket buffer is full,
	 *               and it will leave a partial frame written in the output
	 *               stream, which might be unrecoverable and will require the
	 *               application to close and reopen the STOMP connection in
	 *               order to be able to send more messages.
	 */
	res = stomp_send(conn->stomp_session, i, headers, body, bodysz);

	if (headers != usualhdrs) free(headers);

	if (res < 0) {
		lua_pushnil(L);
		lua_pushliteral(L, "failed");
		return 2;
	}

	lua_pushboolean(L, 1);
	return 1;
}

/* filedesno = connection:getfd() */
static int stompws_getfd (lua_State *L)
{
	stompws_Connection *conn = tostompws(L);

	lua_pushinteger(L, libwebsocket_get_socket_fd(conn->ws_socket));
	return 1;
}

static const luaL_Reg mth[] = {
	{"__gc",     stompws_close},
	{"close",    stompws_close},
	{"send",     stompws_send},
	{"subscribe",stompws_subscribe},
	{"dispatch", stompws_dispatch},
	{"getfd",    stompws_getfd},
	{NULL,       NULL}
};

static const luaL_Reg lib[] = {
	{"connect", stompws_connect},
	{NULL,      NULL}
};

LUALIB_API int luaopen_stompws (lua_State *L)
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
