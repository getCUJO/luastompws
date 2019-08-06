/*
 * Copyright 2019 Evgeni Dobrev <evgeni@studio-punkt.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef STOMP_H
#define STOMP_H

#include <sys/types.h>
#include <libwebsockets.h>

typedef struct _stomp_session stomp_session_t;

struct stomp_hdr {
	const char *key;
	const char *val;
};

struct stomp_ctx_connected {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
};

struct stomp_ctx_receipt {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
};

struct stomp_ctx_error {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
	const void *body;
	size_t body_len;
};
struct stomp_ctx_message {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
	const void *body;
	size_t body_len;
};

enum stomp_prot {
	SPL_10,
	SPL_11,
	SPL_12,
};

enum stomp_cb_type {
	SCB_CONNECTED,
	SCB_ERROR,
	SCB_MESSAGE,
	SCB_RECEIPT,
};

typedef void(*stomp_cb_t)(stomp_session_t *, void *, void *);

void stomp_callback_set(stomp_session_t *, enum stomp_cb_type, stomp_cb_t);

stomp_session_t *stomp_session_new(void *);

void stomp_session_free(stomp_session_t *);

int stomp_disconnect(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_subscribe(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_unsubscribe(stomp_session_t *, int, size_t, const struct stomp_hdr *);

int stomp_begin(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_abort(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_ack(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_nack(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_commit(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_send(stomp_session_t *, size_t, const struct stomp_hdr *, const void *,
    size_t);

int stomp_recv_cmd(stomp_session_t *, const unsigned char*, size_t);

int stomp_connect(stomp_session_t *, struct lws *,size_t,
    const struct stomp_hdr *);

int stomp_send_heartbeat(stomp_session_t *);
int stomp_get_broker_hb(stomp_session_t *);
int stomp_get_client_hb(stomp_session_t *);

#endif /* STOMP_H */
