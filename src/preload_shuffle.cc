/*
 * Copyright (c) 2017, Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>

#include "preload_internal.h"
#include "preload_mon.h"
#include "preload_shuffle.h"

#include "nn_shuffler.h"
#include "nn_shuffler_internal.h"

#include "shuffler/shuffler.h"

#include <ch-placement.h>
typedef struct ch_placement_instance* ch_t;
#include <deltafs-nexus/deltafs-nexus_api.h>
#include <mercury_config.h>
#include <pdlfs-common/xxhash.h>

#include "common.h"

/* shuffle context for the 3-hop shuffler. */
typedef struct _3h_ctx {
  shuffler_t sh;
  nexus_ctx_t nx;
  ch_t ch;
} _3h_ctx_t;

void shuffle_epoch_start(shuffle_ctx_t* ctx) {
  if (ctx->type == SHUFFLE_3HOP) {
    // TODO
  } else {
    nn_shuffler_bgwait();
  }
}

void shuffle_epoch_end(shuffle_ctx_t* ctx) {
  if (ctx->type == SHUFFLE_3HOP) {
    // TODO
  } else {
    nn_shuffler_flush_rpcq();
    if (!nnctx.force_sync) {
      /* wait for rpc replies */
      nn_shuffler_wait();
    }
  }
}

static void _3h_shuffle_deliver(int src, int dst, int type, void* buf,
                                int buf_sz) {
  char* input;
  size_t input_left;
  char path[PATH_MAX];
  char msg[200];
  const char* fname;
  size_t fname_len;
  uint32_t r;
  uint16_t e;
  int ha;
  int epoch;
  char* data;
  size_t len;
  int rv;
  int n;

  assert(buf_sz >= 0);
  input_left = static_cast<size_t>(buf_sz);
  input = static_cast<char*>(buf);
  assert(input != NULL);

  /* rank */
  if (input_left < 8) {
    msg_abort("rpc_corruption");
  }
  memcpy(&r, input, 4);
  if (src != ntohl(r)) msg_abort("bad src");
  input_left -= 4;
  input += 4;
  memcpy(&r, input, 4);
  if (dst != ntohl(r)) msg_abort("bad dst");
  input_left -= 4;
  input += 4;

  /* vpic fname */
  if (input_left < 1) {
    msg_abort("rpc_corruption");
  }
  fname_len = static_cast<unsigned char>(input[0]);
  input_left -= 1;
  input += 1;
  if (input_left < fname_len + 1) {
    msg_abort("rpc_corruption");
  }
  fname = input;
  assert(strlen(fname) == fname_len);
  input_left -= fname_len + 1;
  input += fname_len + 1;

  /* vpic data */
  if (input_left < 1) {
    msg_abort("rpc_corruption");
  }
  len = static_cast<unsigned char>(input[0]);
  input_left -= 1;
  input += 1;
  if (input_left < len) {
    msg_abort("rpc_corruption");
  }
  data = input;
  input_left -= len;
  input += len;

  /* epoch */
  if (input_left < 2) {
    msg_abort("rpc_corruption");
  }
  memcpy(&e, input, 2);
  epoch = ntohs(e);

  assert(pctx.len_plfsdir != 0);
  assert(pctx.plfsdir != NULL);
  snprintf(path, sizeof(path), "%s/%s", pctx.plfsdir, fname);
  rv = preload_foreign_write(path, data, len, epoch);

  /* write trace if we are in testing mode */
  if (pctx.testin && pctx.logfd != -1) {
    ha = pdlfs::xxhash32(data, len, 0); /* data checksum */
    n = snprintf(msg, sizeof(msg),
                 "[RECV] %s %d bytes (e%d) r%d "
                 "<< r%d (hash=%08x)\n",
                 path, int(len), epoch, dst, src, ha);
    n = write(pctx.logfd, msg, n);

    errno = 0;
  }

  if (rv != 0) {
    msg_abort("xxwrite");
  }
}

