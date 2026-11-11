/**
 * @copyright Copyright (c) 2026, mp0rta
 *
 * A client and a server engine of this process, both with multipath, open
 * path 1 on a connection that carries nothing else. The test carries their
 * datagrams, path 0 from client port 5555 and path 1 from port 5556, each
 * one after a fixed one-way delay unless the case drops it, on a virtual
 * clock that jumps to the next arrival or engine timer: a case runs the same
 * way every time.
 */

#include <CUnit/CUnit.h>
#include <string.h>
#include <arpa/inet.h>

#include "xqc_new_path_test.h"
#include "xqc_common_test.h"
#include "src/common/xqc_time.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet.h"

#define XQC_TEST_NP_QUEUE       64
#define XQC_TEST_NP_DELAY       10000       /* one-way delay, microseconds */
#define XQC_TEST_NP_NEVER       XQC_MAX_UINT64_VALUE
#define XQC_TEST_NP_MAX_STEPS   100000
#define XQC_TEST_NP_SERVER_PORT 4433
#define XQC_TEST_NP_CLIENT_PORT 5555        /* of path 0, path n adds n */

typedef struct {
    unsigned char           buf[XQC_CONN_MAX_UDP_PAYLOAD_SIZE];
    size_t                  len;
    uint64_t                path_id;
    xqc_usec_t              arrival;
    /* sent by the server with less than 1200 bytes left under the limit */
    xqc_bool_t              short_budget;
} xqc_test_np_dgram_t;

/* the datagrams in flight one way, in the order they arrive */
typedef struct {
    xqc_test_np_dgram_t     dgram[XQC_TEST_NP_QUEUE];
    unsigned int            head;
    unsigned int            n;
} xqc_test_np_queue_t;

typedef struct xqc_test_np_s xqc_test_np_t;

/* whether a case drops a datagram of len bytes that an end sends on path 1 */
typedef xqc_bool_t (*xqc_test_np_drop_pt)(xqc_test_np_t *t,
    xqc_bool_t from_server, size_t len);

struct xqc_test_np_s {
    xqc_engine_t           *client;
    xqc_engine_t           *server;
    xqc_connection_t       *client_conn;
    xqc_connection_t       *server_conn;
    xqc_usec_t              client_wake;
    xqc_usec_t              server_wake;
    xqc_test_np_queue_t     to_client;
    xqc_test_np_queue_t     to_server;
    xqc_test_np_drop_pt     drop;
    xqc_bool_t              overflow;

    /* path 1, as the test carried it */
    unsigned int            client_sent;
    unsigned int            client_unexpanded;
    unsigned int            client_dropped;
    unsigned int            server_sent;
    unsigned int            server_dropped;

    /*
     * RFC 9000 Section 8.1: until the server validates the client address
     * of path 1, it sends to it at most three times the bytes it received
     * from it
     */
    uint64_t                server_recv;
    uint64_t                server_unvalidated;
    xqc_bool_t              server_validated;
    xqc_bool_t              amplified;

    /*
     * the datagrams the server sent on path 1 with less than 1200 bytes left
     * under that limit; for the first one, its size, the bytes received
     * before it, and the state of path 1 at the client when it arrived (-1
     * until it does)
     */
    unsigned int            short_budget;
    size_t                  short_budget_len;
    uint64_t                short_budget_recv;
    int                     short_budget_client_state;

    xqc_timestamp_pt        saved_clock;
};

/* the case being run, on the virtual clock g_np_now */
static xqc_test_np_t    g_np;
static xqc_usec_t       g_np_now;

static xqc_usec_t
xqc_test_np_clock(void)
{
    return g_np_now;
}

static xqc_path_ctx_t *
xqc_test_np_path(xqc_connection_t *conn)
{
    return xqc_conn_find_path_by_path_id(conn, 1);
}

