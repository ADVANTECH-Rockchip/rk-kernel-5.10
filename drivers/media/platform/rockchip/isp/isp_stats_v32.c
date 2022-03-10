// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022 Rockchip Electronics Co., Ltd. */

#include <linux/kfifo.h>
#include <linux/rkisp32-config.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>	/* for ISP statistics */
#include "dev.h"
#include "regs.h"
#include "common.h"
#include "isp_stats.h"
#include "isp_stats_v32.h"

#define ISP32_3A_MEAS_DONE		BIT(31)

static void isp3_module_done(struct rkisp_isp_stats_vdev *stats_vdev,
			     u32 reg, u32 value)
{
	void __iomem *base = stats_vdev->dev->hw_dev->base_addr;

	writel(value, base + reg);
}

static u32 isp3_stats_read(struct rkisp_isp_stats_vdev *stats_vdev, u32 addr)
{
	return rkisp_read(stats_vdev->dev, addr, true);
}

static void isp3_stats_write(struct rkisp_isp_stats_vdev *stats_vdev,
			     u32 addr, u32 value)
{
	rkisp_write(stats_vdev->dev, addr, value, true);
}

static int
rkisp_stats_get_bls_stats(struct rkisp_isp_stats_vdev *stats_vdev,
			  struct rkisp32_isp_stat_buffer *pbuf)
{
	struct ispsd_in_fmt in_fmt = stats_vdev->dev->isp_sdev.in_fmt;
	enum rkisp_fmt_raw_pat_type raw_type = in_fmt.bayer_pat;
	struct isp2x_bls_stat *bls;
	u32 value;

	if (!pbuf)
		return 0;

	bls = &pbuf->params.bls;
	value = isp3_stats_read(stats_vdev, ISP3X_BLS_CTRL);
	if (value & (ISP_BLS_ENA | ISP_BLS_MODE_MEASURED)) {
		pbuf->meas_type |= ISP32_STAT_BLS;

		switch (raw_type) {
		case RAW_BGGR:
			bls->meas_r = isp3_stats_read(stats_vdev, ISP3X_BLS_D_MEASURED);
			bls->meas_gr = isp3_stats_read(stats_vdev, ISP3X_BLS_C_MEASURED);
			bls->meas_gb = isp3_stats_read(stats_vdev, ISP3X_BLS_B_MEASURED);
			bls->meas_b = isp3_stats_read(stats_vdev, ISP3X_BLS_A_MEASURED);
			break;
		case RAW_GBRG:
			bls->meas_r = isp3_stats_read(stats_vdev, ISP3X_BLS_C_MEASURED);
			bls->meas_gr = isp3_stats_read(stats_vdev, ISP3X_BLS_D_MEASURED);
			bls->meas_gb = isp3_stats_read(stats_vdev, ISP3X_BLS_A_MEASURED);
			bls->meas_b = isp3_stats_read(stats_vdev, ISP3X_BLS_B_MEASURED);
			break;
		case RAW_GRBG:
			bls->meas_r = isp3_stats_read(stats_vdev, ISP3X_BLS_B_MEASURED);
			bls->meas_gr = isp3_stats_read(stats_vdev, ISP3X_BLS_A_MEASURED);
			bls->meas_gb = isp3_stats_read(stats_vdev, ISP3X_BLS_D_MEASURED);
			bls->meas_b = isp3_stats_read(stats_vdev, ISP3X_BLS_C_MEASURED);
			break;
		case RAW_RGGB:
			bls->meas_r = isp3_stats_read(stats_vdev, ISP3X_BLS_A_MEASURED);
			bls->meas_gr = isp3_stats_read(stats_vdev, ISP3X_BLS_B_MEASURED);
			bls->meas_gb = isp3_stats_read(stats_vdev, ISP3X_BLS_C_MEASURED);
			bls->meas_b = isp3_stats_read(stats_vdev, ISP3X_BLS_D_MEASURED);
			break;
		default:
			break;
		}
	}
	return 0;
}

static int
rkisp_stats_get_dhaz_stats(struct rkisp_isp_stats_vdev *stats_vdev,
			   struct rkisp32_isp_stat_buffer *pbuf)
{
	struct isp3x_dhaz_stat *dhaz;
	u32 value, i;

	if (!pbuf)
		return 0;

