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
    int should_delete = 0;

    connection = (struct flb_connection *) data;

    conn = connection->user_data;

    /*
     * Acquire lifecycle lock to prevent concurrent deletion.
     * This protects against use-after-free when the connection is deleted
     * while the event handler is still executing.
     */
    pthread_mutex_lock(&conn->lifecycle_lock);

    /* Check if connection is marked for deletion */
    if (conn->being_deleted) {
        pthread_mutex_unlock(&conn->lifecycle_lock);
        return -1;
    }

    ctx = conn->ctx;
    event = &connection->event;

    if (event->mask & MK_EVENT_READ) {
        if (conn->handshake_status == FW_HANDSHAKE_PINGPONG) {
            flb_plg_trace(ctx->ins, "handshake status = %d", conn->handshake_status);

            ret = fw_prot_secure_forward_handshake(ctx->ins, conn);
            if (ret == -1) {
                flb_plg_trace(ctx->ins, "fd=%i closed connection", event->fd);
                should_delete = 1;
                goto cleanup;
            }

            conn->handshake_status = FW_HANDSHAKE_ESTABLISHED;
            pthread_mutex_unlock(&conn->lifecycle_lock);
            return 0;
        }

        flb_plg_trace(ctx->ins, "handshake status = %d", conn->handshake_status);

        available = (conn->buf_size - conn->buf_len);
        if (available < 1) {
            if (conn->buf_size >= ctx->buffer_max_size) {
                flb_plg_warn(ctx->ins, "fd=%i incoming data exceed limit (%lu bytes)",
                             event->fd, (ctx->buffer_max_size));
                should_delete = 1;
                goto cleanup;
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
                pthread_mutex_unlock(&conn->lifecycle_lock);
                return -1;
            }
            flb_plg_trace(ctx->ins, "fd=%i buffer realloc %i -> %i",
                          event->fd, conn->buf_size, size);

            conn->buf = tmp;
            conn->buf_size = size;
            available = (conn->buf_size - conn->buf_len);
        }

        bytes = flb_io_net_read(connection,
                                (void *) &conn->buf[conn->buf_len],
                                available);

        if (bytes > 0) {
            flb_plg_trace(ctx->ins, "read()=%i pre_len=%i now_len=%i",
                          bytes, conn->buf_len, conn->buf_len + bytes);
            conn->buf_len += bytes;

            ret = fw_prot_process(ctx->ins, conn);
            if (ret == -1) {
                should_delete = 1;
                goto cleanup;
            }
            pthread_mutex_unlock(&conn->lifecycle_lock);
            return bytes;
        }
        else {
            flb_plg_trace(ctx->ins, "fd=%i closed connection", event->fd);
            should_delete = 1;
            goto cleanup;
        }
    }

    if (event->mask & MK_EVENT_CLOSE) {
        flb_plg_trace(ctx->ins, "fd=%i hangup", event->fd);
        should_delete = 1;
        goto cleanup;
    }

    pthread_mutex_unlock(&conn->lifecycle_lock);
    return 0;