static int _3h_shuffle_write(_3h_ctx_t* ctx, const char* fn, char* data,
                             size_t len, int epoch) {
  char buf[200];
  char msg[200];
  hg_return_t hret;
  unsigned long target;
  const char* fname;
  size_t fname_len;
  uint32_t r;
  uint16_t e;
  int ha;
  int src;
  int dst;
  int rpc_sz;
  int sz;
  int n;

  /* sanity checks */
  assert(ctx != NULL);
  assert(ctx->nx != NULL);
  src = nexus_global_rank(ctx->nx);

  assert(pctx.len_plfsdir != 0);
  assert(pctx.plfsdir != NULL);
  assert(strncmp(fn, pctx.plfsdir, pctx.len_plfsdir) == 0);
  assert(fn != NULL);

  fname = fn + pctx.len_plfsdir + 1; /* remove parent path */
  assert(strlen(fname) < 256);
  fname_len = strlen(fname);
  assert(len < 256);

  if (nexus_global_size(ctx->nx) != 1) {
    if (IS_BYPASS_PLACEMENT(pctx.mode)) {
      dst =
          pdlfs::xxhash32(fname, strlen(fname), 0) % nexus_global_size(ctx->nx);
    } else {
      assert(ctx->ch != NULL);
      ch_placement_find_closest(
          ctx->ch, pdlfs::xxhash64(fname, strlen(fname), 0), 1, &target);
      dst = int(target);
    }
  } else {
    dst = src;
  }

  /* write trace if we are in testing mode */
  if (pctx.testin && pctx.logfd != -1) {
    ha = pdlfs::xxhash32(data, len, 0); /* data checksum */
    n = snprintf(msg, sizeof(msg),
                 "[SEND] %s %d bytes (e%d) r%d >> "
                 "r%d (hash=%08x)\n",
                 fn, int(len), epoch, src, dst, ha);

    n = write(pctx.logfd, msg, n);

    errno = 0;
  }

  sz = rpc_sz = 0;

  /* get an estimated size of the rpc */
  rpc_sz += 4;                 /* src rank */
  rpc_sz += 4;                 /* dst rank */
  rpc_sz += 1 + fname_len + 1; /* vpic fname */
  rpc_sz += 1 + len;           /* vpic data */
  rpc_sz += 2;                 /* epoch */
  assert(rpc_sz <= sizeof(buf));

  /* rank */
  r = htonl(src);
  memcpy(buf + sz, &r, 4);
  sz += 4;
  r = htonl(dst);
  memcpy(buf + sz, &r, 4);
  sz += 4;
  /* vpic fname */
  buf[sz] = static_cast<unsigned char>(fname_len);
  sz += 1;
  memcpy(buf + sz, fname, fname_len);
  sz += fname_len;
  buf[sz] = 0;
  sz += 1;
  /* vpic data */
  buf[sz] = static_cast<unsigned char>(len);
  sz += 1;
  memcpy(buf + sz, data, len);
  sz += len;
  /* epoch */
  e = htons(epoch);
  memcpy(buf + sz, &e, 2);
  sz += 2;
  assert(sz == rpc_sz);

  assert(ctx->sh != NULL);
  hret = shuffler_send(ctx->sh, dst, 0, buf, sz);

  if (hret != HG_SUCCESS) {
    rpc_abort("xxsend", hret);
  }

  return 0;
}

int shuffle_write(shuffle_ctx_t* ctx, const char* fn, char* d, size_t n,
                  int epoch) {
  if (ctx->type == SHUFFLE_3HOP) {
    assert(ctx != NULL);
    return _3h_shuffle_write(static_cast<_3h_ctx_t*>(ctx->rep), fn, d, n,
                             epoch);
  } else {
    return nn_shuffler_write(fn, d, n, epoch);
  }
}