static void
xqc_test_np_addr(struct sockaddr_in *addr, uint64_t path_id,
    xqc_bool_t server)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(server ? XQC_TEST_NP_SERVER_PORT
                                  : XQC_TEST_NP_CLIENT_PORT + path_id);
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static void
xqc_test_np_push(xqc_test_np_t *t, xqc_test_np_queue_t *q, uint64_t path_id,
    const unsigned char *buf, size_t len, xqc_bool_t short_budget)
{
    xqc_test_np_dgram_t *d;

    if (q->n == XQC_TEST_NP_QUEUE || len > sizeof(d->buf)) {
        t->overflow = XQC_TRUE;
        return;
    }

    d = &q->dgram[(q->head + q->n++) % XQC_TEST_NP_QUEUE];
    memcpy(d->buf, buf, len);
    d->len = len;
    d->path_id = path_id;
    d->arrival = g_np_now + XQC_TEST_NP_DELAY;
    d->short_budget = short_budget;
}

/*
 * What the server sends on path 1, against the limit while it has not
 * validated the path; whether less than 1200 bytes were left for it
 */
static xqc_bool_t
xqc_test_np_server_account(xqc_test_np_t *t, size_t len)
{
    xqc_path_ctx_t *path;
    uint64_t allowed = 3 * t->server_recv;
    xqc_bool_t short_budget;

    path = xqc_conn_find_path_by_path_id(t->server_conn, 1);
    if (path != NULL && path->path_state == XQC_PATH_STATE_ACTIVE) {
        t->server_validated = XQC_TRUE;
    }

    if (t->server_validated) {
        return XQC_FALSE;
    }

    short_budget = t->server_unvalidated + XQC_QUIC_MIN_MSS > allowed;
    if (short_budget && t->short_budget++ == 0) {
        t->short_budget_len = len;
        t->short_budget_recv = t->server_recv;
    }

    t->server_unvalidated += len;
    if (t->server_unvalidated > allowed) {
        t->amplified = XQC_TRUE;
    }

    return short_budget;
}

static ssize_t
xqc_test_np_send(xqc_bool_t from_server, uint64_t path_id,
    const unsigned char *buf, size_t size)
{
    xqc_test_np_t *t = &g_np;
    xqc_bool_t dropped = XQC_FALSE;
    xqc_bool_t short_budget = XQC_FALSE;

    if (path_id == 1) {
        if (from_server) {
            short_budget = xqc_test_np_server_account(t, size);
            t->server_sent++;

        } else {
            t->client_sent++;
            if (size < XQC_QUIC_MIN_MSS) {
                t->client_unexpanded++;
            }
        }

        dropped = t->drop != NULL && t->drop(t, from_server, size);
        if (dropped) {
            if (from_server) {
                t->server_dropped++;

            } else {
                t->client_dropped++;
            }
        }
    }

    if (!dropped) {
        xqc_test_np_push(t, from_server ? &t->to_client : &t->to_server,
                         path_id, buf, size, short_budget);
    }

    return size;
}

static ssize_t
xqc_test_np_client_write(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    return xqc_test_np_send(XQC_FALSE, 0, buf, size);
}

static ssize_t
xqc_test_np_client_write_ex(uint64_t path_id, const unsigned char *buf,
    size_t size, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    return xqc_test_np_send(XQC_FALSE, path_id, buf, size);
}

static ssize_t
xqc_test_np_server_write(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    return xqc_test_np_send(XQC_TRUE, 0, buf, size);
}

static ssize_t
xqc_test_np_server_write_ex(uint64_t path_id, const unsigned char *buf,
    size_t size, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    return xqc_test_np_send(XQC_TRUE, path_id, buf, size);
}

static void
xqc_test_np_set_timer(xqc_usec_t wake_after, void *engine_user_data)
{
    xqc_usec_t *wake = engine_user_data;

    *wake = g_np_now + wake_after;
}

static int
xqc_test_np_accept(xqc_engine_t *engine, xqc_connection_t *conn,
    const xqc_cid_t *cid, void *user_data)
{
    return 0;
}

static void
xqc_test_np_save_token(const unsigned char *token, uint32_t token_len,
    void *user_data)
{
}

static void
xqc_test_np_save_string(const char *data, size_t data_len, void *user_data)
{
}

