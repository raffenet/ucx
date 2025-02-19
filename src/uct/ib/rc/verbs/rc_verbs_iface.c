/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "rc_verbs.h"

#include <uct/api/uct.h>
#include <uct/ib/base/ib_device.h>
#include <uct/ib/base/ib_log.h>
#include <uct/base/uct_pd.h>
#include <ucs/arch/bitops.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <string.h>


ucs_config_field_t uct_rc_verbs_iface_config_table[] = {
  {"RC_", "", NULL,
   ucs_offsetof(uct_rc_verbs_iface_config_t, super), UCS_CONFIG_TYPE_TABLE(uct_rc_iface_config_table)},

  {"MAX_AM_HDR", "128",
   "Buffer size to reserve for active message headers. If set to 0, the transport will\n"
   "not support zero-copy active messages.",
   ucs_offsetof(uct_rc_verbs_iface_config_t, max_am_hdr), UCS_CONFIG_TYPE_MEMUNITS},

  {NULL}
};

static uct_ib_iface_ops_t uct_rc_verbs_iface_ops;

static UCS_F_NOINLINE unsigned
uct_rc_verbs_iface_post_recv_always(uct_rc_verbs_iface_t *iface, unsigned max)
{
    struct ibv_recv_wr *bad_wr;
    uct_ib_recv_wr_t *wrs;
    unsigned count;
    int ret;

    wrs  = ucs_alloca(sizeof *wrs  * max);

    count = uct_ib_iface_prepare_rx_wrs(&iface->super.super, &iface->super.rx.mp,
                                        wrs, max);
    if (count == 0) {
        return 0;
    }

    UCT_IB_INSTRUMENT_RECORD_RECV_WR_LEN("uct_rc_verbs_iface_post_recv_always",
                                         &wrs[0].ibwr);
    ret = ibv_post_srq_recv(iface->super.rx.srq, &wrs[0].ibwr, &bad_wr);
    if (ret != 0) {
        ucs_fatal("ibv_post_srq_recv() returned %d: %m", ret);
    }
    iface->super.rx.available -= count;

    return count;
}

static inline unsigned uct_rc_verbs_iface_post_recv(uct_rc_verbs_iface_t *iface,
                                                    int fill)
{
    unsigned batch = iface->super.super.config.rx_max_batch;
    unsigned count;

    if (iface->super.rx.available < batch) {
        if (!fill) {
            return 0;
        } else {
            count = iface->super.rx.available;
        }
    } else {
        count = batch;
    }

    return uct_rc_verbs_iface_post_recv_always(iface, count);
}

static UCS_F_ALWAYS_INLINE void 
uct_rc_verbs_iface_poll_tx(uct_rc_verbs_iface_t *iface)
{
    uct_rc_verbs_ep_t *ep;
    unsigned count;
    int i, ret;
    unsigned num_wcs = iface->super.super.config.tx_max_poll;
    struct ibv_wc wc[num_wcs];

    ret = ibv_poll_cq(iface->super.super.send_cq, num_wcs, wc);
    if (ucs_unlikely(ret <= 0)) {
        if (ucs_unlikely(ret < 0)) {
            ucs_fatal("Failed to poll send CQ");
        }
        return;
    }

    for (i = 0; i < ret; ++i) {
        if (ucs_unlikely(wc[i].status != IBV_WC_SUCCESS)) {
            ucs_fatal("Send completion with error: %s", ibv_wc_status_str(wc[i].status));
        }

        UCS_STATS_UPDATE_COUNTER(iface->super.stats, UCT_RC_IFACE_STAT_TX_COMPLETION, 1);

        ep = ucs_derived_of(uct_rc_iface_lookup_ep(&iface->super, wc[i].qp_num),
                            uct_rc_verbs_ep_t);
        ucs_assert(ep != NULL);

        count = wc[i].wr_id + 1; /* Number of sends with WC completes in batch */
        ep->super.available         += count;
        ep->tx.completion_count     += count;
        ++iface->super.tx.cq_available;

        uct_rc_ep_process_tx_completion(&iface->super, &ep->super,
                                        ep->tx.completion_count);
    }
}

