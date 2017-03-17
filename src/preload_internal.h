/*
 * Copyright (c) 2016-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#pragma once

#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include <deltafs/deltafs_api.h>

#include "mon.h"

#include <set>

extern "C" {

#ifndef PRELOAD_MUTEX_LOCKING

typedef int maybe_mutex_t;
typedef int maybe_mutexattr_t;
static inline int maybe_mutex_lock(maybe_mutex_t* __mut) { return 0; }
static inline int maybe_mutex_unlock(maybe_mutex_t* __mut) { return 0; }
static inline int maybe_mutex_trylock(maybe_mutex_t* __mut) { return 0; }
static inline int maybe_mutex_init(maybe_mutex_t* __mut,
        maybe_mutexattr_t* __attr) { return 0; }
static inline int maybe_mutex_destroy(maybe_mutex_t* __mut) { return 0; }
#define MAYBE_MUTEX_INITIALIZER 0

#else

typedef pthread_mutex_t maybe_mutex_t;
typedef pthread_mutexattr_t maybe_mutexattr_t;
#define maybe_mutex_lock(__mut) pthread_mutex_lock(__mut)
#define maybe_mutex_unlock(__mut) pthread_mutex_unlock(__mut)
#define maybe_mutex_trylock(__mut) pthread_mutex_trylock(__mut)
#define maybe_mutex_init(__mut, __attr) pthread_mutex_init(__mut, __attr)
#define maybe_mutex_destroy(__mut) pthread_mutex_destroy(__mut)
#define MAYBE_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

#endif

/*
 * log: append message into a given file.
 */
static inline void log(int fd, const char* fmt, ...) {
    char tmp[500];
    va_list va;
    int n;
    va_start(va, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, va);
    n = write(fd, tmp, n);
    va_end(va);
    errno = 0;
}

#if defined(PRELOAD_TERM_COLOR)
#define ABORT "\033[0;31m!!!ABORT!!!\033[0m"
#define ERROR "\033[0;31m!!!ERROR!!!\033[0m"
#define WARNING "\033[0;33m!!!WARNING!!!\033[0m"
#define INFO "\033[0;32m-INFO-\033[0m"
#else
#define ABORT "!!!ABORT!!!"
#define ERROR "!!!ERROR!!!"
#define WARNING "!!!WARNING!!!"
#define INFO "-INFO-"
#endif

/*
 * info: print a message
 */
static inline void info(const char* msg) {
    log(fileno(stderr), INFO " %s\n", msg);
}

/*
 * warn: print a warning message
 */
static inline void warn(const char* msg) {
    log(fileno(stderr), WARNING " %s\n", msg);
}

/*
 * error: print an error message
 */
static inline void error(const char* msg) {
    if (errno != 0) {
        log(fileno(stderr), ERROR " %s: %s\n", msg,
                strerror(errno));
    } else {
        log(fileno(stderr), ERROR " %s\n", msg);
    }
}

/*
 * msg_abort: abort with a message
 */
static inline void msg_abort(const char *msg) {
    if (errno != 0) {
        log(fileno(stderr), ABORT " %s: %s\n", msg,
                strerror(errno));
    } else {
        log(fileno(stderr), ABORT " %s\n", msg);
    }

    abort();
}

static inline void must_maybelockmutex(maybe_mutex_t* __mut) {
    int r = maybe_mutex_lock(__mut);
    if (r != 0) {
        msg_abort("mtx_lock");
    }
}

static inline void must_maybeunlock(maybe_mutex_t* __mut) {
    int r = maybe_mutex_unlock(__mut);
    if (r != 0) {
        msg_abort("mtx_unlock");
    }
}

static inline const char* maybe_getenv(const char* key) {
    const char* env = getenv(key);
    errno = 0;
    return env;
}

static inline bool is_envset(const char* key) {
    const char* env = getenv(key);
    errno = 0;
    if (env == NULL) {
        return(false);
    } else if (strlen(env) == 0) {
        return(false);
    } else if (strlen(env) == 1 && strcmp(env, "0") == 0) {
        return(false);
    } else {
        return(true);
    }
}

/*
 * preload_write(fn, data, n):
 *   - ship data to deltafs
 */
extern int preload_write(const char* fn, char* data, size_t n, int epoch);

/*
 * preload context:
 *   - run-time state of the preload layer
 */
typedef struct preload_ctx {
    const char* deltafs_root;     /* deltafs root */
    size_t len_deltafs_root;      /* strlen */

    const char* local_root;       /* localfs root */
    size_t len_local_root;        /* strlen */

    int mode;    /* operating mode */
    int paranoid_barrier;   /* surround each epoch with barriers */
    int rank;    /* my MPI world rank */
    int size;    /* my MPI world size */

    const char* plfsdir;      /* path to the plfsdir */
    size_t len_plfsdir;       /* strlen */
    deltafs_plfsdir_t* plfsh; /* opaque handle to an opened plfsdir */
    int plfsfd;      /* fd for the plfsdir */

    std::set<FILE*>* isdeltafs;         /* open files owned by deltafs */

    int testin;      /* developer mode - for debug use only */
    int fake_data;   /* replace vpic output with fake data - for debug only */
    int nomon;       /* skip monitoring */
    int nodist;      /* skip copying mon files out */

    int logfd;       /* descriptor for the testing log file */
    int monfd;       /* descriptor for the mon dump file */

    int vmon;        /* verbose mon stats */
    int verr;        /* verbose error */

} preload_ctx_t;


extern preload_ctx_t pctx;

/*
 * trace: if in testing mode, append a message to the testing log file.
 */
static void inline trace(const char* msg) {
    if (pctx.testin) {
        if (pctx.logfd != -1) {
            log(pctx.logfd, "[T] %s\n", msg);
        }
    }
}

} // extern C