static xqc_engine_t *
xqc_test_np_engine(xqc_engine_type_t type, xqc_usec_t *wake)
{
    xqc_bool_t server = type == XQC_ENGINE_SERVER;
    xqc_engine_ssl_config_t ssl;
    xqc_engine_callback_t cb = {
        .log_callbacks = xqc_null_log_cb,
        .set_event_timer = xqc_test_np_set_timer,
    };
    xqc_transport_callbacks_t tcbs = {
        .write_socket = server ? xqc_test_np_server_write
                               : xqc_test_np_client_write,
        .write_socket_ex = server ? xqc_test_np_server_write_ex
                                  : xqc_test_np_client_write_ex,
        .server_accept = xqc_test_np_accept,
        .save_token = xqc_test_np_save_token,
        .save_session_cb = xqc_test_np_save_string,
        .save_tp_cb = xqc_test_np_save_string,
    };
    xqc_app_proto_callbacks_t ap;
    xqc_engine_t *engine;

    memset(&ssl, 0, sizeof(ssl));
    ssl.private_key_file = "./server.key";
    ssl.cert_file = "./server.crt";
    ssl.ciphers = XQC_TLS_CIPHERS;
    ssl.groups = XQC_TLS_GROUPS;

    engine = xqc_engine_create(type, NULL, &ssl, &cb, &tcbs, wake);
    if (engine == NULL) {
        return NULL;
    }

    memset(&ap, 0, sizeof(ap));
    if (xqc_engine_register_alpn(engine, "transport", 9, &ap, NULL)
        != XQC_OK)
    {
        xqc_engine_destroy(engine);
        return NULL;
    }

    return engine;
}

/* hand the datagrams that arrived by now to their end */
static void
xqc_test_np_deliver(xqc_test_np_t *t, xqc_bool_t to_server)
{
    xqc_test_np_queue_t *q = to_server ? &t->to_server : &t->to_client;
    xqc_engine_t *engine = to_server ? t->server : t->client;
    struct sockaddr_in local, peer;
    xqc_test_np_dgram_t *d;
    xqc_path_ctx_t *path;
    xqc_bool_t delivered = XQC_FALSE;

    while (q->n > 0 && q->dgram[q->head].arrival <= g_np_now) {
        d = &q->dgram[q->head];
        xqc_test_np_addr(&local, d->path_id, to_server);
        xqc_test_np_addr(&peer, d->path_id, !to_server);
        if (to_server && d->path_id == 1) {
            t->server_recv += d->len;
        }

        if (d->short_budget && t->short_budget_client_state < 0) {
            path = xqc_test_np_path(t->client_conn);
            t->short_budget_client_state = path != NULL
                                           ? (int)path->path_state
                                           : XQC_PATH_STATE_CLOSED;
        }

        xqc_engine_packet_process(engine, d->buf, d->len,
                                  (struct sockaddr *)&local, sizeof(local),
                                  (struct sockaddr *)&peer, sizeof(peer),
                                  g_np_now, NULL);
        q->head = (q->head + 1) % XQC_TEST_NP_QUEUE;
        q->n--;
        delivered = XQC_TRUE;
    }

    if (delivered) {
        xqc_engine_finish_recv(engine);
    }
}

static xqc_usec_t
xqc_test_np_next_event(xqc_test_np_t *t)
{
    xqc_usec_t next = xqc_min(t->client_wake, t->server_wake);

    if (t->to_server.n > 0) {
        next = xqc_min(next, t->to_server.dgram[t->to_server.head].arrival);
    }

    if (t->to_client.n > 0) {
        next = xqc_min(next, t->to_client.dgram[t->to_client.head].arrival);
    }

    return next;
}

/* run both ends until the clock reads until */
static void
xqc_test_np_run(xqc_test_np_t *t, xqc_usec_t until)
{
    xqc_usec_t next;
    int steps = 0;

    while ((next = xqc_test_np_next_event(t)) <= until
           && steps++ < XQC_TEST_NP_MAX_STEPS)
    {
        g_np_now = xqc_max(g_np_now, next);
        xqc_test_np_deliver(t, XQC_TRUE);
        xqc_test_np_deliver(t, XQC_FALSE);

        if (t->server_wake <= g_np_now) {
            t->server_wake = XQC_TEST_NP_NEVER;
            xqc_engine_main_logic(t->server);
        }

        if (t->client_wake <= g_np_now) {
            t->client_wake = XQC_TEST_NP_NEVER;
            xqc_engine_main_logic(t->client);
        }
    }

    CU_ASSERT(steps <= XQC_TEST_NP_MAX_STEPS);
    g_np_now = xqc_max(g_np_now, until);
}