	dhaz = &pbuf->params.dhaz;
	value = isp3_stats_read(stats_vdev, ISP3X_DHAZ_CTRL);
	if (value & ISP_DHAZ_ENMUX) {
		pbuf->meas_type |= ISP32_STAT_DHAZ;

		value = isp3_stats_read(stats_vdev, ISP3X_DHAZ_SUMH_RD);
		dhaz->dhaz_pic_sumh = value;

		value = isp3_stats_read(stats_vdev, ISP3X_DHAZ_ADP_RD0);
		dhaz->dhaz_adp_air_base = value >> 16;
		dhaz->dhaz_adp_wt = value & 0xFFFF;

		value = isp3_stats_read(stats_vdev, ISP3X_DHAZ_ADP_RD1);
		dhaz->dhaz_adp_gratio = value >> 16;
		dhaz->dhaz_adp_tmax = value & 0xFFFF;

		for (i = 0; i < ISP3X_DHAZ_HIST_IIR_NUM / 2; i++) {
			value = isp3_stats_read(stats_vdev, ISP3X_DHAZ_HIST_REG0 + 4 * i);
			dhaz->h_rgb_iir[2 * i] = value & 0xFFFF;
			dhaz->h_rgb_iir[2 * i + 1] = value >> 16;
		}
	}
	return 0;
}

static int
rkisp_stats_get_rawawb_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				struct rkisp32_isp_stat_buffer *pbuf)
{
	u32 ctrl = isp3_stats_read(stats_vdev, ISP3X_RAWAWB_CTRL);

	if (!(ctrl & ISP32_3A_MEAS_DONE)) {
		v4l2_dbg(1, rkisp_debug, &stats_vdev->dev->v4l2_dev,
			 "%s fail, ctrl:0x%x\n", __func__, ctrl);
		return -ENODATA;
	}

	if (!pbuf)
		goto out;

	pbuf->meas_type |= ISP32_STAT_RAWAWB;
out:
	isp3_module_done(stats_vdev, ISP3X_RAWAWB_CTRL, ctrl);
	return 0;
}

static int
rkisp_stats_get_rawaf_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
			       struct rkisp32_isp_stat_buffer *pbuf)
{
	struct isp32_rawaf_stat *af;
	u32 ctrl;

	ctrl = isp3_stats_read(stats_vdev, ISP3X_RAWAF_CTRL);
	if (!(ctrl & ISP32_3A_MEAS_DONE)) {
		v4l2_dbg(1, rkisp_debug, &stats_vdev->dev->v4l2_dev,
			 "%s fail, ctrl:0x%x\n", __func__, ctrl);
		return -ENODATA;
	}

	if (!pbuf)
		goto out;

	af = &pbuf->params.rawaf;
	pbuf->meas_type |= ISP32_STAT_RAWAF;

	af->afm_sum_b = isp3_stats_read(stats_vdev, ISP3X_RAWAF_SUM_B);
	af->afm_lum_b = isp3_stats_read(stats_vdev, ISP3X_RAWAF_LUM_B);
	af->int_state = isp3_stats_read(stats_vdev, ISP3X_RAWAF_INT_STATE);
	af->highlit_cnt_winb = isp3_stats_read(stats_vdev, ISP3X_RAWAF_HIGHLIT_CNT_WINB);

out:
	isp3_module_done(stats_vdev, ISP3X_RAWAF_CTRL, ctrl);
	return 0;
}

static int
rkisp_stats_get_rawaebig_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				  struct isp32_rawaebig_stat1 *ae, u32 blk_no)
{
	u32 i, base, addr, ctrl;

	switch (blk_no) {
	case 1:
		base = RAWAE_BIG2_BASE;
		break;
	case 2:
		base = RAWAE_BIG3_BASE;
		break;
	default:
		base = RAWAE_BIG1_BASE;
		break;
	}

	ctrl = isp3_stats_read(stats_vdev, base + ISP3X_RAWAE_BIG_CTRL);
	if (!(ctrl & ISP32_3A_MEAS_DONE)) {
		v4l2_dbg(1, rkisp_debug, &stats_vdev->dev->v4l2_dev,
			 "%s fail, addr:0x%x ctrl:0x%x\n",
			 __func__, base, ctrl);
		return -ENODATA;
	}

	if (!ae)
		goto out;

	for (i = 0; i < ISP32_RAWAEBIG_SUBWIN_NUM; i++) {
		addr = base + ISP3X_RAWAE_BIG_WND1_SUMR + i * 4;
		ae->sumr[i] = isp3_stats_read(stats_vdev, addr);
		addr = base + ISP3X_RAWAE_BIG_WND1_SUMG + i * 4;
		ae->sumg[i] = isp3_stats_read(stats_vdev, addr);
		addr = base + ISP3X_RAWAE_BIG_WND1_SUMB + i * 4;
		ae->sumb[i] = isp3_stats_read(stats_vdev, addr);
	}

out:
	isp3_module_done(stats_vdev, base + ISP3X_RAWAE_BIG_CTRL, ctrl);
	return 0;
}

