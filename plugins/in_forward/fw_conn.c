/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2024 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_input_plugin.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_engine.h>
#include <fluent-bit/flb_network.h>
#include <fluent-bit/flb_downstream.h>

#include "fw.h"
#include "fw_prot.h"
#include "fw_conn.h"

/* Callback invoked every time an event is triggered for a connection */
int fw_conn_event(void *data)
{
    int ret;
    int bytes;
    int available;
    int size;
    char *tmp;
    struct fw_conn *conn;
    struct mk_event *event;
    struct flb_in_fw_config *ctx;
    struct flb_connection *connection;

    connection = (struct flb_connection *) data;

    /* Get ctx from the connection's plugin_context to safely acquire mutex */
    ctx = (struct flb_in_fw_config *) connection->plugin_context;

    /* Bail out early if ctx is NULL (should not happen in normal operation) */
    if (!ctx) {
        return -1;
    }

    /*
     * Acquire mutex BEFORE accessing connection->user_data to prevent race condition.
     * We'll release it before any blocking operations to avoid deadlock.
     */
    pthread_mutex_lock(&ctx->conn_mutex);

    /* Now safely check if connection is still valid after acquiring lock */
    conn = connection->user_data;
    if (!conn || ctx->is_paused) {
        pthread_mutex_unlock(&ctx->conn_mutex);
        return -1;
    }

    /*
     * IMPORTANT: We now have a valid conn pointer. We'll keep using it but
     * must unlock the mutex before any potentially blocking operations
     * (like fw_prot_secure_forward_handshake, fw_prot_process, flb_io_net_read)
     * to avoid deadlocks.
     */

    event = &connection->event;

    if (event->mask & MK_EVENT_READ) {
        if (conn->handshake_status == FW_HANDSHAKE_PINGPONG) {
            flb_plg_trace(ctx->ins, "handshake status = %d", conn->handshake_status);

            /* Unlock mutex before potentially blocking handshake operation */
            pthread_mutex_unlock(&ctx->conn_mutex);

            ret = fw_prot_secure_forward_handshake(ctx->ins, conn);
            if (ret == -1) {
                flb_plg_trace(ctx->ins, "fd=%i closed connection", event->fd);
                /* Re-acquire mutex to safely delete connection */
                pthread_mutex_lock(&ctx->conn_mutex);
                fw_conn_del(conn);
                pthread_mutex_unlock(&ctx->conn_mutex);
                return -1;
            }

            /* Re-acquire mutex to update handshake status */
            pthread_mutex_lock(&ctx->conn_mutex);
            /* Re-check connection is still valid */
            if (connection->user_data == NULL) {
                pthread_mutex_unlock(&ctx->conn_mutex);
                return -1;
            }
            conn->handshake_status = FW_HANDSHAKE_ESTABLISHED;
            pthread_mutex_unlock(&ctx->conn_mutex);
            return 0;
        }

        flb_plg_trace(ctx->ins, "handshake status = %d", conn->handshake_status);

        available = (conn->buf_size - conn->buf_len);
        if (available < 1) {
            if (conn->buf_size >= ctx->buffer_max_size) {
                flb_plg_warn(ctx->ins, "fd=%i incoming data exceed limit (%lu bytes)",
                             event->fd, (ctx->buffer_max_size));
                /* Keep mutex held for fw_conn_del */
                fw_conn_del(conn);
                pthread_mutex_unlock(&ctx->conn_mutex);
                return -1;
            }
            else if (conn->buf_size + ctx->buffer_chunk_size > ctx->buffer_max_size) {
                /* no space to add buffer_chunk_size */
                /* set maximum size */
                size = ctx->buffer_max_size;
            }
            else {
                size = conn->buf_size + ctx->buffer_chunk_size;
            }
            tmp = flb_realloc(conn->buf, size);
            if (!tmp) {
                flb_errno();
                pthread_mutex_unlock(&ctx->conn_mutex);
                return -1;
            }
            flb_plg_trace(ctx->ins, "fd=%i buffer realloc %i -> %i",
                          event->fd, conn->buf_size, size);

            conn->buf = tmp;
            conn->buf_size = size;
            available = (conn->buf_size - conn->buf_len);
        }

        /* Unlock mutex before I/O operation */
        pthread_mutex_unlock(&ctx->conn_mutex);

        bytes = flb_io_net_read(connection,
                                (void *) &conn->buf[conn->buf_len],
                                available);

        if (bytes > 0) {
            flb_plg_trace(ctx->ins, "read()=%i pre_len=%i now_len=%i",
                          bytes, conn->buf_len, conn->buf_len + bytes);
            conn->buf_len += bytes;

            ret = fw_prot_process(ctx->ins, conn);
            if (ret == -1) {
                /* Re-acquire mutex to safely delete connection */
                pthread_mutex_lock(&ctx->conn_mutex);
                fw_conn_del(conn);
                pthread_mutex_unlock(&ctx->conn_mutex);
                return -1;
            }
            return bytes;
        }
        else {
            flb_plg_trace(ctx->ins, "fd=%i closed connection", event->fd);
            /* Re-acquire mutex to safely delete connection */
            pthread_mutex_lock(&ctx->conn_mutex);
            fw_conn_del(conn);
            pthread_mutex_unlock(&ctx->conn_mutex);
            return -1;
        }
    }

    if (event->mask & MK_EVENT_CLOSE) {
        flb_plg_trace(ctx->ins, "fd=%i hangup", event->fd);
        /* Keep mutex held for fw_conn_del */
        fw_conn_del(conn);
        pthread_mutex_unlock(&ctx->conn_mutex);
        return -1;
    }

    pthread_mutex_unlock(&ctx->conn_mutex);
    return 0;
}

