/*
 * Copyright (c) 2016 Carnegie Mellon University.
 * George Amvrosiadis <gamvrosi@cs.cmu.edu>
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
#include <mpi.h>
#include <set>

/* ANL libs */
#include <mercury_request.h>
#include <mercury_macros.h>
#include <ssg.h>
#include <ssg-mpi.h>

/* CMU libs */
#include <pdlfs-common/port.h>

//#define DELTAFS_SHUFFLE_DEBUG

#define HG_PROTO "bmi+tcp"

/*
 * Shuffle context: run-time state of the preload and shuffle lib
 */
typedef struct shuffle_ctx {
    /* Preload context */
    const char *root;
    int len_root;                       /* strlen root */
    int testin;                         /* just testing */

    pdlfs::port::Mutex setlock;
    std::set<FILE *> isdeltafs;

    /* Mercury context */
    char hgaddr[NI_MAXHOST+20];         /* proto://ip:port of host */

    hg_class_t *hgcl;
    hg_context_t *hgctx;
    hg_request_class_t *hgreqcl;

    /* SSG context */
    ssg_t s;
    int shutdown_flag;                  /* XXX: Used for testing */
} shuffle_ctx_t;

/* Generate RPC structs */
#ifdef DELTAFS_SHUFFLE_DEBUG
MERCURY_GEN_PROC(ping_t, ((int32_t)(rank)))
#endif
MERCURY_GEN_PROC(write_t, ((int32_t)(rank)))

extern shuffle_ctx_t sctx;

/* shuffle_config.cc */
void msg_abort(const char *msg);
void genHgAddr(void);
void shuffle_init(void);
void shuffle_destroy(void);

/* shuffle_rpc.cc */
hg_return_t ping_rpc_handler(hg_handle_t h);
hg_return_t write_rpc_handler(hg_handle_t h);
hg_return_t shutdown_rpc_handler(hg_handle_t h);