static int
rkisp_stats_get_rawhstbig_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				   struct isp2x_rawhistbig_stat *hst, u32 blk_no)
{
	u32 addr, ctrl;

	switch (blk_no) {
	case 1:
		addr = ISP3X_RAWHIST_BIG2_BASE;
		break;
	case 2:
		addr = ISP3X_RAWHIST_BIG3_BASE;
		break;
	case 0:
	default:
		addr = ISP3X_RAWHIST_BIG1_BASE;
		break;
	}

	ctrl = isp3_stats_read(stats_vdev, addr + ISP3X_RAWHIST_BIG_CTRL);
	if (!(ctrl & ISP32_3A_MEAS_DONE)) {
		v4l2_dbg(1, rkisp_debug, &stats_vdev->dev->v4l2_dev,
			 "%s fail, addr:0x%x ctrl:0x%x\n",
			 __func__, addr, ctrl);
		return -ENODATA;
	}

	if (!hst)
		goto out;

out:
	isp3_module_done(stats_vdev, addr + ISP3X_RAWHIST_BIG_CTRL, ctrl);
	return 0;
}

static int
rkisp_stats_get_rawae1_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				struct rkisp32_isp_stat_buffer *pbuf)
{
	int ret = 0;

	if (!pbuf) {
		rkisp_stats_get_rawaebig_meas_ddr(stats_vdev, NULL, 1);
	} else {
		ret = rkisp_stats_get_rawaebig_meas_ddr(stats_vdev,
							&pbuf->params.rawae1_1, 1);
		if (!ret)
			pbuf->meas_type |= ISP32_STAT_RAWAE1;
	}

	return ret;
}

static int
rkisp_stats_get_rawhst1_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				 struct rkisp32_isp_stat_buffer *pbuf)
{
	int ret = 0;

	if (!pbuf) {
		rkisp_stats_get_rawhstbig_meas_ddr(stats_vdev, NULL, 1);
	} else {
		ret = rkisp_stats_get_rawhstbig_meas_ddr(stats_vdev,
							 &pbuf->params.rawhist1, 1);
		if (!ret)
			pbuf->meas_type |= ISP32_STAT_RAWHST1;
	}

	return ret;
}

static int
rkisp_stats_get_rawae2_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				struct rkisp32_isp_stat_buffer *pbuf)
{
	int ret = 0;

	if (!pbuf) {
		rkisp_stats_get_rawaebig_meas_ddr(stats_vdev, NULL, 2);
	} else {
		ret = rkisp_stats_get_rawaebig_meas_ddr(stats_vdev,
							&pbuf->params.rawae2_1, 2);
		if (!ret)
			pbuf->meas_type |= ISP32_STAT_RAWAE2;
	}

	return ret;
}

static int
rkisp_stats_get_rawhst2_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				 struct rkisp32_isp_stat_buffer *pbuf)
{
	int ret = 0;

	if (!pbuf) {
		rkisp_stats_get_rawhstbig_meas_ddr(stats_vdev, NULL, 2);
	} else {
		ret = rkisp_stats_get_rawhstbig_meas_ddr(stats_vdev,
							 &pbuf->params.rawhist2, 2);
		if (!ret)
			pbuf->meas_type |= ISP32_STAT_RAWHST2;
	}

	return ret;
}

static int
rkisp_stats_get_rawae3_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				struct rkisp32_isp_stat_buffer *pbuf)
{
	int ret = 0;

	if (!pbuf) {
		rkisp_stats_get_rawaebig_meas_ddr(stats_vdev, NULL, 0);
	} else {
		ret = rkisp_stats_get_rawaebig_meas_ddr(stats_vdev,
							&pbuf->params.rawae3_1, 0);
		if (!ret)
			pbuf->meas_type |= ISP32_STAT_RAWAE3;
	}

	return ret;
}