static UCS_F_ALWAYS_INLINE ucs_status_t 
uct_rc_verbs_iface_poll_rx(uct_rc_verbs_iface_t *iface)
{
    uct_ib_iface_recv_desc_t *desc;
    uct_rc_hdr_t *hdr;
    int i, ret;
    ucs_status_t status;
    unsigned num_wcs = iface->super.super.config.rx_max_poll;
    struct ibv_wc wc[num_wcs];

    ret = ibv_poll_cq(iface->super.super.recv_cq, num_wcs, wc);
    if (ret > 0) {
        for (i = 0; i < ret; ++i) {
            if (ucs_unlikely(wc[i].status != IBV_WC_SUCCESS)) {
                ucs_fatal("Receive completion with error: %s", ibv_wc_status_str(wc[i].status));
            }

            UCS_STATS_UPDATE_COUNTER(iface->super.stats, UCT_RC_IFACE_STAT_RX_COMPLETION, 1);

            desc = (void*)wc[i].wr_id;
            hdr = uct_ib_iface_recv_desc_hdr(&iface->super.super, desc);
            VALGRIND_MAKE_MEM_DEFINED(hdr, wc[i].byte_len);

            UCS_INSTRUMENT_RECORD(UCS_INSTRUMENT_TYPE_IB_RX,
                                  "uct_rc_verbs_iface_poll_rx",
                                  wc[i].wr_id, wc[i].status);
            uct_ib_log_recv_completion(&iface->super.super, IBV_QPT_RC, &wc[i],
                                       hdr, uct_rc_ep_am_packet_dump);
            uct_ib_iface_invoke_am(&iface->super.super, hdr->am_id, hdr + 1,
                                   wc[i].byte_len - sizeof(*hdr), desc);
        }

        iface->super.rx.available += ret;
        status = UCS_OK;
    } else if (ret == 0) {
        status = UCS_ERR_NO_PROGRESS;
    } else {
        ucs_fatal("Failed to poll receive CQ");
    }
    uct_rc_verbs_iface_post_recv(iface, 0);
    return status;
}

void uct_rc_verbs_iface_progress(void *arg)
{
    uct_rc_verbs_iface_t *iface = arg;
    ucs_status_t status;

    status = uct_rc_verbs_iface_poll_rx(iface);
    if (status == UCS_ERR_NO_PROGRESS) {
        uct_rc_verbs_iface_poll_tx(iface);
    }
}

static inline int
uct_rc_verbs_is_ext_atomic_supported(struct ibv_exp_device_attr *dev_attr,
                                           size_t atomic_size)
{
#ifdef HAVE_IB_EXT_ATOMICS
    struct ibv_exp_ext_atomics_params ext_atom = dev_attr->ext_atom;
    return (ext_atom.log_max_atomic_inline >= ucs_ilog2(atomic_size)) &&
           (ext_atom.log_atomic_arg_sizes & atomic_size);
#else
      return 0;
#endif
}

static ucs_status_t uct_rc_verbs_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_rc_verbs_iface_t *iface = ucs_derived_of(tl_iface, uct_rc_verbs_iface_t);
    struct ibv_exp_device_attr *dev_attr =
                    &uct_ib_iface_device(&iface->super.super)->dev_attr;

    uct_rc_iface_query(&iface->super, iface_attr);

    /* PUT */
    iface_attr->cap.put.max_short = iface->config.max_inline;
    iface_attr->cap.put.max_bcopy = iface->super.super.config.seg_size;
    iface_attr->cap.put.max_zcopy =
                        uct_ib_iface_port_attr(&iface->super.super)->max_msg_sz;

    /* GET */
    iface_attr->cap.get.max_bcopy = iface->super.super.config.seg_size;
    iface_attr->cap.get.max_zcopy =
                    uct_ib_iface_port_attr(&iface->super.super)->max_msg_sz;

    /* AM */
    iface_attr->cap.am.max_short  = iface->config.max_inline
                                        - sizeof(uct_rc_hdr_t);
    iface_attr->cap.am.max_bcopy  = iface->super.super.config.seg_size
                                        - sizeof(uct_rc_hdr_t);

    iface_attr->cap.am.max_zcopy  = iface->super.super.config.seg_size
                                            - sizeof(uct_rc_hdr_t);
    iface_attr->cap.am.max_hdr    = iface->config.short_desc_size
                                            - sizeof(uct_rc_hdr_t);

    /*
     * Atomics.
     * Need to make sure device support at least one kind of atomics.
     */
    if (IBV_EXP_HAVE_ATOMIC_HCA(dev_attr) ||
        IBV_EXP_HAVE_ATOMIC_GLOB(dev_attr) ||
        IBV_EXP_HAVE_ATOMIC_HCA_REPLY_BE(dev_attr))
    {
        iface_attr->cap.flags |= UCT_IFACE_FLAG_ATOMIC_ADD64 |
                                 UCT_IFACE_FLAG_ATOMIC_FADD64 |
                                 UCT_IFACE_FLAG_ATOMIC_CSWAP64;

        if (uct_rc_verbs_is_ext_atomic_supported(dev_attr, sizeof(uint32_t))) {
            iface_attr->cap.flags |= UCT_IFACE_FLAG_ATOMIC_ADD32 |
                                     UCT_IFACE_FLAG_ATOMIC_FADD32 |
                                     UCT_IFACE_FLAG_ATOMIC_SWAP32 |
                                     UCT_IFACE_FLAG_ATOMIC_CSWAP32;
        }

        if (uct_rc_verbs_is_ext_atomic_supported(dev_attr, sizeof(uint64_t))) {
            iface_attr->cap.flags |= UCT_IFACE_FLAG_ATOMIC_SWAP64;
        }
    }

    /* Software overhead */
    iface_attr->overhead = 75e-9;

    return UCS_OK;
}

