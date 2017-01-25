/*
 * Copyright (c) 2016-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#pragma once

#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <mpi.h>
#include <limits.h> /* Just for PATH_MAX */
#include <unistd.h>

/* ANL libs */
#include <mercury_request.h>
#include <mercury_macros.h>
#include <mercury_proc_string.h>
#include <ssg.h>
#include <ssg-mpi.h>
#include <ch-placement.h>

/* CMU libs */
#include <deltafs/deltafs_api.h>
#include "preload_internal.h"
#include "preload.h"

#define SHUFFLE_DEBUG
#define SHUFFLE_DEBUG_OUTPUT 0
#define SHUFFLE_LOG(fmt, ...) \
    do { \
        if (SHUFFLE_DEBUG_OUTPUT) { \
            fprintf(stderr, fmt, ##__VA_ARGS__); \
            fflush(stderr); \
         } \
     } while(0)

/*
 * Threshold that determines whether a write is small enough to use
 * point-to-point Mercury RPCs. Otherwise we fall back to a bulk transfer.
 * In bytes, obvi.
 */
#define SHUFFLE_SMALL_WRITE 1024

#define HG_PROTO "bmi+tcp"

/*
 * shuffle context:
 *   - run-time state of the preload and shuffle lib
 */
typedef struct shuffle_ctx {
    /* Mercury context */
    char hgaddr[NI_MAXHOST+20];        /* proto://ip:port of host */

    hg_class_t *hgcl;
    hg_context_t *hgctx;
    hg_request_class_t *hgreqcl;
    hg_id_t write_id;
    hg_id_t shutdown_id;
#ifdef SHUFFLE_DEBUG
    hg_id_t ping_id;
#endif

    /* SSG context */
    ssg_t s;
    int shutdown_flag;

    /* ch-placement context */
    struct ch_placement_instance *chinst;
} shuffle_ctx_t;

#define SHUFFLE_CTX_INITIALIZER { MAYBE_MUTEX_INITIALIZER }

/* Generate RPC structs */
#ifdef SHUFFLE_DEBUG
MERCURY_GEN_PROC(ping_t, ((int32_t)(rank)))
#endif
MERCURY_GEN_PROC(write_in_t, ((hg_const_string_t)(fname))
                             ((hg_bulk_t)(data_handle))
                             ((hg_string_t)(data))
                             ((hg_uint64_t)(data_len))
                             ((hg_int32_t)(rank_in))
                             ((hg_int32_t)(isbulk)))
MERCURY_GEN_PROC(write_out_t, ((hg_int64_t)(ret)))

extern shuffle_ctx_t sctx;

/* shuffle_config.cc */
void msg_abort(const char *msg);
void genHgAddr(void);
void shuffle_init(void);
void shuffle_destroy(void);

#ifdef SHUFFLE_DEBUG
/* shuffle_ping.cc */
hg_return_t ping_rpc_handler(hg_handle_t h);
void ping_test(int rank);
#endif

/* shuffle_shutdown.cc */
hg_return_t shutdown_rpc_handler(hg_handle_t h);
void shuffle_shutdown(int rank);

/* shuffle_write.cc */
int shuffle_write_local(const char *fn, char *data, int len);
hg_return_t write_rpc_handler(hg_handle_t h);
int shuffle_write(const char *fn, char *data, int len);