static int
rkisp_stats_get_rawhst3_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				 struct rkisp32_isp_stat_buffer *pbuf)
{
	int ret = 0;

	if (!pbuf) {
		rkisp_stats_get_rawhstbig_meas_ddr(stats_vdev, NULL, 0);
	} else {
		ret = rkisp_stats_get_rawhstbig_meas_ddr(stats_vdev,
							 &pbuf->params.rawhist3, 0);
		if (!ret)
			pbuf->meas_type |= ISP32_STAT_RAWHST3;
	}

	return ret;
}

static int
rkisp_stats_get_rawaelite_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				   struct rkisp32_isp_stat_buffer *pbuf)
{
	u32 ctrl;

	ctrl = isp3_stats_read(stats_vdev, ISP3X_RAWAE_LITE_CTRL);
	if ((ctrl & ISP32_3A_MEAS_DONE) == 0) {
		v4l2_dbg(1, rkisp_debug, &stats_vdev->dev->v4l2_dev,
			 "%s fail, ctrl:0x%x\n", __func__, ctrl);
		return -ENODATA;
	}

	if (!pbuf)
		goto out;

	pbuf->meas_type |= ISP32_STAT_RAWAE0;

out:
	isp3_module_done(stats_vdev, ISP3X_RAWAE_LITE_CTRL, ctrl);
	return 0;
}

static int
rkisp_stats_get_rawhstlite_meas_ddr(struct rkisp_isp_stats_vdev *stats_vdev,
				    struct rkisp32_isp_stat_buffer *pbuf)
{
	u32 ctrl;

	ctrl = isp3_stats_read(stats_vdev, ISP3X_RAWHIST_LITE_CTRL);
	if ((ctrl & ISP32_3A_MEAS_DONE) == 0) {
		v4l2_dbg(1, rkisp_debug, &stats_vdev->dev->v4l2_dev,
			 "%s fail, ctrl:0x%x\n", __func__, ctrl);
		return -ENODATA;
	}

	if (!pbuf)
		goto out;

	pbuf->meas_type |= ISP32_STAT_RAWHST0;

out:
	isp3_module_done(stats_vdev, ISP3X_RAWHIST_LITE_CTRL, ctrl);
	return 0;
}

static struct rkisp_stats_ops_v32 __maybe_unused stats_ddr_ops_v32 = {
	.get_rawawb_meas = rkisp_stats_get_rawawb_meas_ddr,
	.get_rawaf_meas = rkisp_stats_get_rawaf_meas_ddr,
	.get_rawae0_meas = rkisp_stats_get_rawaelite_meas_ddr,
	.get_rawhst0_meas = rkisp_stats_get_rawhstlite_meas_ddr,
	.get_rawae1_meas = rkisp_stats_get_rawae1_meas_ddr,
	.get_rawhst1_meas = rkisp_stats_get_rawhst1_meas_ddr,
	.get_rawae2_meas = rkisp_stats_get_rawae2_meas_ddr,
	.get_rawhst2_meas = rkisp_stats_get_rawhst2_meas_ddr,
	.get_rawae3_meas = rkisp_stats_get_rawae3_meas_ddr,
	.get_rawhst3_meas = rkisp_stats_get_rawhst3_meas_ddr,
	.get_bls_stats = rkisp_stats_get_bls_stats,
	.get_dhaz_stats = rkisp_stats_get_dhaz_stats,
};

static void
rkisp_stats_update_buf(struct rkisp_isp_stats_vdev *stats_vdev)
{
	struct rkisp_device *dev = stats_vdev->dev;
	struct rkisp_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&stats_vdev->rd_lock, flags);
	if (!stats_vdev->nxt_buf && !list_empty(&stats_vdev->stat)) {
		buf = list_first_entry(&stats_vdev->stat,
				       struct rkisp_buffer, queue);
		list_del(&buf->queue);
		stats_vdev->nxt_buf = buf;
	}
	spin_unlock_irqrestore(&stats_vdev->rd_lock, flags);

	if (stats_vdev->nxt_buf) {
		rkisp_set_bits(dev, ISP3X_SWS_CFG, 0, ISP3X_3A_DDR_WRITE_EN, false);
		rkisp_write(dev, ISP3X_MI_3A_WR_BASE, stats_vdev->nxt_buf->buff_addr[0], false);
		v4l2_dbg(2, rkisp_debug, &dev->v4l2_dev,
			 "%s BASE:0x%x SHD:0x%x\n",
			 __func__, stats_vdev->nxt_buf->buff_addr[0],
			 isp3_stats_read(stats_vdev, ISP3X_MI_3A_WR_BASE));
		if (!dev->hw_dev->is_single) {
			stats_vdev->cur_buf = stats_vdev->nxt_buf;
			stats_vdev->nxt_buf = NULL;
		}
	} else {
		rkisp_clear_bits(dev, ISP3X_SWS_CFG, ISP3X_3A_DDR_WRITE_EN, false);
	}
}