static UCS_CLASS_INIT_FUNC(uct_rc_verbs_iface_t, uct_pd_h pd, uct_worker_h worker,
                           const char *dev_name, size_t rx_headroom,
                           const uct_iface_config_t *tl_config)
{
    uct_rc_verbs_iface_config_t *config =
                    ucs_derived_of(tl_config, uct_rc_verbs_iface_config_t);
    struct ibv_exp_device_attr *dev_attr;
    size_t am_hdr_size;
    ucs_status_t status;
    struct ibv_qp_cap cap;
    struct ibv_qp *qp;

    UCS_CLASS_CALL_SUPER_INIT(uct_rc_iface_t, &uct_rc_verbs_iface_ops, pd,
                              worker, dev_name, rx_headroom, 0, &config->super);

    /* Initialize inline work request */
    memset(&self->inl_am_wr, 0, sizeof(self->inl_am_wr));
    self->inl_am_wr.sg_list                 = self->inl_sge;
    self->inl_am_wr.num_sge                 = 2;
    self->inl_am_wr.opcode                  = IBV_WR_SEND;
    self->inl_am_wr.send_flags              = IBV_SEND_INLINE;

    memset(&self->inl_rwrite_wr, 0, sizeof(self->inl_rwrite_wr));
    self->inl_rwrite_wr.sg_list             = self->inl_sge;
    self->inl_rwrite_wr.num_sge             = 1;
    self->inl_rwrite_wr.opcode              = IBV_WR_RDMA_WRITE;
    self->inl_rwrite_wr.send_flags          = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

    memset(self->inl_sge, 0, sizeof(self->inl_sge));

    /* Configuration */
    am_hdr_size = ucs_max(config->max_am_hdr, sizeof(uct_rc_hdr_t));
    self->config.short_desc_size = ucs_max(UCT_RC_MAX_ATOMIC_SIZE, am_hdr_size);
    dev_attr = &uct_ib_iface_device(&self->super.super)->dev_attr;
    if (IBV_EXP_HAVE_ATOMIC_HCA(dev_attr) || IBV_EXP_HAVE_ATOMIC_GLOB(dev_attr)) {
        self->config.atomic32_handler = uct_rc_ep_atomic_handler_32_be0;
        self->config.atomic64_handler = uct_rc_ep_atomic_handler_64_be0;
    } else if (IBV_EXP_HAVE_ATOMIC_HCA_REPLY_BE(dev_attr)) {
        self->config.atomic32_handler = uct_rc_ep_atomic_handler_32_be1;
        self->config.atomic64_handler = uct_rc_ep_atomic_handler_64_be1;
    }

    /* Create a dummy QP in order to find out max_inline */
    status = uct_rc_iface_qp_create(&self->super, &qp, &cap);
    if (status != UCS_OK) {
        goto err;
    }
    ibv_destroy_qp(qp);
    self->config.max_inline = cap.max_inline_data;

    /* Create AH headers and Atomic mempool */
    status = uct_iface_mpool_init(&self->super.super.super,
                                  &self->short_desc_mp,
                                  sizeof(uct_rc_iface_send_desc_t) +
                                      self->config.short_desc_size,
                                  sizeof(uct_rc_iface_send_desc_t),
                                  UCS_SYS_CACHE_LINE_SIZE,
                                  &config->super.super.tx.mp,
                                  self->super.config.tx_qp_len,
                                  uct_rc_iface_send_desc_init,
                                  "rc_verbs_short_desc");
    if (status != UCS_OK) {
        goto err;
    }

    while (self->super.rx.available > 0) {
        if (uct_rc_verbs_iface_post_recv(self, 1) == 0) {
            ucs_error("failed to post receives");
            status = UCS_ERR_NO_MEMORY;
            goto err_destroy_short_desc_mp;
        }
    }

    return UCS_OK;

err_destroy_short_desc_mp:
    ucs_mpool_cleanup(&self->short_desc_mp, 1);
err:
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_rc_verbs_iface_t)
{
    ucs_mpool_cleanup(&self->short_desc_mp, 1);
}