cleanup:
    /*
     * Check if shutdown is handling deletion while we still hold the lock.
     * This prevents the race where:
     * 1. We release the lock
     * 2. Shutdown sets being_deleted=1 and thinks we're done
     * 3. We call fw_conn_del() trying to acquire conn_mutex
     * 4. Shutdown also tries to acquire conn_mutex for next iteration
     * 5. DEADLOCK
     */
    int shutdown_in_progress = conn->being_deleted;

    /* Release lock before deleting to avoid deadlock */
    pthread_mutex_unlock(&conn->lifecycle_lock);

    if (should_delete && !shutdown_in_progress) {
        fw_conn_del(conn);
    }

    return -1;
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

    /* Initialize lifecycle lock to prevent use-after-free during deletion */
    pthread_mutex_init(&conn->lifecycle_lock, NULL);
    conn->being_deleted = 0;

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
    struct flb_in_fw_config *ctx = conn->ctx;

    /*
     * Step 1: Mark connection as being deleted.
     * This prevents new event handlers from processing this connection.
     */
    pthread_mutex_lock(&conn->lifecycle_lock);
    conn->being_deleted = 1;
    pthread_mutex_unlock(&conn->lifecycle_lock);

    /*
     * Step 2: Unregister from event loop.
     * The downstream unregisters the file descriptor from the event-loop
     * so no new events will be fired for this connection.
     */
    flb_downstream_conn_release(conn->connection);

    /*
     * Step 3: Remove from connections list (if still in it).
     * During shutdown, fw_conn_del_all() may have already moved this connection
     * to a temporary list, in which case we should skip removal to avoid deadlock.
     */
    pthread_mutex_lock(&ctx->conn_mutex);
    if (!mk_list_entry_orphan(&conn->_head)) {
        mk_list_del(&conn->_head);
    }
    pthread_mutex_unlock(&ctx->conn_mutex);

    /*
     * Step 4: Wait for any in-flight event handlers to complete.
     * By acquiring the lifecycle lock here, we ensure no handler is
     * currently executing. If a handler is running, this will block
     * until it completes and releases the lock.
     */
    pthread_mutex_lock(&conn->lifecycle_lock);
    pthread_mutex_unlock(&conn->lifecycle_lock);

    /*
     * Step 5: Now safe to destroy the lock and free all resources.
     * No event handlers can be running at this point.
     */
    pthread_mutex_destroy(&conn->lifecycle_lock);

    /* Release decompression context if it exists */
    if (conn->d_ctx) {
        flb_decompression_context_destroy(conn->d_ctx);
    }

    if (conn->helo != NULL) {
        if (conn->helo->nonce != NULL) {
            flb_sds_destroy(conn->helo->nonce);
        }
        if (conn->helo->salt != NULL) {
            flb_sds_destroy(conn->helo->salt);
        }
        flb_free(conn->helo);
    }
    flb_free(conn->buf);
    flb_free(conn);

    return 0;
}

int fw_conn_del_all(struct flb_in_fw_config *ctx)
{
    struct mk_list tmp_list;
    struct mk_list *head;
    struct mk_list *tmp;
    struct fw_conn *conn;

    /*
     * First, mark ALL connections as being deleted while holding conn_mutex.
     * This prevents any new fw_conn_del() calls from progressing.
     */
    pthread_mutex_lock(&ctx->conn_mutex);

    mk_list_foreach(head, &ctx->connections) {
        conn = mk_list_entry(head, struct fw_conn, _head);
        conn->being_deleted = 1;
    }

    pthread_mutex_unlock(&ctx->conn_mutex);

    /*
     * Now collect all connections. Since being_deleted is set,
     * no handlers will call fw_conn_del() on these connections.
     */
    mk_list_init(&tmp_list);

    pthread_mutex_lock(&ctx->conn_mutex);

    /* Move all connections from ctx->connections to tmp_list */
    mk_list_foreach_safe(head, tmp, &ctx->connections) {
        mk_list_del(head);
        mk_list_add(head, &tmp_list);
    }

    pthread_mutex_unlock(&ctx->conn_mutex);

    /*
     * Now process each connection without holding conn_mutex.
     * This allows event handlers to safely call fw_conn_del() on
     * other connections without deadlocking.
     */
    mk_list_foreach_safe(head, tmp, &tmp_list) {
        conn = mk_list_entry(head, struct fw_conn, _head);

        /* Remove from temporary list */
        mk_list_del(head);

        /*
         * Now delete the connection. We pass through a modified
         * deletion path that doesn't try to remove from list again.
         *
         * Handle partially-initialized connections gracefully.
         * During initialization failures, connections may be in the list
         * but not fully constructed (e.g., connection pointer may be NULL).
         */

        /*
         * Set being_deleted flag to prevent handlers from calling fw_conn_del().
         * We don't wait for lifecycle_lock to avoid circular dependency deadlock:
         * - Shutdown waiting for lifecycle_lock
         * - Handler holding lifecycle_lock, waiting for conn_mutex in fw_conn_del()
         */
        conn->being_deleted = 1;

        /*
         * Close the socket and unregister from event loop.
         * This interrupts any blocked I/O operations, causing handlers to exit.
         * Handlers will see being_deleted=1 and skip calling fw_conn_del().
         */
        if (conn->connection != NULL) {
            flb_downstream_conn_release(conn->connection);
        }

        /* Destroy lock and free resources */
        pthread_mutex_destroy(&conn->lifecycle_lock);

        if (conn->d_ctx) {
            flb_decompression_context_destroy(conn->d_ctx);
        }

        if (conn->helo != NULL) {
            if (conn->helo->nonce != NULL) {
                flb_sds_destroy(conn->helo->nonce);
            }
            if (conn->helo->salt != NULL) {
                flb_sds_destroy(conn->helo->salt);
            }
            flb_free(conn->helo);
        }
        flb_free(conn->buf);
        flb_free(conn);
    }

    return 0;
}