static void
xqc_test_np_close(xqc_test_np_t *t)
{
    CU_ASSERT(!t->overflow);
    if (t->client != NULL) {
        xqc_engine_destroy(t->client);
    }

    if (t->server != NULL) {
        xqc_engine_destroy(t->server);
    }

    xqc_monotonic_timestamp = t->saved_clock;
}

/*
 * A handshake on path 0, from a fixed time of the virtual clock, then the
 * client opens path 1, where drop chooses the datagrams lost. NULL, the
 * failure asserted, when that cannot be done.
 */
static xqc_test_np_t *
xqc_test_np_open(xqc_test_np_drop_pt drop)
{
    xqc_test_np_t *t = &g_np;
    xqc_conn_settings_t settings;
    xqc_conn_ssl_config_t conn_ssl;
    struct sockaddr_in server_addr;
    const xqc_cid_t *cid;
    uint64_t path_id = 0;
    xqc_int_t ret;

    memset(t, 0, sizeof(*t));
    t->client_wake = XQC_TEST_NP_NEVER;
    t->server_wake = XQC_TEST_NP_NEVER;
    t->short_budget_client_state = -1;
    t->saved_clock = xqc_monotonic_timestamp;
    g_np_now = 1000000000000000ULL;
    xqc_monotonic_timestamp = xqc_test_np_clock;

    t->client = xqc_test_np_engine(XQC_ENGINE_CLIENT, &t->client_wake);
    t->server = xqc_test_np_engine(XQC_ENGINE_SERVER, &t->server_wake);
    CU_ASSERT(t->client != NULL && t->server != NULL);
    if (t->client == NULL || t->server == NULL) {
        goto fail;
    }

    memset(&settings, 0, sizeof(settings));
    settings.proto_version = XQC_VERSION_V1;
    settings.enable_multipath = 1;
    xqc_server_set_conn_settings(t->server, &settings);

    memset(&conn_ssl, 0, sizeof(conn_ssl));
    xqc_test_np_addr(&server_addr, 0, XQC_TRUE);
    cid = xqc_connect(t->client, &settings, NULL, 0, "", 0, &conn_ssl,
                      (struct sockaddr *)&server_addr, sizeof(server_addr),
                      "transport", NULL);
    CU_ASSERT(cid != NULL);
    if (cid == NULL) {
        goto fail;
    }

    t->client_conn = xqc_engine_conns_hash_find(t->client, cid, 's');

    /* the handshake, and the connection IDs of path 1 */
    xqc_test_np_run(t, g_np_now + 500000);
    if (t->client_conn != NULL) {
        t->server_conn = xqc_engine_conns_hash_find(t->server,
                             &t->client_conn->dcid_set.current_dcid, 's');
    }

    CU_ASSERT(t->server_conn != NULL
              && xqc_conn_is_handshake_confirmed(t->client_conn)
              && xqc_conn_is_handshake_confirmed(t->server_conn)
              && t->client_conn->enable_multipath);
    if (t->server_conn == NULL
        || !xqc_conn_is_handshake_confirmed(t->client_conn)
        || !xqc_conn_is_handshake_confirmed(t->server_conn)
        || !t->client_conn->enable_multipath)
    {
        goto fail;
    }

    t->drop = drop;
    ret = xqc_conn_create_path(t->client, cid, &path_id, 0);
    CU_ASSERT(ret == XQC_OK && path_id == 1);
    if (ret != XQC_OK || path_id != 1) {
        goto fail;
    }

    return t;

fail:
    xqc_test_np_close(t);
    return NULL;
}

/* both ends validated path 1, the server within the limit */
static void
xqc_test_np_check_validated(xqc_test_np_t *t)
{
    xqc_path_ctx_t *client_path = xqc_test_np_path(t->client_conn);
    xqc_path_ctx_t *server_path = xqc_test_np_path(t->server_conn);

    CU_ASSERT(client_path != NULL
              && client_path->path_state == XQC_PATH_STATE_ACTIVE);
    CU_ASSERT(server_path != NULL
              && server_path->path_state == XQC_PATH_STATE_ACTIVE);
    CU_ASSERT(t->server_validated);
    CU_ASSERT(!t->amplified);
}