void shuffle_finalize(shuffle_ctx_t* ctx) {
  char msg[200];
  unsigned long long accqsz;
  unsigned long long nps;
  int min_maxqsz;
  int max_maxqsz;
  int min_minqsz;
  int max_minqsz;
  if (ctx->type == SHUFFLE_3HOP) {
    // TODO
  } else {
    nn_shuffler_destroy();
    MPI_Reduce(&nnctx.accqsz, &accqsz, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(&nnctx.nps, &nps, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(&nnctx.maxqsz, &min_maxqsz, 1, MPI_INT, MPI_MIN, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(&nnctx.maxqsz, &max_maxqsz, 1, MPI_INT, MPI_MAX, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(&nnctx.minqsz, &min_minqsz, 1, MPI_INT, MPI_MIN, 0,
               MPI_COMM_WORLD);
    MPI_Reduce(&nnctx.minqsz, &max_minqsz, 1, MPI_INT, MPI_MAX, 0,
               MPI_COMM_WORLD);
    if (pctx.my_rank == 0 && nps != 0) {
      snprintf(msg, sizeof(msg),
               "[rpc] incoming queue depth: %.3f per rank\n"
               ">>> max: %d - %d, min: %d - %d",
               double(accqsz) / nps, min_maxqsz, max_maxqsz, min_minqsz,
               max_minqsz);
      info(msg);
    }
  }
}

static void _3h_shuffler_init_ch_placement(_3h_ctx_t* ctx) {
  char msg[100];
  const char* proto;
  const char* env;
  int rank; /* nx */
  int size; /* nx */
  int vf;

  assert(ctx->nx != NULL);

  rank = nexus_global_rank(ctx->nx);
  size = nexus_global_size(ctx->nx);

  if (pctx.paranoid_checks) {
    if (size != pctx.comm_sz || rank != pctx.my_rank) {
      msg_abort("nx-mpi disagree");
    }
  }

  if (!IS_BYPASS_PLACEMENT(pctx.mode)) {
    env = maybe_getenv("SHUFFLE_Virtual_factor");
    if (env == NULL) {
      vf = DEFAULT_VIRTUAL_FACTOR;
    } else {
      vf = atoi(env);
    }

    proto = maybe_getenv("SHUFFLE_Placement_protocol");
    if (proto == NULL) {
      proto = DEFAULT_PLACEMENT_PROTO;
    }

    ctx->ch = ch_placement_initialize(proto, size, vf /* vir factor */,
                                      0 /* hash seed */);
    if (ctx->ch == NULL) {
      msg_abort("ch_init");
    }
  }

  if (pctx.my_rank == 0) {
    if (!IS_BYPASS_PLACEMENT(pctx.mode)) {
      snprintf(msg, sizeof(msg),
               "ch-placement group size: %s (vir-factor: %s, proto: %s)",
               pretty_num(size).c_str(), pretty_num(vf).c_str(), proto);
      info(msg);
    } else {
      warn("ch-placement bypassed");
    }
  }
}

static void _3h_shuffler_init(_3h_ctx_t* ctx) {
  char rpc_name[] = "shuffle_rpc_write";
  const char* subnet;
  const char* proto;
  char msg[100];

  subnet = maybe_getenv("SHUFFLE_Subnet");
  if (subnet == NULL) {
    subnet = DEFAULT_SUBNET;
  }

  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "using subnet %s*", subnet);
    if (strcmp(subnet, "127.0.0.1") == 0) {
      warn(msg);
    } else {
      info(msg);
    }
  }

  proto = maybe_getenv("SHUFFLE_Mercury_proto");
  if (proto == NULL) {
    proto = DEFAULT_HG_PROTO;
  }
  if (pctx.my_rank == 0) {
    snprintf(msg, sizeof(msg), "using %s", proto);
    if (strstr(proto, "tcp") != NULL) {
      warn(msg);
    } else {
      info(msg);
    }
  }

  ctx->nx =
      nexus_bootstrap(const_cast<char*>(subnet), const_cast<char*>(proto));
  if (ctx->nx == NULL) {
    msg_abort("nexus_bootstrap");
  }

  _3h_shuffler_init_ch_placement(ctx);

  ctx->sh = shuffler_init(ctx->nx, rpc_name, 4, 4 << 10, 16, 32 << 10, 256,
                          _3h_shuffle_deliver);

  if (ctx->sh == NULL) {
    msg_abort("sh_init");
  }
}

void shuffle_init(shuffle_ctx_t* ctx) {
  char msg[200];
  int n;
  if (is_envset("SHUFFLE_Use_3hop")) {
    ctx->type = SHUFFLE_3HOP;
    if (pctx.my_rank == 0) {
      snprintf(msg, sizeof(msg), "using the scalable 3-hop shuffler");
      info(msg);
    }
  } else {
    ctx->type = SHUFFLE_NN;
    if (pctx.my_rank == 0) {
      snprintf(msg, sizeof(msg),
               "using the default NN shuffler: code might not scale well\n>>> "
               "switch to the 3-hop shuffler for better scalability");
      warn(msg);
    }
  }
  if (ctx->type == SHUFFLE_3HOP) {
    _3h_ctx_t* rep = static_cast<_3h_ctx_t*>(malloc(sizeof(_3h_ctx_t)));
    memset(rep, 0, sizeof(_3h_ctx_t));
    _3h_shuffler_init(rep);
    ctx->rep = rep;
  } else {
    nn_shuffler_init();
  }
  if (pctx.my_rank == 0) {
    n = 0;
    n += snprintf(msg + n, sizeof(msg) - n, "HG_HAS_POST_LIMIT is ");
#ifdef HG_HAS_POST_LIMIT
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    n += snprintf(msg + n, sizeof(msg) - n, ", HG_HAS_SELF_FORWARD is ");
#ifdef HG_HAS_SELF_FORWARD
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    n += snprintf(msg + n, sizeof(msg) - n, ", HG_HAS_EAGER_BULK is ");
#ifdef HG_HAS_EAGER_BULK
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    n += snprintf(msg + n, sizeof(msg) - n, "\n>>> HG_HAS_CHECKSUMS is ");
#ifdef HG_HAS_CHECKSUMS
    n += snprintf(msg + n, sizeof(msg) - n, "TRUE");
#else
    n += snprintf(msg + n, sizeof(msg) - n, "FALSE");
#endif
    info(msg);
  }
}

void shuffle_msg_sent(size_t n, void** arg1, void** arg2) {
  pctx.mctx.min_nms++;
  pctx.mctx.max_nms++;
  pctx.mctx.nms++;
}

void shuffle_msg_replied(void* arg1, void* arg2) {
  pctx.mctx.nmd++; /* delivered */
}

void shuffle_msg_received() {
  pctx.mctx.min_nmr++;
  pctx.mctx.max_nmr++;
  pctx.mctx.nmr++;
}