UCS_CLASS_DEFINE(uct_rc_verbs_iface_t, uct_rc_iface_t);
static UCS_CLASS_DEFINE_NEW_FUNC(uct_rc_verbs_iface_t, uct_iface_t, uct_pd_h,
                                 uct_worker_h, const char*, size_t,
                                 const uct_iface_config_t*);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_rc_verbs_iface_t, uct_iface_t);


static uct_ib_iface_ops_t uct_rc_verbs_iface_ops = {
    {
    .iface_query              = uct_rc_verbs_iface_query,
    .iface_flush              = uct_rc_iface_flush,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_rc_verbs_iface_t),
    .iface_release_am_desc    = uct_ib_iface_release_am_desc,
    .iface_wakeup_open        = uct_ib_iface_wakeup_open,
    .iface_wakeup_get_fd      = uct_ib_iface_wakeup_get_fd,
    .iface_wakeup_arm         = uct_ib_iface_wakeup_arm,
    .iface_wakeup_wait        = uct_ib_iface_wakeup_wait,
    .iface_wakeup_signal      = uct_ib_iface_wakeup_signal,
    .iface_wakeup_close       = uct_ib_iface_wakeup_close,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_rc_verbs_ep_t),
    .ep_get_address           = uct_rc_ep_get_address,
    .ep_connect_to_ep         = uct_rc_ep_connect_to_ep,
    .iface_get_device_address = uct_ib_iface_get_device_address,
    .iface_is_reachable       = uct_ib_iface_is_reachable,
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_rc_verbs_ep_t),
    .ep_am_short              = uct_rc_verbs_ep_am_short,
    .ep_am_bcopy              = uct_rc_verbs_ep_am_bcopy,
    .ep_am_zcopy              = uct_rc_verbs_ep_am_zcopy,
    .ep_put_short             = uct_rc_verbs_ep_put_short,
    .ep_put_bcopy             = uct_rc_verbs_ep_put_bcopy,
    .ep_put_zcopy             = uct_rc_verbs_ep_put_zcopy,
    .ep_get_bcopy             = uct_rc_verbs_ep_get_bcopy,
    .ep_get_zcopy             = uct_rc_verbs_ep_get_zcopy,
    .ep_atomic_add64          = uct_rc_verbs_ep_atomic_add64,
    .ep_atomic_fadd64         = uct_rc_verbs_ep_atomic_fadd64,
    .ep_atomic_swap64         = uct_rc_verbs_ep_atomic_swap64,
    .ep_atomic_cswap64        = uct_rc_verbs_ep_atomic_cswap64,
    .ep_atomic_add32          = uct_rc_verbs_ep_atomic_add32,
    .ep_atomic_fadd32         = uct_rc_verbs_ep_atomic_fadd32,
    .ep_atomic_swap32         = uct_rc_verbs_ep_atomic_swap32,
    .ep_atomic_cswap32        = uct_rc_verbs_ep_atomic_cswap32,
    .ep_pending_add           = uct_rc_ep_pending_add,
    .ep_pending_purge         = uct_rc_ep_pending_purge,
    .ep_flush                 = uct_rc_verbs_ep_flush
    },
    .arm_tx_cq                = uct_ib_iface_arm_tx_cq,
    .arm_rx_cq                = uct_ib_iface_arm_rx_cq,
};

static ucs_status_t uct_rc_verbs_query_resources(uct_pd_h pd,
                                                 uct_tl_resource_desc_t **resources_p,
                                                 unsigned *num_resources_p)
{
    return uct_ib_device_query_tl_resources(&ucs_derived_of(pd, uct_ib_pd_t)->dev,
                                            "rc", 0,
                                            resources_p, num_resources_p);
}

UCT_TL_COMPONENT_DEFINE(uct_rc_verbs_tl,
                        uct_rc_verbs_query_resources,
                        uct_rc_verbs_iface_t,
                        "rc",
                        "RC_VERBS_",
                        uct_rc_verbs_iface_config_table,
                        uct_rc_verbs_iface_config_t);
UCT_PD_REGISTER_TL(&uct_ib_pdc, &uct_rc_verbs_tl);
