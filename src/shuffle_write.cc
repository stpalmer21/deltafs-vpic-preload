/*
 * Copyright (c) 2017 Carnegie Mellon University.
 * George Amvrosiadis <gamvrosi@cs.cmu.edu>
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include <assert.h>
#include "shuffle.h"

struct write_bulk_args {
    hg_handle_t handle;
    size_t len;
    ssize_t ret;
    const char *fname;
    int rank_in;
};

static int shuffle_posix_write(const char *fn, char *data, int len)
{
    int fd, rv;
    ssize_t wrote;

    fd = open(fn, O_WRONLY|O_CREAT|O_APPEND, 0666);
    if (fd < 0) {
        if (sctx.testmode)
            fprintf(stderr, "shuffle_posix_write: %s: open failed (%s)\n", fn,
                    strerror(errno));
        return(EOF);
    }

    wrote = write(fd, data, len);
    if (wrote != len && sctx.testmode)
        fprintf(stderr, "shuffle_posix_write: %s: write failed: %d (want %d)\n",
                fn, (int)wrote, (int)len);

    rv = close(fd);
    if (rv < 0 && sctx.testmode)
        fprintf(stderr, "shuffle_posix_write: %s: close failed (%s)\n", fn,
                strerror(errno));

    return((wrote != len || rv < 0) ? EOF : 0);
}

static int shuffle_deltafs_write(const char *fn, char *data, int len)
{
    int fd, rv;
    ssize_t wrote;

    fd = deltafs_open(fn, O_WRONLY|O_CREAT|O_APPEND, 0666);
    if (fd < 0) {
        if (sctx.testmode)
            fprintf(stderr, "shuffle_deltafs_write: %s: open failed (%s)\n", fn,
                    strerror(errno));
        return(EOF);
    }

    wrote = deltafs_write(fd, data, len);
    if (wrote != len && sctx.testmode)
        fprintf(stderr, "shuffle_deltafs_write: %s: write failed: %d (want %d)\n",
                fn, (int)wrote, (int)len);

    rv = deltafs_close(fd);
    if (rv < 0 && sctx.testmode)
        fprintf(stderr, "shuffle_deltafs_write: %s: close failed (%s)\n", fn,
                strerror(errno));

    return((wrote != len || rv < 0) ? EOF : 0);
}

/*
 * shuffle_write_local(): write directly to deltafs or posix after shuffle.
 * If used for debugging we will print msg on any err.
 * Returns 0 or EOF on error.
 */
int shuffle_write_local(const char *fn, char *data, int len)
{
    char testpath[PATH_MAX];

    if (sctx.testmode &&
        snprintf(testpath, PATH_MAX, REDIRECT_TEST_ROOT "%s", fn) < 0)
        msg_abort("fclose:snprintf");

    switch (sctx.testmode) {
        case NO_TEST:
            return shuffle_deltafs_write(fn, data, len);
        case DELTAFS_NOPLFS_TEST:
            return shuffle_deltafs_write(testpath, data, len);
        case PRELOAD_TEST:
        case SHUFFLE_TEST:
        case PLACEMENT_TEST:
            return shuffle_posix_write(testpath, data, len);
    }
}

/* Mercury callback for bulk transfer requests */
static hg_return_t write_bulk_transfer_cb(const struct hg_cb_info *info)
{
    struct write_bulk_args *bulk_args = (struct write_bulk_args *)info->arg;
    hg_bulk_t data_handle = info->info.bulk.local_handle;
    hg_return_t hret = HG_SUCCESS;
    char *data;
    int ret, rank;
    char buf[1024] = { 0 };
    write_out_t out;

    /* Grab bulk data */
    hret = HG_Bulk_access(data_handle, 0, bulk_args->len, HG_BULK_READWRITE, 1,
                          (void **) &data, NULL, NULL);
    assert(hret == HG_SUCCESS);

    /* Get my rank */
    rank = ssg_get_rank(sctx.s);
    assert(rank != SSG_RANK_UNKNOWN && rank != SSG_EXTERNAL_RANK);

    fprintf(stderr, "Writing %d bytes to %s (shuffle: %d -> %d)\n",
            (int) bulk_args->len, bulk_args->fname, bulk_args->rank_in, rank);

    /* Perform the write */
    ret = shuffle_write_local(bulk_args->fname, data, (int) bulk_args->len);

    /* Write out to the log if we are running a test */
    if (sctx.testmode) {
        snprintf(buf, sizeof(buf), "source %5d target %5d size %d\n",
                 bulk_args->rank_in, rank, (int) bulk_args->len);
        assert(write(sctx.log, buf, strlen(buf)+1) > 0);
    }

    /* Fill output structure */
    out.ret = ret;

    /* Free block handle */
    hret = HG_Bulk_free(data_handle);
    assert(hret == HG_SUCCESS);

    /* Send response back */
    hret = HG_Respond(bulk_args->handle, NULL, NULL, &out);
    assert(hret == HG_SUCCESS);

    /* Clean up */
    HG_Destroy(bulk_args->handle);
    free(bulk_args);

    return hret;
}

