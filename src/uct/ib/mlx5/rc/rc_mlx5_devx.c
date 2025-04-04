/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2019. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rc_mlx5.inl"

#include <uct/api/uct.h>
#include <ucs/arch/bitops.h>
#include <ucs/async/async.h>
#include <uct/ib/rc/base/rc_iface.h>


ucs_status_t uct_rc_mlx5_devx_iface_subscribe_event(
        uct_rc_mlx5_iface_common_t *iface,
        struct mlx5dv_devx_event_channel *event_channel,
        struct mlx5dv_devx_obj *obj, uint16_t event, uint64_t cookie,
        char *msg_arg)
{
    uct_ib_md_t *md = uct_ib_iface_md(&iface->super.super);
    int ret;

    if (event_channel == NULL) {
        return UCS_OK;
    }

    ret = mlx5dv_devx_subscribe_devx_event(event_channel, obj, sizeof(event),
                                           &event, cookie);
    if (ret) {
        ucs_error("mlx5dv_devx_subscribe_devx_event(%s) failed on %s: %m",
                  msg_arg, ibv_get_device_name(md->dev.ibv_context->device));
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}

static void
uct_rc_mlx5_devx_iface_event_handler(int fd, ucs_event_set_types_t events,
                                     void *arg)
{
    uct_rc_mlx5_iface_common_t *iface = arg;
    uct_ib_md_t *md                   = uct_ib_iface_md(&iface->super.super);
    struct mlx5dv_devx_async_event_hdr devx_event;
    uct_ib_async_event_t event;
    int ret;

    ret = mlx5dv_devx_get_event(iface->event_channel, &devx_event, sizeof(devx_event));
    if (ret < 0) {
        if (errno != EAGAIN) {
            ucs_warn("mlx5dv_devx_get_event(QP) failed on %s: %m",
                     ibv_get_device_name(md->dev.ibv_context->device));
        }
        return;
    }

    event.event_type = (enum ibv_event_type)(devx_event.cookie &
                                             UCT_IB_MLX5_DEVX_EVENT_TYPE_MASK);
    switch (event.event_type) {
    case IBV_EVENT_QP_LAST_WQE_REACHED:
        event.qp_num = devx_event.cookie >> UCT_IB_MLX5_DEVX_EVENT_DATA_SHIFT;
        break;
    default:
        ucs_warn("unexpected async event: %d", event.event_type);
        return;
    }

    uct_ib_handle_async_event(&md->dev, &event);
}

ucs_status_t uct_rc_mlx5_iface_devx_pre_arm(uct_rc_mlx5_iface_common_t *iface)
{
    uct_ib_md_t *md = uct_ib_iface_md(&iface->super.super);
    ucs_status_t status;
    struct mlx5dv_devx_async_event_hdr event;
    int ret;

    status = UCS_OK;
    for (;;) {
        ret = mlx5dv_devx_get_event(iface->cq_event_channel, &event,
                                    sizeof(event));
        if (ret < 0) {
            break;
        }

        iface->super.super.ops->event_cq(&iface->super.super, event.cookie);
        status = UCS_ERR_BUSY;
    }

    if (errno != EAGAIN) {
        ucs_warn("mlx5dv_devx_get_event(CQ) failed on %s: %m",
                 ibv_get_device_name(md->dev.ibv_context->device));
        status = UCS_ERR_IO_ERROR;
    }

    return status;
}

ucs_status_t
uct_rc_mlx5_iface_devx_arm(uct_rc_mlx5_iface_common_t *iface, unsigned events)
{
    int solicited[UCT_IB_DIR_LAST], dir;
    ucs_status_t status;
    uint64_t dirs;

    status = uct_rc_mlx5_iface_devx_pre_arm(iface);
    if (status != UCS_OK) {
        return status;
    }

    dirs = uct_rc_iface_arm_cq_check(&iface->super, events, solicited);
    ucs_for_each_bit(dir, dirs) {
        ucs_assert(dir < UCT_IB_DIR_LAST);
        uct_ib_mlx5dv_arm_cq(&iface->cq[dir], solicited[dir]);
    }

    return UCS_OK;
}

static ucs_status_t uct_rc_mlx5_devx_create_event_channel(
        uct_rc_mlx5_iface_common_t *iface,
        struct mlx5dv_devx_event_channel **event_channel_p)
{
    uct_ib_mlx5_md_t *md = uct_ib_mlx5_iface_md(&iface->super.super);
    struct mlx5dv_devx_event_channel *event_channel;
    ucs_status_t status;

    event_channel = mlx5dv_devx_create_event_channel(
            md->super.dev.ibv_context,
            MLX5_IB_UAPI_DEVX_CR_EV_CH_FLAGS_OMIT_DATA);

    if (event_channel == NULL) {
        ucs_error("mlx5dv_devx_create_event_channel() failed: %m");
        status = UCS_ERR_IO_ERROR;
        goto err;
    }

    status = ucs_sys_fcntl_modfl(event_channel->fd, O_NONBLOCK, 0);
    if (status != UCS_OK) {
        goto err_destroy_channel;
    }

    *event_channel_p = event_channel;
    return UCS_OK;

err_destroy_channel:
    mlx5dv_devx_destroy_event_channel(event_channel);
err:
    return status;
}

ucs_status_t
uct_rc_mlx5_devx_iface_init_events(uct_rc_mlx5_iface_common_t *iface)
{
    uct_ib_mlx5_md_t *md = uct_ib_mlx5_iface_md(&iface->super.super);
    ucs_status_t status;

    iface->event_channel    = NULL;
    iface->cq_event_channel = NULL;

    if (md->super.dev.async_events) {
        status = uct_rc_mlx5_devx_create_event_channel(iface,
                                                       &iface->event_channel);
        if (status != UCS_OK) {
            return status;
        }

        status = ucs_async_set_event_handler(
                iface->super.super.super.worker->async->mode,
                iface->event_channel->fd, UCS_EVENT_SET_EVREAD,
                uct_rc_mlx5_devx_iface_event_handler, iface,
                iface->super.super.super.worker->async);
        if (status != UCS_OK) {
            goto err_destroy_channel;
        }
    }

    if (md->flags & UCT_IB_MLX5_MD_FLAG_DEVX_CQ) {
        status = uct_rc_mlx5_devx_create_event_channel(iface,
                &iface->cq_event_channel);
        if (status != UCS_OK) {
            goto err_free_events;
        }
    }

    return UCS_OK;

err_free_events:
    if (iface->event_channel != NULL) {
        ucs_async_remove_handler(iface->event_channel->fd, 1);
    }

err_destroy_channel:
    if (iface->event_channel != NULL) {
        mlx5dv_devx_destroy_event_channel(iface->event_channel);
    }

    return status;
}

void uct_rc_mlx5_devx_iface_free_events(uct_rc_mlx5_iface_common_t *iface)
{
    if (iface->event_channel != NULL) {
        ucs_async_remove_handler(iface->event_channel->fd, 1);
        mlx5dv_devx_destroy_event_channel(iface->event_channel);
    }

    if (iface->cq_event_channel != NULL) {
        mlx5dv_devx_destroy_event_channel(iface->cq_event_channel);
    }
}

static ucs_status_t
uct_rc_mlx5_devx_init_rx_common(uct_rc_mlx5_iface_common_t *iface,
                                uct_ib_mlx5_md_t *md,
                                const uct_rc_iface_common_config_t *config,
                                void *wq)
{
    ucs_status_t status  = UCS_ERR_NO_MEMORY;
    int len, max, stride, log_num_of_strides, wq_type;

    stride = uct_ib_mlx5_srq_stride(iface->tm.mp.num_strides);
    max    = uct_ib_mlx5_srq_max_wrs(config->super.rx.queue_len,
                                     iface->tm.mp.num_strides);
    max    = ucs_roundup_pow2(max);
    len    = max * stride;

    status = uct_ib_mlx5_md_buf_alloc(md, len, 0, &iface->rx.srq.buf,
                                      &iface->rx.srq.devx.mem, 0, "srq buf");
    if (status != UCS_OK) {
        return status;
    }

    iface->rx.srq.devx.dbrec = uct_ib_mlx5_get_dbrec(md);
    if (!iface->rx.srq.devx.dbrec) {
        goto err_free_mem;
    }

    iface->rx.srq.db = &iface->rx.srq.devx.dbrec->db[MLX5_RCV_DBR];

    if (iface->config.srq_topo == UCT_RC_MLX5_SRQ_TOPO_CYCLIC) {
        wq_type = UCT_RC_MLX5_MP_ENABLED(iface) ?
                  UCT_IB_MLX5_SRQ_TOPO_CYCLIC_MP_RQ :
                  UCT_IB_MLX5_SRQ_TOPO_CYCLIC;
    } else {
        wq_type = UCT_RC_MLX5_MP_ENABLED(iface) ?
                  UCT_IB_MLX5_SRQ_TOPO_LIST_MP_RQ :
                  UCT_IB_MLX5_SRQ_TOPO_LIST;
    }

    UCT_IB_MLX5DV_SET  (wq, wq, wq_type,       wq_type);
    UCT_IB_MLX5DV_SET  (wq, wq, log_wq_sz,     ucs_ilog2(max));
    UCT_IB_MLX5DV_SET  (wq, wq, log_wq_stride, ucs_ilog2(stride));
    UCT_IB_MLX5DV_SET  (wq, wq, pd,            uct_ib_mlx5_devx_md_get_pdn(md));
    UCT_IB_MLX5DV_SET  (wq, wq, dbr_umem_id,   iface->rx.srq.devx.dbrec->mem_id);
    UCT_IB_MLX5DV_SET64(wq, wq, dbr_addr,      iface->rx.srq.devx.dbrec->offset);
    UCT_IB_MLX5DV_SET  (wq, wq, wq_umem_id,    iface->rx.srq.devx.mem.mem->umem_id);

    if (UCT_RC_MLX5_MP_ENABLED(iface)) {
        /* Normalize to device's interface values (range of (-6) - 7) */
        /* cppcheck-suppress internalAstError */
        log_num_of_strides = ucs_ilog2(iface->tm.mp.num_strides) - 9;

        UCT_IB_MLX5DV_SET(wq, wq, log_wqe_num_of_strides,
                          log_num_of_strides & 0xF);
        UCT_IB_MLX5DV_SET(wq, wq, log_wqe_stride_size,
                          (ucs_ilog2(iface->super.super.config.seg_size) - 6));
    }

    iface->rx.srq.type = UCT_IB_MLX5_OBJ_TYPE_DEVX;
    uct_ib_mlx5_srq_buff_init(&iface->rx.srq, 0, max - 1,
                              iface->super.super.config.seg_size,
                              iface->tm.mp.num_strides);
    iface->super.rx.srq.quota = max - 1;

    return UCS_OK;

err_free_mem:
    uct_ib_mlx5_md_buf_free(md, iface->rx.srq.buf, &iface->rx.srq.devx.mem);
    return status;
}

#if IBV_HW_TM
ucs_status_t
uct_rc_mlx5_devx_init_rx_tm(uct_rc_mlx5_iface_common_t *iface,
                            const uct_rc_iface_common_config_t *config,
                            int dc, unsigned rndv_hdr_len)
{
    uct_ib_mlx5_md_t *md = uct_ib_mlx5_iface_md(&iface->super.super);
    uct_ib_device_t *dev = &md->super.dev;
    char in[UCT_IB_MLX5DV_ST_SZ_BYTES(create_xrq_in)]   = {};
    char out[UCT_IB_MLX5DV_ST_SZ_BYTES(create_xrq_out)] = {};
    ucs_status_t status;
    void *xrqc;

    uct_rc_mlx5_init_rx_tm_common(iface, config, rndv_hdr_len);

    UCT_IB_MLX5DV_SET(create_xrq_in, in, opcode, UCT_IB_MLX5_CMD_OP_CREATE_XRQ);
    xrqc = UCT_IB_MLX5DV_ADDR_OF(create_xrq_in, in, xrq_context);

    UCT_IB_MLX5DV_SET(xrqc, xrqc, topology, UCT_IB_MLX5_XRQC_TOPOLOGY_TAG_MATCHING);
    UCT_IB_MLX5DV_SET(xrqc, xrqc, offload,  UCT_IB_MLX5_XRQC_OFFLOAD_RNDV);
    UCT_IB_MLX5DV_SET(xrqc, xrqc, tag_matching_topology_context.log_matching_list_sz,
                                  ucs_ilog2(iface->tm.num_tags) + 1);
    UCT_IB_MLX5DV_SET(xrqc, xrqc, dc,       dc);
    UCT_IB_MLX5DV_SET(xrqc, xrqc, cqn,      iface->cq[UCT_IB_DIR_RX].cq_num);

    status = uct_rc_mlx5_devx_init_rx_common(iface, md, config,
                                             UCT_IB_MLX5DV_ADDR_OF(xrqc, xrqc, wq));
    if (status != UCS_OK) {
        return UCS_OK;
    }

    iface->rx.srq.devx.obj = uct_ib_mlx5_devx_obj_create(dev->ibv_context, in,
                                                         sizeof(in), out,
                                                         sizeof(out), "XRQ",
                                                         UCS_LOG_LEVEL_ERROR);
    if (iface->rx.srq.devx.obj == NULL) {
        status = UCS_ERR_IO_ERROR;
        goto err_cleanup_srq;
    }

    iface->rx.srq.srq_num = UCT_IB_MLX5DV_GET(create_xrq_out, out, xrqn);
    uct_rc_mlx5_iface_tm_set_cmd_qp_len(iface);

    return UCS_OK;

err_cleanup_srq:
    uct_rc_mlx5_devx_cleanup_srq(md, &iface->rx.srq);
    return status;
}
#endif

ucs_status_t uct_rc_mlx5_devx_init_rx(uct_rc_mlx5_iface_common_t *iface,
                                      const uct_rc_iface_common_config_t *config)
{
    uct_ib_mlx5_md_t *md = uct_ib_mlx5_iface_md(&iface->super.super);
    uct_ib_device_t *dev = &md->super.dev;
    char in[UCT_IB_MLX5DV_ST_SZ_BYTES(create_rmp_in)]   = {};
    char out[UCT_IB_MLX5DV_ST_SZ_BYTES(create_rmp_out)] = {};
    ucs_status_t status;
    void *rmpc;

    UCT_IB_MLX5DV_SET(create_rmp_in, in, opcode, UCT_IB_MLX5_CMD_OP_CREATE_RMP);
    rmpc = UCT_IB_MLX5DV_ADDR_OF(create_rmp_in, in, rmp_context);

    UCT_IB_MLX5DV_SET(rmpc, rmpc, state, UCT_IB_MLX5_RMPC_STATE_RDY);

    status = uct_rc_mlx5_devx_init_rx_common(iface, md, config,
                                             UCT_IB_MLX5DV_ADDR_OF(rmpc, rmpc, wq));
    if (status != UCS_OK) {
        return status;
    }

    iface->rx.srq.devx.obj = uct_ib_mlx5_devx_obj_create(dev->ibv_context, in,
                                                         sizeof(in), out,
                                                         sizeof(out), "RMP",
                                                         UCS_LOG_LEVEL_ERROR);
    if (iface->rx.srq.devx.obj == NULL) {
        status = UCS_ERR_IO_ERROR;
        goto err_cleanup_srq;
    }

    iface->rx.srq.srq_num = UCT_IB_MLX5DV_GET(create_rmp_out, out, rmpn);

    return UCS_OK;

err_cleanup_srq:
    uct_rc_mlx5_devx_cleanup_srq(md, &iface->rx.srq);
    return status;
}

void uct_rc_mlx5_devx_cleanup_srq(uct_ib_mlx5_md_t *md, uct_ib_mlx5_srq_t *srq)
{
    uct_ib_mlx5_put_dbrec(srq->devx.dbrec);
    uct_ib_mlx5_md_buf_free(md, srq->buf, &srq->devx.mem);
}

ucs_status_t uct_rc_mlx5_iface_common_devx_connect_qp(
        uct_rc_mlx5_iface_common_t *iface, uct_ib_mlx5_qp_t *qp,
        uint32_t dest_qp_num, struct ibv_ah_attr *ah_attr,
        enum ibv_mtu path_mtu, uint8_t path_index, unsigned max_rd_atomic)
{
    uct_ib_mlx5_md_t *md = uct_ib_mlx5_iface_md(&iface->super.super);
    char in_2rtr[UCT_IB_MLX5DV_ST_SZ_BYTES(init2rtr_qp_in)]   = {};
    char out_2rtr[UCT_IB_MLX5DV_ST_SZ_BYTES(init2rtr_qp_out)] = {};
    char in_2rts[UCT_IB_MLX5DV_ST_SZ_BYTES(rtr2rts_qp_in)]    = {};
    char out_2rts[UCT_IB_MLX5DV_ST_SZ_BYTES(rtr2rts_qp_out)]  = {};
    uint32_t opt_param_mask = UCT_IB_MLX5_QP_OPTPAR_RRE |
                              UCT_IB_MLX5_QP_OPTPAR_RAE |
                              UCT_IB_MLX5_QP_OPTPAR_RWE;
    struct mlx5_wqe_av mlx5_av;
    ucs_status_t status;
    struct ibv_ah *ah;
    void *qpc;

    UCT_IB_MLX5DV_SET(init2rtr_qp_in, in_2rtr, opcode,
                      UCT_IB_MLX5_CMD_OP_INIT2RTR_QP);
    UCT_IB_MLX5DV_SET(init2rtr_qp_in, in_2rtr, qpn, qp->qp_num);

    ucs_assert(path_mtu != UCT_IB_ADDRESS_INVALID_PATH_MTU);
    qpc = UCT_IB_MLX5DV_ADDR_OF(init2rtr_qp_in, in_2rtr, qpc);
    UCT_IB_MLX5DV_SET(qpc, qpc, mtu, path_mtu);
    UCT_IB_MLX5DV_SET(qpc, qpc, log_msg_max, UCT_IB_MLX5_LOG_MAX_MSG_SIZE);
    UCT_IB_MLX5DV_SET(qpc, qpc, remote_qpn, dest_qp_num);

    uct_ib_mlx5_devx_set_qpc_dp_ordering(md, qpc, iface);

    if (uct_ib_iface_is_roce(&iface->super.super)) {
        status = uct_ib_iface_create_ah(&iface->super.super, ah_attr,
                                        "RC DEVX QP connect", &ah);
        if (status != UCS_OK) {
            return status;
        }

        uct_ib_mlx5_get_av(ah, &mlx5_av);
        memcpy(UCT_IB_MLX5DV_ADDR_OF(qpc, qpc, primary_address_path.rmac_47_32),
               mlx5_av.rmac, sizeof(mlx5_av.rmac));
        memcpy(UCT_IB_MLX5DV_ADDR_OF(qpc, qpc, primary_address_path.rgid_rip),
               mlx5_av.rgid, sizeof(mlx5_av.rgid));
        UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.hop_limit,
                          mlx5_av.hop_limit);
        UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.src_addr_index,
                          ah_attr->grh.sgid_index);
        UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.eth_prio,
                          iface->super.super.config.sl);
        if (uct_ib_iface_is_roce_v2(&iface->super.super)) {
            ucs_assert(ah_attr->dlid >= UCT_IB_ROCE_UDP_SRC_PORT_BASE);
            UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.udp_sport,
                              ah_attr->dlid);
            UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.dscp,
                              uct_ib_iface_roce_dscp(&iface->super.super));
        }

        uct_ib_mlx5_devx_set_qpc_port_affinity(md, path_index, qpc,
                                               &opt_param_mask);
    } else {
        UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.grh, ah_attr->is_global);
        UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.rlid, ah_attr->dlid);
        UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.mlid,
                          ah_attr->src_path_bits & 0x7f);
        UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.sl,
                          iface->super.super.config.sl);

        if (ah_attr->is_global) {
            UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.src_addr_index,
                              ah_attr->grh.sgid_index);
            UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.hop_limit,
                              ah_attr->grh.hop_limit);
            memcpy(UCT_IB_MLX5DV_ADDR_OF(qpc, qpc, primary_address_path.rgid_rip),
                   &ah_attr->grh.dgid,
                   UCT_IB_MLX5DV_FLD_SZ_BYTES(qpc, primary_address_path.rgid_rip));
            /* TODO add flow_label support */
            UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.tclass,
                              iface->super.super.config.traffic_class);
        }
    }

    UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.vhca_port_num, ah_attr->port_num);
    UCT_IB_MLX5DV_SET(qpc, qpc, log_rra_max, ucs_ilog2_or0(max_rd_atomic));
    UCT_IB_MLX5DV_SET(qpc, qpc, atomic_mode,
                      uct_ib_mlx5_get_atomic_mode(&iface->super.super));
    UCT_IB_MLX5DV_SET(qpc, qpc, rre, true);
    UCT_IB_MLX5DV_SET(qpc, qpc, rwe, true);
    UCT_IB_MLX5DV_SET(qpc, qpc, rae, true);
    UCT_IB_MLX5DV_SET(qpc, qpc, min_rnr_nak, iface->super.config.min_rnr_timer);

    if (md->super.ece_enable) {
        UCT_IB_MLX5DV_SET(init2rtr_qp_in, in_2rtr, ece, iface->super.config.ece);
    }

    UCT_IB_MLX5DV_SET(init2rtr_qp_in, in_2rtr, opt_param_mask, opt_param_mask);

    status = uct_ib_mlx5_devx_modify_qp(qp, in_2rtr, sizeof(in_2rtr),
                                        out_2rtr, sizeof(out_2rtr));
    if (status != UCS_OK) {
        return status;
    }

    UCT_IB_MLX5DV_SET(rtr2rts_qp_in, in_2rts, opcode,
                      UCT_IB_MLX5_CMD_OP_RTR2RTS_QP);
    UCT_IB_MLX5DV_SET(rtr2rts_qp_in, in_2rts, qpn, qp->qp_num);

    qpc = UCT_IB_MLX5DV_ADDR_OF(rtr2rts_qp_in, in_2rts, qpc);
    UCT_IB_MLX5DV_SET(qpc, qpc, log_sra_max, ucs_ilog2_or0(max_rd_atomic));
    UCT_IB_MLX5DV_SET(qpc, qpc, retry_count, iface->super.config.retry_cnt);
    UCT_IB_MLX5DV_SET(qpc, qpc, rnr_retry, iface->super.config.rnr_retry);
    UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.ack_timeout,
                      iface->super.config.timeout);
    UCT_IB_MLX5DV_SET(qpc, qpc, primary_address_path.log_rtm,
                      iface->super.config.exp_backoff);
    UCT_IB_MLX5DV_SET(qpc, qpc, log_ack_req_freq,
                      iface->config.log_ack_req_freq);

    status = uct_ib_mlx5_devx_modify_qp(qp, in_2rts, sizeof(in_2rts),
                                        out_2rts, sizeof(out_2rts));
    if (status != UCS_OK) {
        return status;
    }

    ucs_debug("connected rc devx qp 0x%x on "UCT_IB_IFACE_FMT" to lid %d(+%d) sl %d "
              "remote_qp 0x%x mtu %zu timer %dx%d rnr %dx%d rd_atom %d",
              qp->qp_num, UCT_IB_IFACE_ARG(&iface->super.super), ah_attr->dlid,
              ah_attr->src_path_bits, ah_attr->sl, dest_qp_num,
              uct_ib_mtu_value(iface->super.super.config.path_mtu),
              iface->super.config.timeout,
              iface->super.config.retry_cnt,
              iface->super.config.min_rnr_timer,
              iface->super.config.rnr_retry,
              iface->super.config.max_rd_atomic);
    return UCS_OK;
}