static void
rkisp_stats_send_meas_v32(struct rkisp_isp_stats_vdev *stats_vdev,
			  struct rkisp_isp_readout_work *meas_work)
{
	unsigned int cur_frame_id = -1;
	struct rkisp_buffer *cur_buf = stats_vdev->cur_buf;
	struct rkisp32_isp_stat_buffer *cur_stat_buf = NULL;
	struct rkisp_stats_ops_v32 *ops =
		(struct rkisp_stats_ops_v32 *)stats_vdev->priv_ops;
	u32 size = sizeof(struct rkisp32_isp_stat_buffer);
	int ret = 0;

	/* config buf for next frame */
	stats_vdev->cur_buf = NULL;
	if (stats_vdev->nxt_buf) {
		stats_vdev->cur_buf = stats_vdev->nxt_buf;
		stats_vdev->nxt_buf = NULL;
	}
	rkisp_stats_update_buf(stats_vdev);

	cur_frame_id = meas_work->frame_id;

	if (cur_buf) {
		cur_stat_buf =
			(struct rkisp32_isp_stat_buffer *)(cur_buf->vaddr[0]);
		cur_stat_buf->frame_id = cur_frame_id;
	}

	if (meas_work->isp_ris & ISP3X_AFM_SUM_OF)
		v4l2_warn(stats_vdev->vnode.vdev.v4l2_dev,
			  "ISP3X_AFM_SUM_OF\n");

	if (meas_work->isp_ris & ISP3X_AFM_LUM_OF)
		v4l2_warn(stats_vdev->vnode.vdev.v4l2_dev,
			  "ISP3X_AFM_LUM_OF\n");

	if (meas_work->isp3a_ris & ISP3X_3A_RAWAF_SUM)
		v4l2_warn(stats_vdev->vnode.vdev.v4l2_dev,
			  "ISP3X_3A_RAWAF_SUM\n");