/* Mercury RPC callback for redirected writes */
hg_return_t write_rpc_handler(hg_handle_t h)
{
    hg_return_t hret;
    write_in_t in;
    struct hg_info *info;
    hg_bulk_t in_handle, data_handle;
    int rank_in;
    struct write_bulk_args *bulk_args = NULL;

    bulk_args = (struct write_bulk_args *) malloc(
                    sizeof(struct write_bulk_args));
    assert(bulk_args);

    /* Keep handle to pass to callback */
    bulk_args->handle = h;

    /* Get info from handle */
    info = HG_Get_info(h);
    assert(info != NULL);

    /* Get input struct */
    hret = HG_Get_input(h, &in);
    assert(hret == HG_SUCCESS);

    in_handle = in.data_handle;

    /* Create a new block handle to read the data */
    bulk_args->len = HG_Bulk_get_size(in_handle);
    bulk_args->fname = in.fname;
    bulk_args->rank_in = in.rank_in;

    /* Create a new bulk handle to read the data */
    hret = HG_Bulk_create(info->hg_class, 1, NULL,
                          (hg_size_t *) &(bulk_args->len),
                          HG_BULK_READWRITE, &data_handle);
    assert(hret == HG_SUCCESS);

    /* Pull bulk data */
    hret = HG_Bulk_transfer(info->context, write_bulk_transfer_cb, bulk_args,
                           HG_BULK_PULL, info->addr, in_handle, 0,
                           data_handle, 0, bulk_args->len, HG_OP_ID_IGNORE);
    assert(hret == HG_SUCCESS);

    HG_Free_input(h, &in);
    return hret;
}

/* Redirects write to the right node through Mercury */
int shuffle_write(const char *fn, char *data, int len)
{
    write_in_t write_in;
    write_out_t write_out;
    hg_id_t write_id;
    hg_return_t hret;
    int rank, peer_rank;
    hg_addr_t peer_addr;
    hg_handle_t write_handle = HG_HANDLE_NULL;
    hg_request_t *hgreq;
    hg_bulk_t data_handle;
    unsigned int req_complete_flag = 0;
    int ret, write_ret;

    hret = HG_Register_data(sctx.hgcl, write_id, &sctx, NULL);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Register_data (write)");

    /* Decide RPC receiver. If we're alone we execute it locally. */
    if (ssg_get_count(sctx.s) == 1)
        return shuffle_write_local(fn, data, len);

    rank = ssg_get_rank(sctx.s);
    if (rank == SSG_RANK_UNKNOWN || rank == SSG_EXTERNAL_RANK)
        msg_abort("ssg_get_rank: bad rank");

    /* TODO: Currently sending to our neighbor. Use ch-placement instead */
    peer_rank = (rank + 1) % ssg_get_count(sctx.s);
    peer_addr = ssg_get_addr(sctx.s, peer_rank);
    if (peer_addr == HG_ADDR_NULL)
        msg_abort("ssg_get_addr");

    /* Put together write RPC */
    fprintf(stderr, "Redirecting write: %d -> %d\n", rank, peer_rank);
    hret = HG_Create(sctx.hgctx, peer_addr, write_id, &write_handle);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Create");

    hgreq = hg_request_create(sctx.hgreqcl);
    if (hgreq == NULL)
        msg_abort("hg_request_create (write)");

    /* TODO: Currently using bulk transfers only.
             Check whether we need to use bulk or point-to-point */
    hret = HG_Bulk_create(sctx.hgcl, 1, (void **) &data, (hg_size_t *) &len,
                          HG_BULK_READ_ONLY, &data_handle);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Bulk_create");

    write_in.fname = fn;
    write_in.data_handle = data_handle;
    write_in.rank_in = rank;

    /* Send off write RPC */
    hret = HG_Forward(write_handle, &hg_request_complete_cb, hgreq, &write_in);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Forward");

    /* Receive reply and return it */
    ret = hg_request_wait(hgreq, HG_MAX_IDLE_TIME, &req_complete_flag);
    if (ret == HG_UTIL_FAIL)
        msg_abort("write failed");
    if (req_complete_flag == 0)
        msg_abort("write timed out");

    hret = HG_Get_output(write_handle, &write_out);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Get_output");

    write_ret = (int) write_out.ret;

    hret = HG_Free_output(write_handle, &write_out);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Free_output");

    hret = HG_Destroy(write_handle);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Destroy");

    hg_request_destroy(hgreq);

    /* TODO: Currently need to free bulk resources every time.
             Check whether we are using bulk or point-to-point */
    hret = HG_Bulk_free(data_handle);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Bulk_free");

    return write_ret;
}