/* Create a new Forward request instance */
struct fw_conn *fw_conn_add(struct flb_connection *connection, struct flb_in_fw_config *ctx)
{
    struct fw_conn *conn;
    int             ret;
    struct flb_in_fw_helo *helo = NULL;

    conn = flb_calloc(1, sizeof(struct fw_conn));
    if (!conn) {
        flb_errno();

        return NULL;
    }

    conn->handshake_status = FW_HANDSHAKE_ESTABLISHED;
    /*
     * Always force the secure-forward handshake when:
     *  - a shared key is configured, or
     *  - empty_shared_key is enabled (empty string shared key), or
     *  - user authentication is configured (users > 0).
     *
     * This closes the gap where "users-only" previously skipped authentication entirely.
     */
    conn->handshake_status = FW_HANDSHAKE_ESTABLISHED; /* default */
    if (ctx->shared_key != NULL ||
        ctx->empty_shared_key == FLB_TRUE ||
        mk_list_size(&ctx->users) > 0) {
        conn->handshake_status = FW_HANDSHAKE_HELO;
        helo = flb_calloc(1, sizeof(struct flb_in_fw_helo));
        if (!helo) {
            flb_errno();
            flb_free(conn);
            return NULL;
        }

        ret = fw_prot_secure_forward_handshake_start(ctx->ins, connection, helo);
        if (ret != 0) {
            flb_free(helo);
            flb_free(conn);

            return NULL;
        }

        conn->handshake_status = FW_HANDSHAKE_PINGPONG;
    }

    conn->connection = connection;
    conn->helo       = helo;

    /* Set data for the event-loop */
    connection->user_data     = conn;
    connection->plugin_context = ctx;
    connection->event.type    = FLB_ENGINE_EV_CUSTOM;
    connection->event.handler = fw_conn_event;

    /* Connection info */
    conn->ctx     = ctx;
    conn->buf_len = 0;
    conn->rest    = 0;
    conn->status  = FW_NEW;

    /* Allocate read buffer */
    conn->buf = flb_malloc(ctx->buffer_chunk_size);
    if (!conn->buf) {
        flb_errno();
        if (conn->helo != NULL) {
            flb_free(conn->helo);
        }
        flb_free(conn);
        return NULL;
    }
    conn->buf_size = ctx->buffer_chunk_size;
    conn->in       = ctx->ins;

    conn->compression_type = FLB_COMPRESSION_ALGORITHM_NONE;
    conn->d_ctx = NULL;

    /* Register instance into the event loop */
    ret = mk_event_add(flb_engine_evl_get(),
                       connection->fd,
                       FLB_ENGINE_EV_CUSTOM,
                       MK_EVENT_READ,
                       &connection->event);
    if (ret == -1) {
        flb_plg_error(ctx->ins, "could not register new connection");
        if (conn->helo != NULL) {
            flb_free(conn->helo);
        }
        flb_free(conn->buf);
        flb_free(conn);
        return NULL;
    }

    mk_list_add(&conn->_head, &ctx->connections);
    return conn;
}

int fw_conn_del(struct fw_conn *conn)
{
    /*
     * IMPORTANT: This function must be called with ctx->conn_mutex held
     * because it modifies the ctx->connections list.
     *
     * NULL the user_data pointer to prevent any in-flight event handlers
     * from accessing this connection after it's freed
     */
    conn->connection->user_data = NULL;

    /* Remove from connections list while mutex is held */
    mk_list_del(&conn->_head);

    /* The downstream unregisters the file descriptor from the event-loop
     * so there's nothing to be done by the plugin
     */
    flb_downstream_conn_release(conn->connection);

    /* Release decompression context if it exists */
    if (conn->d_ctx) {
        flb_decompression_context_destroy(conn->d_ctx);
        conn->d_ctx = NULL;
    }

    if (conn->helo != NULL) {
        if (conn->helo->nonce != NULL) {
            flb_sds_destroy(conn->helo->nonce);
        }
        if (conn->helo->salt != NULL) {
            flb_sds_destroy(conn->helo->salt);
        }
        flb_free(conn->helo);
        conn->helo = NULL;
    }

    /* Free buffer and set to NULL to prevent use-after-free */
    if (conn->buf) {
        flb_free(conn->buf);
        conn->buf = NULL;
    }

    flb_free(conn);

    return 0;
}

int fw_conn_del_all(struct flb_in_fw_config *ctx)
{
    struct mk_list *tmp;
    struct mk_list *head;
    struct fw_conn *conn;

    mk_list_foreach_safe(head, tmp, &ctx->connections) {
        conn = mk_list_entry(head, struct fw_conn, _head);
        fw_conn_del(conn);
    }

    return 0;
}