	if (meas_work->isp3a_ris & ISP3X_3A_RAWAWB)
		ret |= ops->get_rawawb_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWAF)
		ret |= ops->get_rawaf_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWAE_BIG)
		ret |= ops->get_rawae3_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWHIST_BIG)
		ret |= ops->get_rawhst3_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWAE_CH0)
		ret |= ops->get_rawae0_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWAE_CH1)
		ret |= ops->get_rawae1_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWAE_CH2)
		ret |= ops->get_rawae2_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWHIST_CH0)
		ret |= ops->get_rawhst0_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWHIST_CH1)
		ret |= ops->get_rawhst1_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp3a_ris & ISP3X_3A_RAWHIST_CH2)
		ret |= ops->get_rawhst2_meas(stats_vdev, cur_stat_buf);

	if (meas_work->isp_ris & ISP3X_FRAME) {
		ret |= ops->get_bls_stats(stats_vdev, cur_stat_buf);
		ret |= ops->get_dhaz_stats(stats_vdev, cur_stat_buf);
	}

	if (cur_buf) {
		if (ret || !cur_stat_buf->meas_type) {
			unsigned long flags;

			spin_lock_irqsave(&stats_vdev->rd_lock, flags);
			list_add_tail(&cur_buf->queue, &stats_vdev->stat);
			spin_unlock_irqrestore(&stats_vdev->rd_lock, flags);
		} else {
			vb2_set_plane_payload(&cur_buf->vb.vb2_buf, 0, size);
			cur_buf->vb.sequence = cur_frame_id;
			cur_buf->vb.vb2_buf.timestamp = meas_work->timestamp;
			vb2_buffer_done(&cur_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
	}
}

static void
rkisp_stats_isr_v32(struct rkisp_isp_stats_vdev *stats_vdev,
		    u32 isp_ris, u32 isp3a_ris)
{
	struct rkisp_isp_readout_work work;
	u32 iq_isr_mask = ISP3X_SIAWB_DONE | ISP3X_SIAF_FIN |
		ISP3X_EXP_END | ISP3X_SIHST_RDY | ISP3X_AFM_SUM_OF | ISP3X_AFM_LUM_OF;
	u32 cur_frame_id, isp_mis_tmp = 0;
	u32 temp_isp_ris, temp_isp3a_ris;

	rkisp_dmarx_get_frame(stats_vdev->dev, &cur_frame_id, NULL, NULL, true);

	spin_lock(&stats_vdev->irq_lock);

	temp_isp_ris = isp3_stats_read(stats_vdev, ISP3X_ISP_RIS);
	temp_isp3a_ris = isp3_stats_read(stats_vdev, ISP3X_ISP_3A_RIS);

	isp_mis_tmp = isp_ris & iq_isr_mask;
	if (isp_mis_tmp) {
		isp3_stats_write(stats_vdev, ISP3X_ISP_ICR, isp_mis_tmp);

		isp_mis_tmp &= isp3_stats_read(stats_vdev, ISP3X_ISP_MIS);
		if (isp_mis_tmp)
			v4l2_err(stats_vdev->vnode.vdev.v4l2_dev,
				 "isp icr 3A info err: 0x%x 0x%x\n",
				 isp_mis_tmp, isp_ris);
	}

	isp_mis_tmp = temp_isp3a_ris;
	if (isp_mis_tmp) {
		isp3_stats_write(stats_vdev, ISP3X_ISP_3A_ICR, isp_mis_tmp);

		isp_mis_tmp &= isp3_stats_read(stats_vdev, ISP3X_ISP_3A_MIS);
		if (isp_mis_tmp)
			v4l2_err(stats_vdev->vnode.vdev.v4l2_dev,
				 "isp3A icr 3A info err: 0x%x 0x%x\n",
				 isp_mis_tmp, isp3a_ris);
	}

	if (!stats_vdev->streamon)
		goto unlock;

	if (isp_ris & ISP3X_FRAME) {
		work.readout = RKISP_ISP_READOUT_MEAS;
		work.frame_id = cur_frame_id;
		work.isp_ris = temp_isp_ris | isp_ris;
		work.isp3a_ris = temp_isp3a_ris;
		work.timestamp = ktime_get_ns();
		rkisp_stats_send_meas_v32(stats_vdev, &work);
	}

unlock:
	spin_unlock(&stats_vdev->irq_lock);
}

static void
rkisp_stats_rdbk_enable_v32(struct rkisp_isp_stats_vdev *stats_vdev, bool en)
{
	if (!en) {
		stats_vdev->isp_rdbk = 0;
		stats_vdev->isp3a_rdbk = 0;
	}

	stats_vdev->rdbk_mode = en;
}

static struct rkisp_isp_stats_ops rkisp_isp_stats_ops_tbl = {
	.isr_hdl = rkisp_stats_isr_v32,
	.send_meas = rkisp_stats_send_meas_v32,
	.rdbk_enable = rkisp_stats_rdbk_enable_v32,
};

void rkisp_stats_first_ddr_config_v32(struct rkisp_isp_stats_vdev *stats_vdev)
{
	struct rkisp_device *dev = stats_vdev->dev;
	u32 size = stats_vdev->vdev_fmt.fmt.meta.buffersize;

	if (!stats_vdev->streamon)
		return;

	rkisp_stats_update_buf(stats_vdev);
	rkisp_write(dev, ISP3X_MI_DBR_WR_SIZE, size, false);
	if (stats_vdev->nxt_buf) {
		stats_vdev->cur_buf = stats_vdev->nxt_buf;
		stats_vdev->nxt_buf = NULL;
	}
}

void rkisp_stats_next_ddr_config_v32(struct rkisp_isp_stats_vdev *stats_vdev)
{
	struct rkisp_hw_dev *hw = stats_vdev->dev->hw_dev;

	if (!stats_vdev->streamon)
		return;
	/* pingpong buf */
	if (hw->is_single)
		rkisp_stats_update_buf(stats_vdev);
}

void rkisp_init_stats_vdev_v32(struct rkisp_isp_stats_vdev *stats_vdev)
{
	stats_vdev->vdev_fmt.fmt.meta.dataformat =
		V4L2_META_FMT_RK_ISP1_STAT_3A;
	stats_vdev->vdev_fmt.fmt.meta.buffersize =
		sizeof(struct rkisp32_isp_stat_buffer);

	stats_vdev->ops = &rkisp_isp_stats_ops_tbl;
	stats_vdev->priv_ops = &stats_ddr_ops_v32;
	stats_vdev->rd_stats_from_ddr = true;
}

void rkisp_uninit_stats_vdev_v32(struct rkisp_isp_stats_vdev *stats_vdev)
{

}