/*
 * RFC 9000 Sections 8.1 and 8.2: with no loss, both ends validate path 1,
 * and the server never runs short of budget for an expanded datagram.
 */
void
xqc_test_new_path_validated_by_both_ends(void)
{
    xqc_test_np_t *t = xqc_test_np_open(NULL);

    if (t == NULL) {
        return;
    }

    xqc_test_np_run(t, g_np_now + 1000000);

    xqc_test_np_check_validated(t);
    CU_ASSERT_EQUAL(t->short_budget, 0);

    xqc_test_np_close(t);
}

/*
 * The server's first datagram on path 1, its PATH_CHALLENGE, is lost, and
 * so is the first datagram of the client there that is not expanded, the
 * ACK of the server's PATH_RESPONSE, which validated the path for the
 * client
 */
static xqc_bool_t
xqc_test_np_drop_challenge_and_ack(xqc_test_np_t *t, xqc_bool_t from_server,
    size_t len)
{
    if (from_server) {
        return t->server_sent == 1;
    }

    return len < XQC_QUIC_MIN_MSS && t->client_unexpanded == 1;
}

/*
 * RFC 9000 Section 8.2.1: when the server retransmits its PATH_CHALLENGE
 * with less than 1200 bytes left under the anti-amplification limit, it
 * sends it without expanding it, and the path is validated on both ends.
 */
void
xqc_test_new_path_challenge_resent_unexpanded(void)
{
    xqc_test_np_t *t = xqc_test_np_open(xqc_test_np_drop_challenge_and_ack);

    if (t == NULL) {
        return;
    }

    xqc_test_np_run(t, g_np_now + 5000000);

    CU_ASSERT_EQUAL(t->server_dropped, 1);
    CU_ASSERT_EQUAL(t->client_dropped, 1);

    /*
     * all the server had received on path 1 was the expanded PATH_CHALLENGE
     * of the client, and it reached the client after the PATH_RESPONSE that
     * validated the path there
     */
    CU_ASSERT(t->short_budget > 0);
    CU_ASSERT_EQUAL(t->short_budget_recv, XQC_QUIC_MIN_MSS);
    CU_ASSERT(t->short_budget_len < XQC_QUIC_MIN_MSS);
    CU_ASSERT_EQUAL(t->short_budget_client_state, XQC_PATH_STATE_ACTIVE);
    xqc_test_np_check_validated(t);

    xqc_test_np_close(t);
}

/*
 * The server's PATH_CHALLENGE and PATH_RESPONSE on path 1 are lost, and so
 * is all the client sends there after its PATH_CHALLENGE, until the server
 * sent a datagram on path 1 with less than 1200 bytes left under the limit
 */
static xqc_bool_t
xqc_test_np_drop_until_short_budget(xqc_test_np_t *t, xqc_bool_t from_server,
    size_t len)
{
    if (t->short_budget > 0) {
        return XQC_FALSE;
    }

    if (from_server) {
        return len >= XQC_QUIC_MIN_MSS;
    }

    return t->client_sent > 1;
}

/*
 * draft-ietf-quic-multipath-21 Section 3.1 and RFC 9000 Section 8.2.1: an
 * unexpanded PATH_CHALLENGE that reaches the client while it still
 * validates path 1 is answered, not taken for a path that cannot carry 1200
 * bytes, and the path is validated on both ends.
 */
void
xqc_test_new_path_small_challenge_while_validating(void)
{
    xqc_test_np_t *t = xqc_test_np_open(xqc_test_np_drop_until_short_budget);

    if (t == NULL) {
        return;
    }

    xqc_test_np_run(t, g_np_now + 5000000);

    CU_ASSERT_EQUAL(t->server_dropped, 2);
    CU_ASSERT(t->short_budget > 0);
    CU_ASSERT_EQUAL(t->short_budget_recv, XQC_QUIC_MIN_MSS);
    CU_ASSERT(t->short_budget_len < XQC_QUIC_MIN_MSS);
    CU_ASSERT_EQUAL(t->short_budget_client_state, XQC_PATH_STATE_VALIDATING);
    xqc_test_np_check_validated(t);

    xqc_test_np_close(t);
}
