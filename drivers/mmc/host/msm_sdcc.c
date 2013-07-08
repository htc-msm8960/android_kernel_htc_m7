/*
 *  linux/drivers/mmc/host/msm_sdcc.c - Qualcomm MSM 7X00A SDCC Driver
 *
 *  Copyright (C) 2007 Google Inc,
 *  Copyright (C) 2003 Deep Blue Solutions, Ltd, All Rights Reserved.
 *  Copyright (c) 2009-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Based on mmci.c
 *
 * Author: San Mehat (san@android.com)
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/log2.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>
#include <linux/clk.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/memory.h>
#include <linux/pm_runtime.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/pm_qos.h>
#include <linux/android_alarm.h>
#include <linux/proc_fs.h>

#include <asm/cacheflush.h>
#include <asm/div64.h>
#include <asm/sizes.h>

#include <asm/mach/mmc.h>
#include <mach/msm_iomap.h>
#include <mach/clk.h>
#include <mach/dma.h>
#include <mach/sdio_al.h>
#include <mach/mpm.h>
#include <mach/msm_bus.h>

#include <linux/rtc.h>

#include "msm_sdcc.h"
#include "msm_sdcc_dml.h"
#include <mach/msm_rtb_disable.h> 

#define DRIVER_NAME "msm-sdcc"

#define DBG(host, fmt, args...)	\
	pr_debug("%s: %s: " fmt "\n", mmc_hostname(host->mmc), __func__ , args)

#define PRINTRTC  do { \
struct timespec ts; \
struct rtc_time tm; \
getnstimeofday(&ts); \
rtc_time_to_tm(ts.tv_sec, &tm); \
printk(KERN_INFO " at %lld (%d-%02d-%02d %02d:%02d:%02d.%09lu UTC)\n", \
ktime_to_ns(ktime_get()), tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, \
tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec); \
} while (0)


#define IRQ_DEBUG 0
#define SPS_SDCC_PRODUCER_PIPE_INDEX	1
#define SPS_SDCC_CONSUMER_PIPE_INDEX	2
#define SPS_CONS_PERIPHERAL		0
#define SPS_PROD_PERIPHERAL		1
#define SPS_MIN_XFER_SIZE		MCI_FIFOSIZE

#define MSM_MMC_BUS_VOTING_DELAY	200 

#ifdef CONFIG_WIMAX
#define SDC_CLK_VERBOSE 1
#endif
#define MAX_CONTINUOUS_TUNING_COUNT 10

#if defined(CONFIG_DEBUG_FS)
static void msmsdcc_dbg_createhost(struct msmsdcc_host *);
static struct dentry *debugfs_dir;
static struct dentry *debugfs_file;
static int  msmsdcc_dbg_init(void);
#endif

static int msmsdcc_prep_xfer(struct msmsdcc_host *host, struct mmc_data
			     *data);
static void msmsdcc_print_pin_info(struct msmsdcc_host *host);
#ifdef CONFIG_PM_RUNTIME
static void msmsdcc_print_rpm_info(struct msmsdcc_host *host);
#endif
static u64 dma_mask = DMA_BIT_MASK(32);
static unsigned int msmsdcc_pwrsave = 1;

static struct mmc_command dummy52cmd;
static struct mmc_request dummy52mrq = {
	.cmd = &dummy52cmd,
	.data = NULL,
	.stop = NULL,
};
static struct mmc_command dummy52cmd = {
	.opcode = SD_IO_RW_DIRECT,
	.flags = MMC_RSP_PRESENT,
	.data = NULL,
	.mrq = &dummy52mrq,
};
static const u32 tuning_block_64[] = {
	0x00FF0FFF, 0xCCC3CCFF, 0xFFCC3CC3, 0xEFFEFFFE,
	0xDDFFDFFF, 0xFBFFFBFF, 0xFF7FFFBF, 0xEFBDF777,
	0xF0FFF0FF, 0x3CCCFC0F, 0xCFCC33CC, 0xEEFFEFFF,
	0xFDFFFDFF, 0xFFBFFFDF, 0xFFF7FFBB, 0xDE7B7FF7
};

static const u32 tuning_block_128[] = {
	0xFF00FFFF, 0x0000FFFF, 0xCCCCFFFF, 0xCCCC33CC,
	0xCC3333CC, 0xFFFFCCCC, 0xFFFFEEFF, 0xFFEEEEFF,
	0xFFDDFFFF, 0xDDDDFFFF, 0xBBFFFFFF, 0xBBFFFFFF,
	0xFFFFFFBB, 0xFFFFFF77, 0x77FF7777, 0xFFEEDDBB,
	0x00FFFFFF, 0x00FFFFFF, 0xCCFFFF00, 0xCC33CCCC,
	0x3333CCCC, 0xFFCCCCCC, 0xFFEEFFFF, 0xEEEEFFFF,
	0xDDFFFFFF, 0xDDFFFFFF, 0xFFFFFFDD, 0xFFFFFFBB,
	0xFFFFBBBB, 0xFFFF77FF, 0xFF7777FF, 0xEEDDBB77
};

const int SD_detect_debounce_time = 50;
#if SD_DEBOUNCE_DEBUG
static ktime_t last_irq;
ktime_t detect_wq;
ktime_t irq_diff;
#endif

#if IRQ_DEBUG == 1
static char *irq_status_bits[] = { "cmdcrcfail", "datcrcfail", "cmdtimeout",
				   "dattimeout", "txunderrun", "rxoverrun",
				   "cmdrespend", "cmdsent", "dataend", NULL,
				   "datablkend", "cmdactive", "txactive",
				   "rxactive", "txhalfempty", "rxhalffull",
				   "txfifofull", "rxfifofull", "txfifoempty",
				   "rxfifoempty", "txdataavlbl", "rxdataavlbl",
				   "sdiointr", "progdone", "atacmdcompl",
				   "sdiointrope", "ccstimeout", NULL, NULL,
				   NULL, NULL, NULL };

static void
msmsdcc_print_status(struct msmsdcc_host *host, char *hdr, uint32_t status)
{
	int i;

	pr_debug("%s-%s ", mmc_hostname(host->mmc), hdr);
	for (i = 0; i < 32; i++) {
		if (status & (1 << i))
			pr_debug("%s ", irq_status_bits[i]);
	}
	pr_debug("\n");
}
#endif

static char *mmc_type_str(unsigned int slot_type)
{
	switch (slot_type) {
	case MMC_TYPE_MMC:	return "MMC";
	case MMC_TYPE_SD:	return "SD";
	case MMC_TYPE_SDIO:	return "SDIO";
	case MMC_TYPE_SDIO_WIFI:    return "SDIO(WIFI)";
	case MMC_TYPE_SDIO_WIMAX:	return "SDIO(WiMAX)";
	case MMC_TYPE_SDIO_SVLTE:	return "SDIO(SVLTE)";
	default:		return "Unknown type";
	}
}

static int is_mmc_platform(struct mmc_platform_data *plat)
{
	if (plat && plat->slot_type && *plat->slot_type == MMC_TYPE_MMC)
		return 1;

	return 0;
}

int mmc_tuning_fail = 0;
EXPORT_SYMBOL(mmc_tuning_fail);

static int is_wifi_slot(struct mmc_platform_data *plat)
{
	if (plat->slot_type && *plat->slot_type == MMC_TYPE_SDIO_WIFI)
		return 1;

	return 0;
}

int is_wifi_platform(struct mmc_platform_data *plat)
{
	if (plat && plat->slot_type && *plat->slot_type == MMC_TYPE_SDIO_WIFI)
		return 1;

	return 0;
}
EXPORT_SYMBOL(is_wifi_platform);

int is_wifi_mmc_host(struct mmc_host *mmc)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	if (host && is_wifi_platform(host->plat))
		return 1;

	return 0;
}
EXPORT_SYMBOL(is_wifi_mmc_host);

int is_wimax_platform(struct mmc_platform_data *plat)
{
	if (plat && plat->slot_type && *plat->slot_type == MMC_TYPE_SDIO_WIMAX)
		return 1;

	return 0;
}
EXPORT_SYMBOL(is_wimax_platform);

#ifdef CONFIG_WIMAX
static int wimax_enable_irq(struct mmc_host *mmc)
{
	struct msmsdcc_host *host = mmc_priv(mmc);

	pr_info("[WIMAX] [MMC] %s: enable_irq wimax ++ host->sdcc_irq_disabled=%d\n", mmc_hostname(mmc),host->sdcc_irq_disabled);
	if (is_wimax_platform(host->plat)) {
		if (host->sdcc_irq_disabled) {
			host->sdcc_irq_disabled = 0;
			pr_info("[WIMAX] [MMC] %s: enable_irq wimax\n", mmc_hostname(mmc));
			enable_irq(host->core_irqres->start);
		} else
		    
			;
	}
	pr_info("[WIMAX] [MMC] %s: enable_irq wimax --\n", mmc_hostname(mmc));
	return 0;
}

static int wimax_disable_irq(struct mmc_host *mmc)
{
	struct msmsdcc_host *host = mmc_priv(mmc);

	pr_info("[WIMAX] [MMC] %s: disable_irq wimax ++ host->sdcc_irq_disabled=%d\n", mmc_hostname(mmc),host->sdcc_irq_disabled);
	if (is_wimax_platform(host->plat)) {
		if (!host->sdcc_irq_disabled) {
			pr_info("[WIMAX] [MMC] %s: disable_irq wimax\n", mmc_hostname(mmc));
			disable_irq(host->core_irqres->start);
			host->sdcc_irq_disabled = 1;
		} else
			
			;

	}
	return 0;
}
#endif
static int is_sd_platform(struct mmc_platform_data *plat)
{
	if (plat && plat->slot_type && *plat->slot_type == MMC_TYPE_SD)
		return 1;

	return 0;
}
#if SD_DEBOUNCE_DEBUG
int mmc_is_sd_host(struct mmc_host *mmc) {
	 struct msmsdcc_host *host = mmc_priv(mmc);
	 if (host->plat->slot_type && *host->plat->slot_type == MMC_TYPE_SD)
		return 1;
	else
		return 0;
}
#endif

static void
msmsdcc_start_command(struct msmsdcc_host *host, struct mmc_command *cmd,
		      u32 c);
static inline void msmsdcc_sync_reg_wr(struct msmsdcc_host *host);
static inline void msmsdcc_delay(struct msmsdcc_host *host);
static void msmsdcc_dump_sdcc_state(struct msmsdcc_host *host);
static void msmsdcc_sg_start(struct msmsdcc_host *host);
static int msmsdcc_vreg_reset(struct msmsdcc_host *host);
static int msmsdcc_runtime_resume(struct device *dev);
static int msmsdcc_execute_tuning(struct mmc_host *mmc, u32 opcode);
static bool msmsdcc_is_wait_for_auto_prog_done(struct msmsdcc_host *host,
					       struct mmc_request *mrq);
static bool msmsdcc_is_wait_for_prog_done(struct msmsdcc_host *host,
					  struct mmc_request *mrq);

static inline unsigned short msmsdcc_get_nr_sg(struct msmsdcc_host *host)
{
	unsigned short ret = NR_SG;

	if (is_sps_mode(host)) {
		ret = SPS_MAX_DESCS;
	} else { 
		if (NR_SG > MAX_NR_SG_DMA_PIO)
			ret = MAX_NR_SG_DMA_PIO;
	}

	return ret;
}

static void msmsdcc_pm_qos_update_latency(struct msmsdcc_host *host, int vote)
{
	if (!host->cpu_dma_latency)
		return;

	if (vote)
		pm_qos_update_request(&host->pm_qos_req_dma,
				host->cpu_dma_latency);
	else
		pm_qos_update_request(&host->pm_qos_req_dma,
					PM_QOS_DEFAULT_VALUE);
}

#ifdef CONFIG_MMC_MSM_SPS_SUPPORT
static int msmsdcc_sps_reset_ep(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep);
static int msmsdcc_sps_restore_ep(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep);
#else
static inline int msmsdcc_sps_init_ep_conn(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep,
				bool is_producer) { return 0; }
static inline void msmsdcc_sps_exit_ep_conn(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep) { }
static inline int msmsdcc_sps_reset_ep(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep)
{
	return 0;
}
static inline int msmsdcc_sps_restore_ep(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep)
{
	return 0;
}
static inline int msmsdcc_sps_init(struct msmsdcc_host *host) { return 0; }
static inline void msmsdcc_sps_exit(struct msmsdcc_host *host) {}
#endif 

static void msmsdcc_sps_pipes_reset_and_restore(struct msmsdcc_host *host)
{
	int rc;

	
	rc = msmsdcc_sps_reset_ep(host, &host->sps.prod);
	if (rc)
		pr_err("%s:msmsdcc_sps_reset_ep(prod) error=%d\n",
				mmc_hostname(host->mmc), rc);
	rc = msmsdcc_sps_reset_ep(host, &host->sps.cons);
	if (rc)
		pr_err("%s:msmsdcc_sps_reset_ep(cons) error=%d\n",
				mmc_hostname(host->mmc), rc);

	if (host->sps.reset_device) {
		rc = sps_device_reset(host->sps.bam_handle);
		if (rc)
			pr_err("%s: sps_device_reset error=%d\n",
				mmc_hostname(host->mmc), rc);
		host->sps.reset_device = false;
	}

	
	rc = msmsdcc_sps_restore_ep(host, &host->sps.prod);
	if (rc)
		pr_err("%s:msmsdcc_sps_restore_ep(prod) error=%d\n",
				mmc_hostname(host->mmc), rc);
	rc = msmsdcc_sps_restore_ep(host, &host->sps.cons);
	if (rc)
		pr_err("%s:msmsdcc_sps_restore_ep(cons) error=%d\n",
				mmc_hostname(host->mmc), rc);
}

static int msmsdcc_bam_dml_reset_and_restore(struct msmsdcc_host *host)
{
	int rc;

	
	rc = msmsdcc_sps_reset_ep(host, &host->sps.prod);
	if (rc) {
		pr_err("%s: msmsdcc_sps_reset_ep(prod) error=%d\n",
				mmc_hostname(host->mmc), rc);
		goto out;
	}

	rc = msmsdcc_sps_reset_ep(host, &host->sps.cons);
	if (rc) {
		pr_err("%s: msmsdcc_sps_reset_ep(cons) error=%d\n",
				mmc_hostname(host->mmc), rc);
		goto out;
	}

	
	rc = sps_device_reset(host->sps.bam_handle);
	if (rc) {
		pr_err("%s: sps_device_reset error=%d\n",
				mmc_hostname(host->mmc), rc);
		goto out;
	}

	memset(host->sps.prod.config.desc.base, 0x00,
			host->sps.prod.config.desc.size);
	memset(host->sps.cons.config.desc.base, 0x00,
			host->sps.cons.config.desc.size);

	
	rc = msmsdcc_sps_restore_ep(host, &host->sps.prod);
	if (rc) {
		pr_err("%s: msmsdcc_sps_restore_ep(prod) error=%d\n",
				mmc_hostname(host->mmc), rc);
		goto out;
	}
	rc = msmsdcc_sps_restore_ep(host, &host->sps.cons);
	if (rc) {
		pr_err("%s: msmsdcc_sps_restore_ep(cons) error=%d\n",
				mmc_hostname(host->mmc), rc);
		goto out;
	}

	
	rc = msmsdcc_dml_init(host);
	if (rc)
		pr_err("%s: msmsdcc_dml_init error=%d\n",
				mmc_hostname(host->mmc), rc);

out:
	if (!rc)
		host->sps.reset_bam = false;
	return rc;
}

static void msmsdcc_soft_reset(struct msmsdcc_host *host)
{
	if (is_sw_reset_save_config(host)) {
		ktime_t start;

		writel_relaxed(readl_relaxed(host->base + MMCIPOWER)
				| MCI_SW_RST_CFG, host->base + MMCIPOWER);
		msmsdcc_sync_reg_wr(host);

		start = ktime_get();
		while (readl_relaxed(host->base + MMCIPOWER) & MCI_SW_RST_CFG) {
			if (ktime_to_us(ktime_sub(ktime_get(), start)) > 1000) {
				pr_err("%s: %s failed\n",
					mmc_hostname(host->mmc), __func__);
				BUG();
			}
		}
	} else {
		writel_relaxed(0, host->base + MMCICOMMAND);
		msmsdcc_sync_reg_wr(host);
		writel_relaxed(0, host->base + MMCIDATACTRL);
		msmsdcc_sync_reg_wr(host);
	}
}

static void msmsdcc_hard_reset(struct msmsdcc_host *host)
{
	int ret;

	if (is_sw_hard_reset(host)) {
		ktime_t start;

		writel_relaxed(readl_relaxed(host->base + MMCIPOWER)
				| MCI_SW_RST, host->base + MMCIPOWER);
		msmsdcc_sync_reg_wr(host);

		start = ktime_get();
		while (readl_relaxed(host->base + MMCIPOWER) & MCI_SW_RST) {
			if (ktime_to_us(ktime_sub(ktime_get(), start)) > 1000) {
				pr_err("%s: %s failed\n",
					mmc_hostname(host->mmc), __func__);
				BUG();
			}
		}
	} else {
		ret = clk_reset(host->clk, CLK_RESET_ASSERT);
		if (ret)
			pr_err("%s: Clock assert failed at %u Hz" \
				" with err %d\n", mmc_hostname(host->mmc),
				host->clk_rate, ret);

		ret = clk_reset(host->clk, CLK_RESET_DEASSERT);
		if (ret)
			pr_err("%s: Clock deassert failed at %u Hz" \
				" with err %d\n", mmc_hostname(host->mmc),
				host->clk_rate, ret);

		mb();
		
		msmsdcc_delay(host);
	}
}

static void msmsdcc_reset_and_restore(struct msmsdcc_host *host)
{
	if (is_soft_reset(host)) {
		if (is_mmc_platform(host->plat)) {
			if (is_sps_mode(host))
			    host->sps.reset_bam = true;

			msmsdcc_soft_reset(host);

			pr_info("%s: Applied soft reset to Controller\n",
					mmc_hostname(host->mmc));
			if (host->mmc->card && mmc_card_mmc(host->mmc->card)) {
				pr_info("%s: cid %08x%08x%08x%08x (%s)\n",
					mmc_hostname(host->mmc->card->host),
					host->mmc->card->raw_cid[0], host->mmc->card->raw_cid[1],
					host->mmc->card->raw_cid[2], host->mmc->card->raw_cid[3], host->mmc->card->cid.prod_name);
				pr_info("%s: csd %08x%08x%08x%08x\n",
					mmc_hostname(host->mmc->card->host),
					host->mmc->card->raw_csd[0], host->mmc->card->raw_csd[1],
					host->mmc->card->raw_csd[2], host->mmc->card->raw_csd[3]);
				if (host->mmc->card->ext_csd.fwrev)
					pr_info("%s: fw_ver %s\n",
						mmc_hostname(host->mmc->card->host),
						host->mmc->card->ext_csd.fwrev);
			}
		} else {
			if (is_sps_mode(host)) {
				
				msmsdcc_dml_reset(host);
				host->sps.pipe_reset_pending = true;
			}
			mb();

			msmsdcc_soft_reset(host);
			pr_info("%s: Applied soft reset to Controller\n",
					mmc_hostname(host->mmc));
			if (is_sps_mode(host))
				msmsdcc_dml_init(host);
		}
	} else {
		
		u32	mci_clk = 0;
		u32	mci_mask0 = 0;
		u32	dll_config = 0;

		
		mci_clk = readl_relaxed(host->base + MMCICLOCK);
		mci_mask0 = readl_relaxed(host->base + MMCIMASK0);
		host->pwr = readl_relaxed(host->base + MMCIPOWER);
		if (host->tuning_needed)
			dll_config = readl_relaxed(host->base + MCI_DLL_CONFIG);
		mb();

		msmsdcc_hard_reset(host);
		pr_info("%s: Applied hard reset to controller\n",
				mmc_hostname(host->mmc));

		
		writel_relaxed(host->pwr, host->base + MMCIPOWER);
		msmsdcc_sync_reg_wr(host);
		writel_relaxed(mci_clk, host->base + MMCICLOCK);
		msmsdcc_sync_reg_wr(host);
		writel_relaxed(mci_mask0, host->base + MMCIMASK0);
		if (host->tuning_needed)
			writel_relaxed(dll_config, host->base + MCI_DLL_CONFIG);
		mb(); 

		if (is_sps_mode(host))
			host->sps.reset_bam = true;
	}

	if (host->dummy_52_needed)
		host->dummy_52_needed = 0;
}
#ifdef CONFIG_WIMAX
EXPORT_SYMBOL(msmsdcc_reset_and_restore);
#endif

static void msmsdcc_reset_dpsm(struct msmsdcc_host *host)
{
	struct mmc_request *mrq = host->curr.mrq;

	if (!mrq || !mrq->cmd || !mrq->data)
		goto out;

	if (mrq->data->flags & MMC_DATA_WRITE) {
		if (!msmsdcc_is_wait_for_prog_done(host, mrq)) {
			if (is_wait_for_tx_rx_active(host) &&
			    !is_auto_prog_done(host))
				pr_warning("%s: %s: AUTO_PROG_DONE capability is must\n",
					   mmc_hostname(host->mmc), __func__);
			goto no_polling;
		}
	}

	
	if (is_wait_for_tx_rx_active(host)) {
		ktime_t start = ktime_get();

		while (readl_relaxed(host->base + MMCISTATUS) &
				(MCI_TXACTIVE | MCI_RXACTIVE)) {
			if (ktime_to_us(ktime_sub(ktime_get(), start)) > 1000) {
				pr_err("%s: %s still active\n",
					mmc_hostname(host->mmc),
					readl_relaxed(host->base + MMCISTATUS)
					& MCI_TXACTIVE ? "TX" : "RX");
				msmsdcc_dump_sdcc_state(host);
				msmsdcc_reset_and_restore(host);
				goto out;
			}
		}
	}

no_polling:
	writel_relaxed(0, host->base + MMCIDATACTRL);
	msmsdcc_sync_reg_wr(host); 
out:
	return;
}

static int
msmsdcc_request_end(struct msmsdcc_host *host, struct mmc_request *mrq)
{
	int retval = 0;

	BUG_ON(host->curr.data);

	del_timer(&host->req_tout_timer);

	if (mrq->data)
		mrq->data->bytes_xfered = host->curr.data_xfered;
	if (mrq->cmd->error == -ETIMEDOUT)
		mdelay(5);

	msmsdcc_reset_dpsm(host);

	
	memset(&host->curr, 0, sizeof(struct msmsdcc_curr_req));

	spin_unlock(&host->lock);
	mmc_request_done(host->mmc, mrq);
	spin_lock(&host->lock);

	return retval;
}

static void
msmsdcc_stop_data(struct msmsdcc_host *host)
{
	host->curr.data = NULL;
	host->curr.got_dataend = 0;
	host->curr.wait_for_auto_prog_done = false;
	host->curr.got_auto_prog_done = false;
}

static inline uint32_t msmsdcc_fifo_addr(struct msmsdcc_host *host)
{
	return host->core_memres->start + MMCIFIFO;
}

static inline unsigned int msmsdcc_get_min_sup_clk_rate(
					struct msmsdcc_host *host);

static inline void msmsdcc_sync_reg_wr(struct msmsdcc_host *host)
{
	mb();
	if (!is_wait_for_reg_write(host))
		udelay(host->reg_write_delay);
	else if (readl_relaxed(host->base + MCI_STATUS2) &
			MCI_MCLK_REG_WR_ACTIVE) {
		ktime_t start, diff;

		start = ktime_get();
		while (readl_relaxed(host->base + MCI_STATUS2) &
			MCI_MCLK_REG_WR_ACTIVE) {
			diff = ktime_sub(ktime_get(), start);
			
			if (ktime_to_us(diff) > 1000) {
				pr_warning("%s: previous reg. write is"
					" still active\n",
					mmc_hostname(host->mmc));
				break;
			}
		}
	}
}

static inline void msmsdcc_delay(struct msmsdcc_host *host)
{
	udelay(host->reg_write_delay);

}

static inline void
msmsdcc_start_command_exec(struct msmsdcc_host *host, u32 arg, u32 c)
{
	writel_relaxed(arg, host->base + MMCIARGUMENT);
	writel_relaxed(c, host->base + MMCICOMMAND);
	mb();
}

static void
msmsdcc_dma_exec_func(struct msm_dmov_cmd *cmd)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)cmd->user;

	writel_relaxed(host->cmd_timeout, host->base + MMCIDATATIMER);
	writel_relaxed((unsigned int)host->curr.xfer_size,
			host->base + MMCIDATALENGTH);
	writel_relaxed(host->cmd_datactrl, host->base + MMCIDATACTRL);
	msmsdcc_sync_reg_wr(host); 

	if (host->cmd_cmd) {
		msmsdcc_start_command_exec(host,
			(u32)host->cmd_cmd->arg, (u32)host->cmd_c);
	}
}

static void
msmsdcc_dma_complete_tlet(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	unsigned long		flags;
	struct mmc_request	*mrq;

	spin_lock_irqsave(&host->lock, flags);
	mrq = host->curr.mrq;
	BUG_ON(!mrq);

	if (!(host->dma.result & DMOV_RSLT_VALID)) {
		pr_err("msmsdcc: Invalid DataMover result\n");
		goto out;
	}

	if (host->dma.result & DMOV_RSLT_DONE) {
		host->curr.data_xfered = host->curr.xfer_size;
		host->curr.xfer_remain -= host->curr.xfer_size;
	} else {
		
		if (host->dma.result & DMOV_RSLT_ERROR)
			pr_err("%s: DMA error (0x%.8x)\n",
			       mmc_hostname(host->mmc), host->dma.result);
		if (host->dma.result & DMOV_RSLT_FLUSH)
			pr_err("%s: DMA channel flushed (0x%.8x)\n",
			       mmc_hostname(host->mmc), host->dma.result);
		pr_err("Flush data: %.8x %.8x %.8x %.8x %.8x %.8x\n",
		       host->dma.err.flush[0], host->dma.err.flush[1],
		       host->dma.err.flush[2], host->dma.err.flush[3],
		       host->dma.err.flush[4],
		       host->dma.err.flush[5]);
		msmsdcc_reset_and_restore(host);
		if (!mrq->data->error)
			mrq->data->error = -EIO;
	}
	if (!mrq->data->host_cookie)
		dma_unmap_sg(mmc_dev(host->mmc), host->dma.sg,
			     host->dma.num_ents, host->dma.dir);

	if (host->curr.user_pages) {
		struct scatterlist *sg = host->dma.sg;
		int i;

		for (i = 0; i < host->dma.num_ents; i++, sg++)
			flush_dcache_page(sg_page(sg));
	}

	host->dma.sg = NULL;
	host->dma.busy = 0;

	if ((host->curr.got_dataend && (!host->curr.wait_for_auto_prog_done ||
		(host->curr.wait_for_auto_prog_done &&
		host->curr.got_auto_prog_done))) || mrq->data->error) {

		if (!mrq->data->error) {
			host->curr.data_xfered = host->curr.xfer_size;
			host->curr.xfer_remain -= host->curr.xfer_size;
		}
		if (host->dummy_52_needed) {
			mrq->data->bytes_xfered = host->curr.data_xfered;
			host->dummy_52_sent = 1;
			msmsdcc_start_command(host, &dummy52cmd,
					      MCI_CPSM_PROGENA);
			goto out;
		}
		msmsdcc_stop_data(host);
		if (!mrq->data->stop || mrq->cmd->error ||
			(mrq->sbc && !mrq->data->error)) {
			mrq->data->bytes_xfered = host->curr.data_xfered;
			msmsdcc_reset_dpsm(host);
			del_timer(&host->req_tout_timer);
			memset(&host->curr, 0, sizeof(struct msmsdcc_curr_req));
			spin_unlock_irqrestore(&host->lock, flags);

			mmc_request_done(host->mmc, mrq);
			return;
		} else if (mrq->data->stop && ((mrq->sbc && mrq->data->error)
				|| !mrq->sbc)) {
			msmsdcc_start_command(host, mrq->data->stop, 0);
		}
	}

out:
	spin_unlock_irqrestore(&host->lock, flags);
	return;
}

#ifdef CONFIG_MMC_MSM_SPS_SUPPORT
static void
msmsdcc_sps_complete_cb(struct sps_event_notify *notify)
{
	struct msmsdcc_host *host =
		(struct msmsdcc_host *)
		((struct sps_event_notify *)notify)->user;

	host->sps.notify = *notify;
	pr_debug("%s: %s: sps ev_id=%d, addr=0x%x, size=0x%x, flags=0x%x\n",
		mmc_hostname(host->mmc), __func__, notify->event_id,
		notify->data.transfer.iovec.addr,
		notify->data.transfer.iovec.size,
		notify->data.transfer.iovec.flags);
	
	tasklet_schedule(&host->sps.tlet);
}

static void msmsdcc_sps_complete_tlet(unsigned long data)
{
	unsigned long flags;
	int i, rc;
	u32 data_xfered = 0;
	struct mmc_request *mrq;
	struct sps_iovec iovec;
	struct sps_pipe *sps_pipe_handle;
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	struct sps_event_notify *notify = &host->sps.notify;

	spin_lock_irqsave(&host->lock, flags);
	if (host->sps.dir == DMA_FROM_DEVICE)
		sps_pipe_handle = host->sps.prod.pipe_handle;
	else
		sps_pipe_handle = host->sps.cons.pipe_handle;
	mrq = host->curr.mrq;

	if (!mrq) {
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	pr_debug("%s: %s: sps event_id=%d\n",
		mmc_hostname(host->mmc), __func__,
		notify->event_id);

	for (i = 0; i < host->sps.xfer_req_cnt; i++) {
		rc = sps_get_iovec(sps_pipe_handle, &iovec);
		if (rc) {
			pr_err("%s: %s: sps_get_iovec() failed rc=%d, i=%d",
				mmc_hostname(host->mmc), __func__, rc, i);
			break;
		}
		data_xfered += iovec.size;
	}

	if (data_xfered == host->curr.xfer_size) {
		host->curr.data_xfered = host->curr.xfer_size;
		host->curr.xfer_remain -= host->curr.xfer_size;
		pr_debug("%s: Data xfer success. data_xfered=0x%x",
			mmc_hostname(host->mmc),
			host->curr.xfer_size);
	} else {
		pr_err("%s: Data xfer failed. data_xfered=0x%x,"
			" xfer_size=%d", mmc_hostname(host->mmc),
			data_xfered, host->curr.xfer_size);
		msmsdcc_reset_and_restore(host);
		if (!mrq->data->error)
			mrq->data->error = -EIO;
	}

	
	if (!mrq->data->host_cookie)
		dma_unmap_sg(mmc_dev(host->mmc), host->sps.sg,
			     host->sps.num_ents, host->sps.dir);
	host->sps.sg = NULL;
	host->sps.busy = 0;

	if ((host->curr.got_dataend && (!host->curr.wait_for_auto_prog_done ||
		(host->curr.wait_for_auto_prog_done &&
		host->curr.got_auto_prog_done))) || mrq->data->error) {

		if (!mrq->data->error) {
			host->curr.data_xfered = host->curr.xfer_size;
			host->curr.xfer_remain -= host->curr.xfer_size;
		}
		if (host->dummy_52_needed) {
			mrq->data->bytes_xfered = host->curr.data_xfered;
			host->dummy_52_sent = 1;
			msmsdcc_start_command(host, &dummy52cmd,
					      MCI_CPSM_PROGENA);
			spin_unlock_irqrestore(&host->lock, flags);
			return;
		}
		msmsdcc_stop_data(host);
		if (!mrq->data->stop || mrq->cmd->error ||
			(mrq->sbc && !mrq->data->error)) {
			mrq->data->bytes_xfered = host->curr.data_xfered;
			msmsdcc_reset_dpsm(host);
			del_timer(&host->req_tout_timer);
			memset(&host->curr, 0, sizeof(struct msmsdcc_curr_req));
			spin_unlock_irqrestore(&host->lock, flags);

			mmc_request_done(host->mmc, mrq);
			return;
		} else if (mrq->data->stop && ((mrq->sbc && mrq->data->error)
				|| !mrq->sbc)) {
			msmsdcc_start_command(host, mrq->data->stop, 0);
		}
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static void msmsdcc_sps_exit_curr_xfer(struct msmsdcc_host *host)
{
	struct mmc_request *mrq;

	mrq = host->curr.mrq;
	BUG_ON(!mrq);

	msmsdcc_reset_and_restore(host);
	if (!mrq->data->error)
		mrq->data->error = -EIO;

	
	if (!mrq->data->host_cookie)
		dma_unmap_sg(mmc_dev(host->mmc), host->sps.sg,
			     host->sps.num_ents, host->sps.dir);

	host->sps.sg = NULL;
	host->sps.busy = 0;
	if (host->curr.data)
		msmsdcc_stop_data(host);

	if (!mrq->data->stop || mrq->cmd->error ||
		(mrq->sbc && !mrq->data->error))
		msmsdcc_request_end(host, mrq);
	else if (mrq->data->stop && ((mrq->sbc && mrq->data->error)
			|| !mrq->sbc))
		msmsdcc_start_command(host, mrq->data->stop, 0);

}
#else
static inline void msmsdcc_sps_complete_cb(struct sps_event_notify *notify) { }
static inline void msmsdcc_sps_complete_tlet(unsigned long data) { }
static inline void msmsdcc_sps_exit_curr_xfer(struct msmsdcc_host *host) { }
#endif 

static int msmsdcc_enable_cdr_cm_sdc4_dll(struct msmsdcc_host *host);

static void
msmsdcc_dma_complete_func(struct msm_dmov_cmd *cmd,
			  unsigned int result,
			  struct msm_dmov_errdata *err)
{
	struct msmsdcc_dma_data	*dma_data =
		container_of(cmd, struct msmsdcc_dma_data, hdr);
	struct msmsdcc_host *host = dma_data->host;

	dma_data->result = result;
	if (err)
		memcpy(&dma_data->err, err, sizeof(struct msm_dmov_errdata));

	tasklet_schedule(&host->dma_tlet);
}

static bool msmsdcc_is_dma_possible(struct msmsdcc_host *host,
				    struct mmc_data *data)
{
	bool ret = true;
	u32 xfer_size = data->blksz * data->blocks;

	if (is_sps_mode(host)) {
		if (xfer_size <= SPS_MIN_XFER_SIZE)
			ret = false;
	} else if (is_dma_mode(host)) {
		if (xfer_size % MCI_FIFOSIZE)
			ret = false;
	} else {
		
		ret = false;
	}

	return ret;
}

static int msmsdcc_config_dma(struct msmsdcc_host *host, struct mmc_data *data)
{
	struct msmsdcc_nc_dmadata *nc;
	dmov_box *box;
	uint32_t rows;
	unsigned int n;
	int i, err = 0, box_cmd_cnt = 0;
	struct scatterlist *sg = data->sg;
	unsigned int len, offset;

	if ((host->dma.channel == -1) || (host->dma.crci == -1))
		return -ENOENT;

	BUG_ON((host->pdev_id < 1) || (host->pdev_id > 5));

	host->dma.sg = data->sg;
	host->dma.num_ents = data->sg_len;

	
	BUG_ON(host->dma.num_ents > msmsdcc_get_nr_sg(host));

	nc = host->dma.nc;

	if (data->flags & MMC_DATA_READ)
		host->dma.dir = DMA_FROM_DEVICE;
	else
		host->dma.dir = DMA_TO_DEVICE;

	if (!data->host_cookie) {
		n = msmsdcc_prep_xfer(host, data);
		if (unlikely(n < 0)) {
			host->dma.sg = NULL;
			host->dma.num_ents = 0;
			return -ENOMEM;
		}
	}

	
	host->curr.user_pages = 0;
	box = &nc->cmd[0];
	for (i = 0; i < host->dma.num_ents; i++) {
		len = sg_dma_len(sg);
		offset = 0;

		do {
			
			if (!len || (box_cmd_cnt >= MMC_MAX_DMA_CMDS)) {
				err = -ENOTSUPP;
				goto unmap;
			}

			box->cmd = CMD_MODE_BOX;

			if (len >= MMC_MAX_DMA_BOX_LENGTH) {
				len = MMC_MAX_DMA_BOX_LENGTH;
				len -= len % data->blksz;
			}
			rows = (len % MCI_FIFOSIZE) ?
				(len / MCI_FIFOSIZE) + 1 :
				(len / MCI_FIFOSIZE);

			if (data->flags & MMC_DATA_READ) {
				box->src_row_addr = msmsdcc_fifo_addr(host);
				box->dst_row_addr = sg_dma_address(sg) + offset;
				box->src_dst_len = (MCI_FIFOSIZE << 16) |
						(MCI_FIFOSIZE);
				box->row_offset = MCI_FIFOSIZE;
				box->num_rows = rows * ((1 << 16) + 1);
				box->cmd |= CMD_SRC_CRCI(host->dma.crci);
			} else {
				box->src_row_addr = sg_dma_address(sg) + offset;
				box->dst_row_addr = msmsdcc_fifo_addr(host);
				box->src_dst_len = (MCI_FIFOSIZE << 16) |
						(MCI_FIFOSIZE);
				box->row_offset = (MCI_FIFOSIZE << 16);
				box->num_rows = rows * ((1 << 16) + 1);
				box->cmd |= CMD_DST_CRCI(host->dma.crci);
			}

			offset += len;
			len = sg_dma_len(sg) - offset;
			box++;
			box_cmd_cnt++;
		} while (len);
		sg++;
	}
	
	box--;
	box->cmd |= CMD_LC;

	
	BUG_ON(host->dma.cmd_busaddr & 0x07);

	nc->cmdptr = (host->dma.cmd_busaddr >> 3) | CMD_PTR_LP;
	host->dma.hdr.cmdptr = DMOV_CMD_PTR_LIST |
			       DMOV_CMD_ADDR(host->dma.cmdptr_busaddr);
	host->dma.hdr.complete_func = msmsdcc_dma_complete_func;

	
	mb();

unmap:
	if (err) {
		if (!data->host_cookie)
			dma_unmap_sg(mmc_dev(host->mmc), host->dma.sg,
				     host->dma.num_ents, host->dma.dir);
		pr_err("%s: cannot do DMA, fall back to PIO mode err=%d\n",
				mmc_hostname(host->mmc), err);
	}

	return err;
}

static int msmsdcc_prep_xfer(struct msmsdcc_host *host,
			     struct mmc_data *data)
{
	int rc = 0;
	unsigned int dir;

	
	BUG_ON(data->sg_len > msmsdcc_get_nr_sg(host));

	if (data->flags & MMC_DATA_READ)
		dir = DMA_FROM_DEVICE;
	else
		dir = DMA_TO_DEVICE;

	
	rc = dma_map_sg(mmc_dev(host->mmc), data->sg, data->sg_len,
			dir);

	if (unlikely(rc != data->sg_len)) {
		pr_err("%s: Unable to map in all sg elements, rc=%d\n",
		       mmc_hostname(host->mmc), rc);
		rc = -ENOMEM;
		goto dma_map_err;
	}

	pr_debug("%s: %s: %s: sg_len=%d\n",
		mmc_hostname(host->mmc), __func__,
		dir == DMA_FROM_DEVICE ? "READ" : "WRITE",
		data->sg_len);

	goto out;

dma_map_err:
	dma_unmap_sg(mmc_dev(host->mmc), data->sg, data->sg_len,
		     data->flags);
out:
	return rc;
}
#ifdef CONFIG_MMC_MSM_SPS_SUPPORT
static int msmsdcc_sps_start_xfer(struct msmsdcc_host *host,
				  struct mmc_data *data)
{
	int rc = 0;
	u32 flags;
	int i;
	u32 addr, len, data_cnt;
	struct scatterlist *sg = data->sg;
	struct sps_pipe *sps_pipe_handle;

	host->sps.sg = data->sg;
	host->sps.num_ents = data->sg_len;
	host->sps.xfer_req_cnt = 0;
	if (data->flags & MMC_DATA_READ) {
		host->sps.dir = DMA_FROM_DEVICE;
		sps_pipe_handle = host->sps.prod.pipe_handle;
	} else {
		host->sps.dir = DMA_TO_DEVICE;
		sps_pipe_handle = host->sps.cons.pipe_handle;
	}

	if (!data->host_cookie) {
		rc = msmsdcc_prep_xfer(host, data);
		if (unlikely(rc < 0)) {
			host->dma.sg = NULL;
			host->dma.num_ents = 0;
			goto out;
		}
	}

	for (i = 0; i < data->sg_len; i++) {
		len = sg_dma_len(sg);
		addr = sg_dma_address(sg);
		flags = 0;
		while (len > 0) {
			if (len > SPS_MAX_DESC_SIZE) {
				data_cnt = SPS_MAX_DESC_SIZE;
			} else {
				data_cnt = len;
				if (i == data->sg_len - 1)
					flags = SPS_IOVEC_FLAG_INT |
						SPS_IOVEC_FLAG_EOT;
			}
			rc = sps_transfer_one(sps_pipe_handle, addr,
						data_cnt, host, flags);
			if (rc) {
				pr_err("%s: sps_transfer_one() error! rc=%d,"
					" pipe=0x%x, sg=0x%x, sg_buf_no=%d\n",
					mmc_hostname(host->mmc), rc,
					(u32)sps_pipe_handle, (u32)sg, i);
				goto dma_map_err;
			}
			addr += data_cnt;
			len -= data_cnt;
			host->sps.xfer_req_cnt++;
		}
		sg++;
	}
	goto out;

dma_map_err:
	
	if (!data->host_cookie)
		dma_unmap_sg(mmc_dev(host->mmc), host->sps.sg,
			     host->sps.num_ents, host->sps.dir);
out:
	return rc;
}
#else
static int msmsdcc_sps_start_xfer(struct msmsdcc_host *host,
				struct mmc_data *data) { return 0; }
#endif 

static void
msmsdcc_start_command_deferred(struct msmsdcc_host *host,
				struct mmc_command *cmd, u32 *c)
{
	DBG(host, "op %02x arg %08x flags %08x\n",
	    cmd->opcode, cmd->arg, cmd->flags);

	*c |= (cmd->opcode | MCI_CPSM_ENABLE);

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			*c |= MCI_CPSM_LONGRSP;
		*c |= MCI_CPSM_RESPONSE;
	}

	if (0)
		*c |= MCI_CPSM_INTERRUPT;

	
	if (mmc_cmd_type(cmd) == MMC_CMD_ADTC)
		*c |= MCI_CSPM_DATCMD;

	
	if (host->tuning_needed && host->en_auto_cmd19 &&
		!(host->mmc->ios.timing == MMC_TIMING_MMC_HS200)) {

		if ((cmd->opcode == MMC_SET_BLOCK_COUNT &&
			host->curr.mrq->cmd->opcode ==
				MMC_READ_MULTIPLE_BLOCK) ||
			(!host->curr.mrq->sbc &&
			(cmd->opcode == MMC_READ_SINGLE_BLOCK ||
			cmd->opcode == MMC_READ_MULTIPLE_BLOCK))) {
			msmsdcc_enable_cdr_cm_sdc4_dll(host);
			if (host->en_auto_cmd19 &&
			    host->mmc->ios.timing == MMC_TIMING_UHS_SDR104)
				*c |= MCI_CSPM_AUTO_CMD19;
		}
	}

	if (cmd->mrq->data && (cmd->mrq->data->flags & MMC_DATA_READ))
		writel_relaxed((readl_relaxed(host->base +
				MCI_DLL_CONFIG) | MCI_CDR_EN),
				host->base + MCI_DLL_CONFIG);
	else
		
		writel_relaxed((readl_relaxed(host->base +
				MCI_DLL_CONFIG) & ~MCI_CDR_EN),
				host->base + MCI_DLL_CONFIG);

	if ((cmd->flags & MMC_RSP_R1B) == MMC_RSP_R1B) {
		*c |= MCI_CPSM_PROGENA;
		host->prog_enable = 1;
	}
	if (cmd == cmd->mrq->stop)
		*c |= MCI_CSPM_MCIABORT;

	if (host->curr.cmd != NULL) {
		pr_err("%s: Overlapping command requests\n",
		       mmc_hostname(host->mmc));
	}
	host->curr.cmd = cmd;
}

static void
msmsdcc_start_data(struct msmsdcc_host *host, struct mmc_data *data,
			struct mmc_command *cmd, u32 c)
{
	unsigned int datactrl = 0, timeout;
	unsigned long long clks;
	void __iomem *base = host->base;
	unsigned int pio_irqmask = 0;

	BUG_ON(!data->sg);
	BUG_ON(!data->sg_len);

	host->curr.data = data;
	host->curr.xfer_size = data->blksz * data->blocks;
	host->curr.xfer_remain = host->curr.xfer_size;
	host->curr.data_xfered = 0;
	host->curr.got_dataend = 0;
	host->curr.got_auto_prog_done = false;

	datactrl = MCI_DPSM_ENABLE | (data->blksz << 4);

	if (host->curr.wait_for_auto_prog_done)
		datactrl |= MCI_AUTO_PROG_DONE;

	if (msmsdcc_is_dma_possible(host, data)) {
		if (is_dma_mode(host) && !msmsdcc_config_dma(host, data)) {
			datactrl |= MCI_DPSM_DMAENABLE;
		} else if (is_sps_mode(host)) {
			if (!msmsdcc_sps_start_xfer(host, data)) {
				
				mb();
				msmsdcc_dml_start_xfer(host, data);
				datactrl |= MCI_DPSM_DMAENABLE;
				host->sps.busy = 1;
			}
		}
	}

	
	if (!(datactrl & MCI_DPSM_DMAENABLE)) {
		if (data->flags & MMC_DATA_READ) {
			pio_irqmask = MCI_RXFIFOHALFFULLMASK;
			if (host->curr.xfer_remain < MCI_FIFOSIZE)
				pio_irqmask |= MCI_RXDATAAVLBLMASK;
		} else
			pio_irqmask = MCI_TXFIFOHALFEMPTYMASK |
					MCI_TXFIFOEMPTYMASK;

		msmsdcc_sg_start(host);
	}

	if (data->flags & MMC_DATA_READ)
		datactrl |= (MCI_DPSM_DIRECTION | MCI_RX_DATA_PEND);
	else if (host->curr.use_wr_data_pend)
		datactrl |= MCI_DATA_PEND;

	clks = (unsigned long long)data->timeout_ns * host->clk_rate;
	do_div(clks, 1000000000UL);
	timeout = data->timeout_clks + (unsigned int)clks*2 ;
	WARN(!timeout,
	     "%s: data timeout is zero. timeout_ns=0x%x, timeout_clks=0x%x\n",
	     mmc_hostname(host->mmc), data->timeout_ns, data->timeout_clks);

	if (is_dma_mode(host) && (datactrl & MCI_DPSM_DMAENABLE)) {
		
		
		host->cmd_timeout = timeout;
		host->cmd_pio_irqmask = pio_irqmask;
		host->cmd_datactrl = datactrl;
		host->cmd_cmd = cmd;

		host->dma.hdr.exec_func = msmsdcc_dma_exec_func;
		host->dma.hdr.user = (void *)host;
		host->dma.busy = 1;

		if (cmd) {
			msmsdcc_start_command_deferred(host, cmd, &c);
			host->cmd_c = c;
		}
		writel_relaxed((readl_relaxed(host->base + MMCIMASK0) &
				(~(MCI_IRQ_PIO))) | host->cmd_pio_irqmask,
				host->base + MMCIMASK0);
		mb();
		msm_dmov_enqueue_cmd_ext(host->dma.channel, &host->dma.hdr);
	} else {
		
		writel_relaxed(timeout, base + MMCIDATATIMER);

		writel_relaxed(host->curr.xfer_size, base + MMCIDATALENGTH);

		writel_relaxed((readl_relaxed(host->base + MMCIMASK0) &
				(~(MCI_IRQ_PIO))) | pio_irqmask,
				host->base + MMCIMASK0);
		writel_relaxed(datactrl, base + MMCIDATACTRL);

		if (cmd) {
			
			msmsdcc_sync_reg_wr(host);
			
			msmsdcc_start_command(host, cmd, c);
		} else {
			mb();
		}
	}
}

static void
msmsdcc_start_command(struct msmsdcc_host *host, struct mmc_command *cmd, u32 c)
{
	msmsdcc_start_command_deferred(host, cmd, &c);
	msmsdcc_start_command_exec(host, cmd->arg, c);
}

static void
msmsdcc_data_err(struct msmsdcc_host *host, struct mmc_data *data,
		 unsigned int status)
{
	if (status & MCI_DATACRCFAIL) {
		if (!(data->mrq->cmd->opcode == MMC_BUS_TEST_W
			|| data->mrq->cmd->opcode == MMC_BUS_TEST_R
			|| data->mrq->cmd->opcode ==
				MMC_SEND_TUNING_BLOCK_HS200)) {
			pr_err("%s: Data CRC error\n",
			       mmc_hostname(host->mmc));
			pr_err("%s: opcode 0x%.8x\n", __func__,
			       data->mrq->cmd->opcode);
			pr_err("%s: blksz %d, blocks %d\n", __func__,
			       data->blksz, data->blocks);
			if(is_sd_platform(host->plat))
				msmsdcc_print_pin_info(host);
			data->error = -EILSEQ;
			msmsdcc_dump_sdcc_state(host);
		}
		
		if (host->tuning_needed && !host->tuning_in_progress)
			host->tuning_done = false;
	} else if (status & MCI_DATATIMEOUT) {
		if (!(data->mrq->cmd->opcode == MMC_BUS_TEST_W
			|| data->mrq->cmd->opcode == MMC_BUS_TEST_R)) {
			pr_err("%s: CMD%d: Data timeout. DAT0 => %d\n",
				 mmc_hostname(host->mmc),
				 data->mrq->cmd->opcode,
				 (readl_relaxed(host->base
				 + MCI_TEST_INPUT) & 0x2) ? 1 : 0);
			data->error = -ETIMEDOUT;
			msmsdcc_dump_sdcc_state(host);
		}
		
		if (host->tuning_needed && !host->tuning_in_progress)
			host->tuning_done = false;
	} else if (status & MCI_RXOVERRUN) {
		pr_err("%s: RX overrun\n", mmc_hostname(host->mmc));
		data->error = -EIO;
	} else if (status & MCI_TXUNDERRUN) {
		pr_err("%s: TX underrun\n", mmc_hostname(host->mmc));
		data->error = -EIO;
	} else {
		pr_err("%s: Unknown error (0x%.8x)\n",
		      mmc_hostname(host->mmc), status);
		data->error = -EIO;
	}

	
	if (host->dummy_52_needed)
		host->dummy_52_needed = 0;
}

static int
msmsdcc_pio_read(struct msmsdcc_host *host, char *buffer, unsigned int remain)
{
	void __iomem	*base = host->base;
	uint32_t	*ptr = (uint32_t *) buffer;
	int		count = 0;

	if (remain % 4)
		remain = ((remain >> 2) + 1) << 2;

	while (readl_relaxed(base + MMCISTATUS) & MCI_RXDATAAVLBL) {

		*ptr = readl_relaxed(base + MMCIFIFO + (count % MCI_FIFOSIZE));
		ptr++;
		count += sizeof(uint32_t);

		remain -=  sizeof(uint32_t);
		if (remain == 0)
			break;
	}
	return count;
}

static int
msmsdcc_pio_write(struct msmsdcc_host *host, char *buffer,
		  unsigned int remain)
{
	void __iomem *base = host->base;
	char *ptr = buffer;
	unsigned int maxcnt = MCI_FIFOHALFSIZE;

	while (readl_relaxed(base + MMCISTATUS) &
		(MCI_TXFIFOEMPTY | MCI_TXFIFOHALFEMPTY)) {
		unsigned int count, sz;

		count = min(remain, maxcnt);

		sz = count % 4 ? (count >> 2) + 1 : (count >> 2);
		writesl(base + MMCIFIFO, ptr, sz);
		ptr += count;
		remain -= count;

		if (remain == 0)
			break;
	}
	mb();

	return ptr - buffer;
}

/*
 * Copy up to a word (4 bytes) between a scatterlist
 * and a temporary bounce buffer when the word lies across
 * two pages. The temporary buffer can then be read to/
 * written from the FIFO once.
 */
static void _msmsdcc_sg_consume_word(struct msmsdcc_host *host)
{
	struct msmsdcc_pio_data *pio = &host->pio;
	unsigned int bytes_avail;

	if (host->curr.data->flags & MMC_DATA_READ)
		memcpy(pio->sg_miter.addr, pio->bounce_buf,
		       pio->bounce_buf_len);
	else
		memcpy(pio->bounce_buf, pio->sg_miter.addr,
		       pio->bounce_buf_len);

	while (pio->bounce_buf_len != 4) {
		if (!sg_miter_next(&pio->sg_miter))
			break;
		bytes_avail = min_t(unsigned int, pio->sg_miter.length,
			4 - pio->bounce_buf_len);
		if (host->curr.data->flags & MMC_DATA_READ)
			memcpy(pio->sg_miter.addr,
			       &pio->bounce_buf[pio->bounce_buf_len],
			       bytes_avail);
		else
			memcpy(&pio->bounce_buf[pio->bounce_buf_len],
			       pio->sg_miter.addr, bytes_avail);

		pio->sg_miter.consumed = bytes_avail;
		pio->bounce_buf_len += bytes_avail;
	}
}

static int msmsdcc_sg_next(struct msmsdcc_host *host, char **buf, int *len)
{
	struct msmsdcc_pio_data *pio = &host->pio;
	unsigned int length, rlength;
	char *buffer;

	if (!sg_miter_next(&pio->sg_miter))
		return 0;

	buffer = pio->sg_miter.addr;
	length = pio->sg_miter.length;

	if (length < host->curr.xfer_remain) {
		rlength = round_down(length, 4);
		if (rlength) {
			length = rlength;
			goto sg_next_end;
		}
		pio->bounce_buf_len = length;
		memset(pio->bounce_buf, 0, 4);

		if (host->curr.data->flags & MMC_DATA_READ) {
			buffer = pio->bounce_buf;
			length = 4;
			goto sg_next_end;
		} else {
			_msmsdcc_sg_consume_word(host);
			buffer = pio->bounce_buf;
			length = pio->bounce_buf_len;
		}
	}

sg_next_end:
	*buf = buffer;
	*len = length;
	return 1;
}

static void msmsdcc_sg_consumed(struct msmsdcc_host *host,
				unsigned int length)
{
	struct msmsdcc_pio_data *pio = &host->pio;

	if (host->curr.data->flags & MMC_DATA_READ) {
		if (length > pio->sg_miter.consumed)
			_msmsdcc_sg_consume_word(host);
		else
			pio->sg_miter.consumed = length;
	} else
		if (length < pio->sg_miter.consumed)
			pio->sg_miter.consumed = length;
}

static void msmsdcc_sg_start(struct msmsdcc_host *host)
{
	unsigned int sg_miter_flags = SG_MITER_ATOMIC;

	host->pio.bounce_buf_len = 0;

	if (host->curr.data->flags & MMC_DATA_READ)
		sg_miter_flags |= SG_MITER_TO_SG;
	else
		sg_miter_flags |= SG_MITER_FROM_SG;

	sg_miter_start(&host->pio.sg_miter, host->curr.data->sg,
		       host->curr.data->sg_len, sg_miter_flags);
}

static void msmsdcc_sg_stop(struct msmsdcc_host *host)
{
	sg_miter_stop(&host->pio.sg_miter);
}

static irqreturn_t
msmsdcc_pio_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;
	void __iomem		*base = host->base;
	uint32_t		status;
	unsigned long flags;
	unsigned int remain;
	char *buffer;

	spin_lock(&host->lock);

	status = readl_relaxed(base + MMCISTATUS);

	if (((readl_relaxed(host->base + MMCIMASK0) & status) &
				(MCI_IRQ_PIO)) == 0) {
		spin_unlock(&host->lock);
		return IRQ_NONE;
	}
#if IRQ_DEBUG
	msmsdcc_print_status(host, "irq1-r", status);
#endif
	local_irq_save(flags);

	do {
		unsigned int len;

		if (!(status & (MCI_TXFIFOHALFEMPTY | MCI_TXFIFOEMPTY
				| MCI_RXDATAAVLBL)))
			break;

		if (!msmsdcc_sg_next(host, &buffer, &remain))
			break;

		len = 0;
		if (status & MCI_RXACTIVE)
			len = msmsdcc_pio_read(host, buffer, remain);
		if (status & MCI_TXACTIVE)
			len = msmsdcc_pio_write(host, buffer, remain);

		
		if (len > remain)
			len = remain;

		host->curr.xfer_remain -= len;
		host->curr.data_xfered += len;
		remain -= len;
		msmsdcc_sg_consumed(host, len);

		if (remain) 
			break; 

		status = readl_relaxed(base + MMCISTATUS);
	} while (1);

	msmsdcc_sg_stop(host);
	local_irq_restore(flags);

	if (status & MCI_RXACTIVE && host->curr.xfer_remain < MCI_FIFOSIZE) {
		writel_relaxed((readl_relaxed(host->base + MMCIMASK0) &
				(~(MCI_IRQ_PIO))) | MCI_RXDATAAVLBLMASK,
				host->base + MMCIMASK0);
		if (!host->curr.xfer_remain) {
			writel_relaxed((readl_relaxed(host->base + MMCIMASK0) &
				(~(MCI_IRQ_PIO))) | 0, host->base + MMCIMASK0);
		}
		mb();
	} else if (!host->curr.xfer_remain) {
		writel_relaxed((readl_relaxed(host->base + MMCIMASK0) &
				(~(MCI_IRQ_PIO))) | 0, host->base + MMCIMASK0);
		mb();
	}

	spin_unlock(&host->lock);

	return IRQ_HANDLED;
}

static void
msmsdcc_request_start(struct msmsdcc_host *host, struct mmc_request *mrq);

static void msmsdcc_wait_for_rxdata(struct msmsdcc_host *host,
					struct mmc_data *data)
{
	u32 loop_cnt = 0;

	while (((int)host->curr.xfer_remain > 0) && (++loop_cnt < 1000)) {
		if (readl_relaxed(host->base + MMCISTATUS) & MCI_RXDATAAVLBL) {
			spin_unlock(&host->lock);
			msmsdcc_pio_irq(1, host);
			spin_lock(&host->lock);
		}
	}
	if (loop_cnt == 1000) {
		pr_info("%s: Timed out while polling for Rx Data\n",
				mmc_hostname(host->mmc));
		data->error = -ETIMEDOUT;
		msmsdcc_reset_and_restore(host);
	}
}

static void msmsdcc_do_cmdirq(struct msmsdcc_host *host, uint32_t status)
{
	struct mmc_command *cmd = host->curr.cmd;

	host->curr.cmd = NULL;
	if (mmc_resp_type(cmd))
		cmd->resp[0] = readl_relaxed(host->base + MMCIRESPONSE0);
	if (mmc_resp_type(cmd) & MMC_RSP_136) {
		cmd->resp[1] = readl_relaxed(host->base + MMCIRESPONSE1);
		cmd->resp[2] = readl_relaxed(host->base + MMCIRESPONSE2);
		cmd->resp[3] = readl_relaxed(host->base + MMCIRESPONSE3);
	}

	if (status & (MCI_CMDTIMEOUT | MCI_AUTOCMD19TIMEOUT)) {
		if (is_sd_platform(host->plat)) {
			if (cmd->opcode != MMC_SLEEP_AWAKE && cmd->opcode != 8 &&
				cmd->opcode != SD_IO_RW_DIRECT) {
				pr_err("%s: CMD%d: Command timeout\n",
					mmc_hostname(host->mmc), cmd->opcode);
				msmsdcc_dump_sdcc_state(host);
			}
		} else
			pr_debug("%s: CMD%d: Command timeout\n",
				mmc_hostname(host->mmc), cmd->opcode);
		cmd->error = -ETIMEDOUT;
	} else if ((status & MCI_CMDCRCFAIL && cmd->flags & MMC_RSP_CRC) &&
			!host->tuning_in_progress) {
		pr_err("%s: CMD%d: Command CRC error\n",
			mmc_hostname(host->mmc), cmd->opcode);
		msmsdcc_dump_sdcc_state(host);
		
		if (host->tuning_needed)
			host->tuning_done = false;
		if(is_sd_platform(host->plat))
			msmsdcc_print_pin_info(host);
		cmd->error = -EILSEQ;
	}

	if (!cmd->error) {
		if (cmd->cmd_timeout_ms > host->curr.req_tout_ms) {
			host->curr.req_tout_ms = cmd->cmd_timeout_ms;
			mod_timer(&host->req_tout_timer, (jiffies +
				  msecs_to_jiffies(host->curr.req_tout_ms)));
		}
	}

	if (!cmd->data || cmd->error) {
		if (host->curr.data && host->dma.sg &&
			is_dma_mode(host))
			msm_dmov_flush(host->dma.channel, 0);
		else if (host->curr.data && host->sps.sg &&
			is_sps_mode(host)) {
			
			msmsdcc_sps_exit_curr_xfer(host);
		}
		else if (host->curr.data) { 
			msmsdcc_reset_and_restore(host);
			msmsdcc_stop_data(host);
			msmsdcc_request_end(host, cmd->mrq);
		} else { 
			if (!cmd->error && host->prog_enable) {
				if (status & MCI_PROGDONE) {
					host->prog_enable = 0;
					msmsdcc_request_end(host, cmd->mrq);
				} else
					host->curr.cmd = cmd;
			} else {
				host->prog_enable = 0;
				host->curr.wait_for_auto_prog_done = false;
				if (host->dummy_52_needed)
					host->dummy_52_needed = 0;
				if (cmd->data && cmd->error)
					msmsdcc_reset_and_restore(host);
				msmsdcc_request_end(host, cmd->mrq);
			}
		}
	} else if (cmd->data) {
		if (cmd == host->curr.mrq->sbc)
			msmsdcc_start_command(host, host->curr.mrq->cmd, 0);
		else if ((cmd->data->flags & MMC_DATA_WRITE) &&
			   !host->curr.use_wr_data_pend)
			msmsdcc_start_data(host, cmd->data, NULL, 0);
	}
}

static irqreturn_t
msmsdcc_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;
	u32			status;
	int			ret = 0;
	int			timer = 0;

#ifdef CONFIG_WIFI_IRQ_DEBUG
	static unsigned long irq_count = 0;
#endif
#ifdef CONFIG_WIMAX
	static unsigned long irq_count_wimax = 0;

if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s msmsdcc_irq+++\n", mmc_hostname(host->mmc), __func__);

	if (is_wimax_platform(host->plat)) {
		if (mmc_wimax_get_irq_log()) {
			if (host->irq_time_wimax == 0)
				host->irq_time_wimax = jiffies + HZ;
			irq_count_wimax++;
		}
	}
#endif

#ifdef CONFIG_WIFI_IRQ_DEBUG
	if (is_wifi_platform(host->plat)) {
		if (host->irq_time == 0)
			host->irq_time = jiffies + HZ;
		irq_count++;
	}
#endif

	spin_lock(&host->lock);

	do {
		struct mmc_command *cmd;
		struct mmc_data *data;

		if (timer) {
			timer = 0;
			msmsdcc_delay(host);
		}

		if (!atomic_read(&host->clks_on)) {
			pr_debug("%s: %s: SDIO async irq received\n",
					mmc_hostname(host->mmc), __func__);
#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			pr_info("%s: %s: SDIO async irq received\n",
					mmc_hostname(host->mmc), __func__);
#endif

			if (!host->sdcc_irq_disabled) {
				disable_irq_nosync(irq);
				host->sdcc_irq_disabled = 1;
			}

			if (host->sdcc_suspended) {
				if (host->plat->sdiowakeup_irq)
					wake_lock(&host->sdio_wlock);
			} else {
				spin_unlock(&host->lock);
				mmc_signal_sdio_irq(host->mmc);
				spin_lock(&host->lock);
			}
			ret = 1;
			break;
		}

		status = readl_relaxed(host->base + MMCISTATUS);

		if (((readl_relaxed(host->base + MMCIMASK0) & status) &
						(~(MCI_IRQ_PIO))) == 0)
			break;

#if IRQ_DEBUG
		msmsdcc_print_status(host, "irq0-r", status);
#endif
		status &= readl_relaxed(host->base + MMCIMASK0);
		writel_relaxed(status, host->base + MMCICLEAR);
		
		if (host->clk_rate <=
				msmsdcc_get_min_sup_clk_rate(host))
			msmsdcc_sync_reg_wr(host);
#if IRQ_DEBUG
		msmsdcc_print_status(host, "irq0-p", status);
#endif

		if (status & MCI_SDIOINTROPE) {
			if (host->sdcc_suspending)
				wake_lock(&host->sdio_suspend_wlock);
			spin_unlock(&host->lock);
			mmc_signal_sdio_irq(host->mmc);
			spin_lock(&host->lock);
		}
		data = host->curr.data;

		if (host->dummy_52_sent) {
			if (status & (MCI_PROGDONE | MCI_CMDCRCFAIL |
					  MCI_CMDTIMEOUT)) {
				if (status & MCI_CMDTIMEOUT)
					pr_debug("%s: dummy CMD52 timeout\n",
						mmc_hostname(host->mmc));
				if (status & MCI_CMDCRCFAIL)
					pr_debug("%s: dummy CMD52 CRC failed\n",
						mmc_hostname(host->mmc));
				host->dummy_52_sent = 0;
				host->dummy_52_needed = 0;
				if (data) {
					msmsdcc_stop_data(host);
					msmsdcc_request_end(host, data->mrq);
				}
				WARN(!data, "No data cmd for dummy CMD52\n");
				spin_unlock(&host->lock);
				return IRQ_HANDLED;
			}
			break;
		}

		cmd = host->curr.cmd;
		if ((status & (MCI_CMDSENT | MCI_CMDRESPEND | MCI_CMDCRCFAIL |
			MCI_CMDTIMEOUT | MCI_PROGDONE |
			MCI_AUTOCMD19TIMEOUT)) && host->curr.cmd) {
			msmsdcc_do_cmdirq(host, status);
		}

		if (host->curr.data) {
			
			if (status & (MCI_DATACRCFAIL|MCI_DATATIMEOUT|
				      MCI_TXUNDERRUN|MCI_RXOVERRUN)) {
				msmsdcc_data_err(host, data, status);
				host->curr.data_xfered = 0;
				if (host->dma.sg && is_dma_mode(host))
					msm_dmov_flush(host->dma.channel, 0);
				else if (host->sps.sg && is_sps_mode(host)) {
					
					msmsdcc_sps_exit_curr_xfer(host);
				} else {
					msmsdcc_reset_and_restore(host);
					if (host->curr.data)
						msmsdcc_stop_data(host);
					if (!data->stop || (host->curr.mrq->sbc
						&& !data->error))
						timer |=
						 msmsdcc_request_end(host,
								    data->mrq);
					else if ((host->curr.mrq->sbc
						&& data->error) ||
						!host->curr.mrq->sbc) {
						msmsdcc_start_command(host,
								     data->stop,
								     0);
						timer = 1;
					}
				}
			}

			
			if (host->curr.wait_for_auto_prog_done &&
				(status & MCI_PROGDONE))
				host->curr.got_auto_prog_done = true;

			
			if (!host->curr.got_dataend && (status & MCI_DATAEND))
				host->curr.got_dataend = 1;

			if (host->curr.got_dataend &&
				(!host->curr.wait_for_auto_prog_done ||
				(host->curr.wait_for_auto_prog_done &&
				host->curr.got_auto_prog_done))) {
				if (!host->dma.busy && !host->sps.busy) {
					if (data->flags & MMC_DATA_READ)
						msmsdcc_wait_for_rxdata(host,
								data);
					if (!data->error) {
						host->curr.data_xfered =
							host->curr.xfer_size;
						host->curr.xfer_remain -=
							host->curr.xfer_size;
					}

					if (!host->dummy_52_needed) {
						msmsdcc_stop_data(host);
						if (!data->stop ||
							(host->curr.mrq->sbc
							&& !data->error))
							msmsdcc_request_end(
								  host,
								  data->mrq);
						else if ((host->curr.mrq->sbc
							&& data->error) ||
							!host->curr.mrq->sbc) {
							msmsdcc_start_command(
								host,
								data->stop, 0);
							timer = 1;
						}
					} else {
						host->dummy_52_sent = 1;
						msmsdcc_start_command(host,
							&dummy52cmd,
							MCI_CPSM_PROGENA);
					}
				}
			}
		}

		ret = 1;
	} while (status);

	spin_unlock(&host->lock);

#ifdef CONFIG_WIFI_IRQ_DEBUG
	if (is_wifi_platform(host->plat) && time_after(jiffies, host->irq_time)) {
		pr_info("[WIFI][MMC] %s: %s irq count %lu\n",
			mmc_hostname(host->mmc), __func__, irq_count);
		host->irq_time = jiffies + HZ;
	}
#endif
#ifdef CONFIG_WIMAX
	if (mmc_wimax_get_irq_log()) {
		if (is_wimax_platform(host->plat) && time_after(jiffies, host->irq_time_wimax)) {
			pr_info("[WIMAX][MMC] %s: %s irq count %lu\n",
				mmc_hostname(host->mmc), __func__, irq_count_wimax);
			host->irq_time_wimax = jiffies + HZ;
		}
	}
#endif

	return IRQ_RETVAL(ret);
}

static void
msmsdcc_pre_req(struct mmc_host *mmc, struct mmc_request *mrq,
		bool is_first_request)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	struct mmc_data *data = mrq->data;
	int rc = 0;

	if (unlikely(!data)) {
		pr_err("%s: %s cannot prepare null data\n", mmc_hostname(mmc),
		       __func__);
		return;
	}
	if (unlikely(data->host_cookie)) {
		
		data->host_cookie = 0;
		pr_err("%s: %s Request reposted for prepare\n",
		       mmc_hostname(mmc), __func__);
		return;
	}

	if (!msmsdcc_is_dma_possible(host, data))
		return;

	rc = msmsdcc_prep_xfer(host, data);
	if (unlikely(rc < 0)) {
		data->host_cookie = 0;
		return;
	}

	data->host_cookie = 1;
}

static void
msmsdcc_post_req(struct mmc_host *mmc, struct mmc_request *mrq, int err)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned int dir;
	struct mmc_data *data = mrq->data;

	if (unlikely(!data)) {
		pr_err("%s: %s cannot cleanup null data\n", mmc_hostname(mmc),
		       __func__);
		return;
	}
	if (data->flags & MMC_DATA_READ)
		dir = DMA_FROM_DEVICE;
	else
		dir = DMA_TO_DEVICE;

	if (data->host_cookie)
		dma_unmap_sg(mmc_dev(host->mmc), data->sg,
			     data->sg_len, dir);

	data->host_cookie = 0;
}

static void
msmsdcc_request_start(struct msmsdcc_host *host, struct mmc_request *mrq)
{
	if (mrq->data) {
		
		if ((mrq->data->flags & MMC_DATA_READ) ||
		    host->curr.use_wr_data_pend)
			msmsdcc_start_data(host, mrq->data,
					   mrq->sbc ? mrq->sbc : mrq->cmd,
					   0);
		else
			msmsdcc_start_command(host,
					      mrq->sbc ? mrq->sbc : mrq->cmd,
					      0);
	} else {
		msmsdcc_start_command(host, mrq->cmd, 0);
	}
}

static bool msmsdcc_is_wait_for_auto_prog_done(struct msmsdcc_host *host,
					       struct mmc_request *mrq)
{
	if (is_auto_prog_done(host) && (mrq->sbc || !mrq->stop))
		return true;

	return false;
}

static bool msmsdcc_is_wait_for_prog_done(struct msmsdcc_host *host,
					  struct mmc_request *mrq)
{
	if (msmsdcc_is_wait_for_auto_prog_done(host, mrq) || mrq->stop)
		return true;

	return false;
}

static void
msmsdcc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long		flags;
	int retries = 5;

	WARN(host->dummy_52_sent, "Dummy CMD52 in progress\n");
	if (host->plat->is_sdio_al_client)
		msmsdcc_sdio_al_lpm(mmc, false);

	if (is_mmc_platform(host->plat)) {
		
		if (is_sps_mode(host) && host->sps.reset_bam) {
			while (retries) {
				if (!msmsdcc_bam_dml_reset_and_restore(host))
					break;
				pr_err("%s: msmsdcc_bam_dml_reset_and_restore returned error. %d attempts left.\n",
					mmc_hostname(host->mmc), --retries);
			}
		}
	} else {
		
		if (is_sps_mode(host) && host->sps.pipe_reset_pending) {
			msmsdcc_sps_pipes_reset_and_restore(host);
			host->sps.pipe_reset_pending = false;
		}
	}

	if (host->tuning_needed && !host->tuning_in_progress &&
		!host->tuning_done) {
		if (host->mmc->ios.timing == MMC_TIMING_UHS_SDR104) {
			if (host->cont_tuning_cnt <= MAX_CONTINUOUS_TUNING_COUNT) 
				printk(KERN_ERR "%s: do tuning due to SDR104 before rquest", mmc_hostname(mmc));
			msmsdcc_execute_tuning(mmc,
						   MMC_SEND_TUNING_BLOCK);
		} else if (host->mmc->ios.timing == MMC_TIMING_MMC_HS200)
			msmsdcc_execute_tuning(mmc,
						   MMC_SEND_TUNING_BLOCK_HS200);
	}

	spin_lock_irqsave(&host->lock, flags);

	if (host->eject) {
		if (mrq->data && !(mrq->data->flags & MMC_DATA_READ)) {
			mrq->cmd->error = 0;
			mrq->data->bytes_xfered = mrq->data->blksz *
						  mrq->data->blocks;
		} else
			mrq->cmd->error = -ENOMEDIUM;

		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(mmc, mrq);
		return;
	}

#ifdef CONFIG_WIMAX
	if (is_sd_platform(host->plat) || is_wimax_platform(host->plat)) {
#else
	if (is_sd_platform(host->plat)) {
#endif
		spin_unlock_irqrestore(&host->lock, flags);
		mutex_lock(&host->rpm_mutex);
		if (!atomic_read(&host->clks_on) || host->sdcc_irq_disabled) {
			pr_info("%s: %s irq or clock is off in I/O time\n", mmc_hostname(mmc), __func__);
			msmsdcc_print_rpm_info(host);
		}
		if (!atomic_read(&host->clks_on)) {
			pr_err("%s: %s Find clock is disabled, Enable it.\n", mmc_hostname(mmc),
					__func__);
			mmc_host_clk_hold(mmc);
			mmc->ios.clock = host->clk_rate;
			mmc_set_ios(mmc);
			mmc_host_clk_release(mmc);
		}
		spin_lock_irqsave(&host->lock, flags);
		if (host->sdcc_irq_disabled) {
			pr_err("%s: %s Find irq is disabled, Enable it.\n", mmc_hostname(mmc),
								__func__);
			if (host->sdcc_irq_disabled) {
				struct irq_desc *desc = irq_to_desc(host->core_irqres->start);
				enable_irq(host->core_irqres->start);
				host->sdcc_irq_disabled = 0;
				if (desc->depth != 0)
					pr_info("%s: [enable irq]find woring irq depth %d in %s\n", mmc_hostname(mmc), desc->depth, __func__);
			}
		}
		spin_unlock_irqrestore(&host->lock, flags);
		mutex_unlock(&host->rpm_mutex);
		spin_lock_irqsave(&host->lock, flags);
	}

	if (!host->pwr || !atomic_read(&host->clks_on) ||
			host->sdcc_irq_disabled ||
			host->sps.reset_bam) {
		WARN(1, "%s: %s: SDCC is in bad state. don't process"
		     " new request (CMD%d)\n", mmc_hostname(host->mmc),
		     __func__, mrq->cmd->opcode);
		msmsdcc_dump_sdcc_state(host);
		mrq->cmd->error = -EIO;
		if (mrq->data) {
			mrq->data->error = -EIO;
			mrq->data->bytes_xfered = 0;
		}
		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(mmc, mrq);
		return;
	}

	WARN(host->curr.mrq, "%s: %s: New request (CMD%d) received while"
	     " other request (CMD%d) is in progress\n",
	     mmc_hostname(host->mmc), __func__,
	     mrq->cmd->opcode, host->curr.mrq->cmd->opcode);

	if ((mmc->card) && (mmc->card->quirks & MMC_QUIRK_INAND_DATA_TIMEOUT))
		host->curr.req_tout_ms = 20000;
	else {
		if (is_wifi_platform(host->plat)) {
			host->curr.req_tout_ms = 4000;
		} else {
			host->curr.req_tout_ms = MSM_MMC_REQ_TIMEOUT;
		}
	}
	mod_timer(&host->req_tout_timer,
			(jiffies +
			 msecs_to_jiffies(host->curr.req_tout_ms)));

	host->curr.mrq = mrq;
	if (mrq->sbc) {
		mrq->sbc->mrq = mrq;
		mrq->sbc->data = mrq->data;
	}

	if (mrq->data && (mrq->data->flags & MMC_DATA_WRITE)) {
		if (msmsdcc_is_wait_for_auto_prog_done(host, mrq)) {
			host->curr.wait_for_auto_prog_done = true;
		} else {

			if ((mrq->cmd->opcode == SD_IO_RW_EXTENDED) ||
			    (mrq->cmd->opcode == 54))
#ifdef CONFIG_HTC_DISABLE_DUMMY52
                
#else
                
				host->dummy_52_needed = 1;
#endif
		}

		if ((mrq->cmd->opcode == MMC_WRITE_BLOCK) ||
		    (mrq->cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK))
			host->curr.use_wr_data_pend = true;
	}

	msmsdcc_request_start(host, mrq);

	spin_unlock_irqrestore(&host->lock, flags);
}

static inline int msmsdcc_vreg_set_voltage(struct msm_mmc_reg_data *vreg,
					int min_uV, int max_uV)
{
	int rc = 0;

	if (vreg->set_voltage_sup) {
		rc = regulator_set_voltage(vreg->reg, min_uV, max_uV);
		if (rc) {
			pr_err("%s: regulator_set_voltage(%s) failed."
				" min_uV=%d, max_uV=%d, rc=%d\n",
				__func__, vreg->name, min_uV, max_uV, rc);
		}
	}

	return rc;
}

static inline int msmsdcc_vreg_get_voltage(struct msm_mmc_reg_data *vreg)
{
	int rc = 0;

	rc = regulator_get_voltage(vreg->reg);
	if (rc < 0)
		pr_err("%s: regulator_get_voltage(%s) failed. rc=%d\n",
			__func__, vreg->name, rc);

	return rc;
}

static inline int msmsdcc_vreg_set_optimum_mode(struct msm_mmc_reg_data *vreg,
						int uA_load)
{
	int rc = 0;

	if (vreg->set_voltage_sup) {
		rc = regulator_set_optimum_mode(vreg->reg, uA_load);
		if (rc < 0)
			pr_err("%s: regulator_set_optimum_mode(reg=%s, "
				"uA_load=%d) failed. rc=%d\n", __func__,
				vreg->name, uA_load, rc);
		else
			rc = 0;
	}

	return rc;
}

static inline int msmsdcc_vreg_init_reg(struct msm_mmc_reg_data *vreg,
				struct device *dev)
{
	int rc = 0;

	
	if (vreg->reg)
		goto out;

	
	vreg->reg = regulator_get(dev, vreg->name);
	if (IS_ERR(vreg->reg)) {
		rc = PTR_ERR(vreg->reg);
		pr_err("%s: regulator_get(%s) failed. rc=%d\n",
			__func__, vreg->name, rc);
		goto out;
	}

	if (regulator_count_voltages(vreg->reg) > 0) {
		vreg->set_voltage_sup = 1;
		
		if (!vreg->high_vol_level || !vreg->hpm_uA) {
			pr_err("%s: %s invalid constraints specified\n",
					__func__, vreg->name);
			rc = -EINVAL;
		}
	}

out:
	return rc;
}

static inline void msmsdcc_vreg_deinit_reg(struct msm_mmc_reg_data *vreg)
{
	if (vreg->reg)
		regulator_put(vreg->reg);
}

static int msmsdcc_vreg_init(struct msmsdcc_host *host, bool is_init)
{
	int rc = 0;
	struct msm_mmc_slot_reg_data *curr_slot;
	struct msm_mmc_reg_data *curr_vdd_reg, *curr_vdd_io_reg;
	struct device *dev = mmc_dev(host->mmc);

	curr_slot = host->plat->vreg_data;
	if (!curr_slot)
		goto out;

	curr_vdd_reg = curr_slot->vdd_data;
	curr_vdd_io_reg = curr_slot->vdd_io_data;

	if (is_init) {
		if (curr_vdd_reg) {
			rc = msmsdcc_vreg_init_reg(curr_vdd_reg, dev);
			if (rc)
				goto out;
		}
		if (curr_vdd_io_reg) {
			rc = msmsdcc_vreg_init_reg(curr_vdd_io_reg, dev);
			if (rc)
				goto vdd_reg_deinit;
		}
		rc = msmsdcc_vreg_reset(host);
		if (rc)
			pr_err("msmsdcc.%d vreg reset failed (%d)\n",
			       host->pdev_id, rc);
		goto out;
	} else {
		
		goto vdd_io_reg_deinit;
	}
vdd_io_reg_deinit:
	if (curr_vdd_io_reg)
		msmsdcc_vreg_deinit_reg(curr_vdd_io_reg);
vdd_reg_deinit:
	if (curr_vdd_reg)
		msmsdcc_vreg_deinit_reg(curr_vdd_reg);
out:
	return rc;
}

static int msmsdcc_vreg_enable(struct msm_mmc_reg_data *vreg)
{
	int rc = 0;

	
	rc = msmsdcc_vreg_set_optimum_mode(vreg, vreg->hpm_uA);
	if (rc < 0)
		goto out;

	if (!vreg->is_enabled) {
		
		rc = msmsdcc_vreg_set_voltage(vreg, vreg->high_vol_level,
						vreg->high_vol_level);
		if (rc)
			goto out;

		rc = regulator_enable(vreg->reg);
		if (rc) {
			pr_err("%s: regulator_enable(%s) failed. rc=%d\n",
			__func__, vreg->name, rc);
			goto out;
		}
		vreg->is_enabled = true;
	}

out:
	return rc;
}

static int msmsdcc_vreg_disable(struct msm_mmc_reg_data *vreg, bool is_init)
{
	int rc = 0;

	
	if (vreg->is_enabled && !vreg->always_on) {
		rc = regulator_disable(vreg->reg);
		if (rc) {
			pr_err("%s: regulator_disable(%s) failed. rc=%d\n",
				__func__, vreg->name, rc);
			goto out;
		}
		vreg->is_enabled = false;

		rc = msmsdcc_vreg_set_optimum_mode(vreg, 0);
		if (rc < 0)
			goto out;

		
		rc = msmsdcc_vreg_set_voltage(vreg, 0, vreg->high_vol_level);
		if (rc)
			goto out;
	} else if (vreg->is_enabled && vreg->always_on) {
		if (!is_init && vreg->lpm_sup) {
			
			rc = msmsdcc_vreg_set_optimum_mode(vreg, vreg->lpm_uA);
			if (rc < 0)
				goto out;
		} else if (is_init && vreg->reset_at_init) {
			rc = regulator_disable(vreg->reg);
			if (rc) {
				pr_err("%s: regulator_disable(%s) failed at " \
					"bootup. rc=%d\n", __func__,
					vreg->name, rc);
				goto out;
			}
			vreg->is_enabled = false;
		}
	}
out:
	return rc;
}

static int msmsdcc_setup_vreg(struct msmsdcc_host *host, bool enable,
		bool is_init)
{
	int rc = 0, i;
	struct msm_mmc_slot_reg_data *curr_slot;
	struct msm_mmc_reg_data *vreg_table[2];

	curr_slot = host->plat->vreg_data;
	if (!curr_slot)
		goto out;

	vreg_table[0] = curr_slot->vdd_data;
	vreg_table[1] = curr_slot->vdd_io_data;
	if(is_sd_platform(host->plat) && (enable ^ vreg_table[0]->is_enabled)) {
		mdelay(10);
		pr_info("%s: %s SD slot power\n",
			enable ? "Enabling" : "Disabling", mmc_hostname(host->mmc));
	}

	for (i = 0; i < ARRAY_SIZE(vreg_table); i++) {
		if (vreg_table[i]) {
			if (enable)
				rc = msmsdcc_vreg_enable(vreg_table[i]);
			else
				rc = msmsdcc_vreg_disable(vreg_table[i],
						is_init);
			if (rc)
				goto out;
		}
	}
out:
	return rc;
}

static int msmsdcc_vreg_reset(struct msmsdcc_host *host)
{
	int rc;

	rc = msmsdcc_setup_vreg(host, 1, true);
	if (rc)
		return rc;
	rc = msmsdcc_setup_vreg(host, 0, true);
	return rc;
}

enum vdd_io_level {
	
	VDD_IO_LOW,
	
	VDD_IO_HIGH,
	VDD_IO_SET_LEVEL,
};

static int msmsdcc_get_vdd_io_vol(struct msmsdcc_host *host)
{
	int rc = 0;

	if (host->plat->vreg_data) {
		struct msm_mmc_reg_data *io_reg =
			host->plat->vreg_data->vdd_io_data;

		if (!io_reg)
			io_reg = host->plat->vreg_data->vdd_data;

		if (io_reg && io_reg->is_enabled)
			rc = msmsdcc_vreg_get_voltage(io_reg);
	}

	return rc;
}

static void msmsdcc_update_io_pad_pwr_switch(struct msmsdcc_host *host)
{
	int rc = 0;
	unsigned long flags;

	if (!is_io_pad_pwr_switch(host))
		return;

	rc = msmsdcc_get_vdd_io_vol(host);

	spin_lock_irqsave(&host->lock, flags);
	if (rc > 0 && rc < 2700000)
		host->io_pad_pwr_switch = 1;
	else
		host->io_pad_pwr_switch = 0;

	if (atomic_read(&host->clks_on)) {
		if (host->io_pad_pwr_switch)
			writel_relaxed((readl_relaxed(host->base + MMCICLOCK) |
					IO_PAD_PWR_SWITCH),
					host->base + MMCICLOCK);
		else
			writel_relaxed((readl_relaxed(host->base + MMCICLOCK) &
					~IO_PAD_PWR_SWITCH),
					host->base + MMCICLOCK);
		msmsdcc_sync_reg_wr(host);
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static int msmsdcc_set_vdd_io_vol(struct msmsdcc_host *host,
				  enum vdd_io_level level,
				  unsigned int voltage_level)
{
	int rc = 0;
	int set_level;

	if (host->plat->vreg_data) {
		struct msm_mmc_reg_data *vdd_io_reg =
			host->plat->vreg_data->vdd_io_data;

		if (vdd_io_reg && vdd_io_reg->is_enabled) {
			switch (level) {
			case VDD_IO_LOW:
				set_level = vdd_io_reg->low_vol_level;
				break;
			case VDD_IO_HIGH:
				set_level = vdd_io_reg->high_vol_level;
				break;
			case VDD_IO_SET_LEVEL:
				set_level = voltage_level;
				break;
			default:
				pr_err("%s: %s: invalid argument level = %d",
				       mmc_hostname(host->mmc), __func__,
				       level);
				rc = -EINVAL;
				goto out;
			}
			rc = msmsdcc_vreg_set_voltage(vdd_io_reg,
						      set_level, set_level);
		}
	}

out:
	return rc;
}

static inline int msmsdcc_is_pwrsave(struct msmsdcc_host *host)
{
	if (host->clk_rate > 400000 && msmsdcc_pwrsave)
		return 1;
	return 0;
}

static int msmsdcc_setup_clocks(struct msmsdcc_host *host, bool enable)
{
	int rc = 0;
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
		pr_info("[WIMAX] [MMC] %s: %s enable=%d\n", mmc_hostname(host->mmc), __func__,enable);
	}
#endif

	if (enable && !atomic_read(&host->clks_on)) {

#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
			mmc_wimax_enable_host_wakeup(0);
			if (mmc_wimax_get_disable_irq_config() == 2)
		        wimax_enable_irq(host->mmc);
		}
#endif
		if (!IS_ERR_OR_NULL(host->bus_clk)) {
			rc = clk_prepare_enable(host->bus_clk);
			if (rc) {
				pr_err("%s: %s: failed to enable the bus-clock with error %d\n",
					mmc_hostname(host->mmc), __func__, rc);
				goto out;
			}
		}
		if (!IS_ERR(host->pclk)) {
			rc = clk_prepare_enable(host->pclk);
			if (rc) {
				pr_err("%s: %s: failed to enable the pclk with error %d\n",
					mmc_hostname(host->mmc), __func__, rc);
				goto disable_bus;
			}
		}

		rc = clk_prepare_enable(host->clk);
		if (rc) {
			pr_err("%s: %s: failed to enable the host-clk with error %d\n",
				mmc_hostname(host->mmc), __func__, rc);
			goto disable_pclk;
		}
		mb();
		msmsdcc_delay(host);
		atomic_set(&host->clks_on, 1);
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			pr_info("[WIMAX] [MMC] %s: %s wimax clk on\n", mmc_hostname(host->mmc), __func__);
#endif
	} else if (!enable && atomic_read(&host->clks_on)) {

#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
			mmc_wimax_enable_host_wakeup(1);
			if (mmc_wimax_get_disable_irq_config() == 2)
				wimax_disable_irq(host->mmc);
		}
#endif
		mb();
		msmsdcc_delay(host);
		clk_disable_unprepare(host->clk);
		if (!IS_ERR(host->pclk))
			clk_disable_unprepare(host->pclk);
		if (!IS_ERR_OR_NULL(host->bus_clk))
			clk_disable_unprepare(host->bus_clk);
		atomic_set(&host->clks_on, 0);

#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			pr_info("[WIMAX] [MMC] %s: %s wimax clk off\n", mmc_hostname(host->mmc), __func__);
#endif
	}
	goto out;

disable_pclk:
	if (!IS_ERR_OR_NULL(host->pclk))
		clk_disable_unprepare(host->pclk);
disable_bus:
	if (!IS_ERR_OR_NULL(host->bus_clk))
		clk_disable_unprepare(host->bus_clk);
out:
	return rc;
}
#ifdef CONFIG_WIMAX
EXPORT_SYMBOL(msmsdcc_setup_clocks);
#endif

static inline unsigned int msmsdcc_get_sup_clk_rate(struct msmsdcc_host *host,
						unsigned int req_clk)
{
	unsigned int sel_clk = -1;

	if (req_clk < msmsdcc_get_min_sup_clk_rate(host)) {
		sel_clk = msmsdcc_get_min_sup_clk_rate(host);
		goto out;
	}

	if (host->plat->sup_clk_table && host->plat->sup_clk_cnt) {
		unsigned char cnt;

		for (cnt = 0; cnt < host->plat->sup_clk_cnt; cnt++) {
			if (host->plat->sup_clk_table[cnt] > req_clk)
				break;
			else if (host->plat->sup_clk_table[cnt] == req_clk) {
				sel_clk = host->plat->sup_clk_table[cnt];
				break;
			} else
				sel_clk = host->plat->sup_clk_table[cnt];
		}
	} else {
		if ((req_clk < host->plat->msmsdcc_fmax) &&
			(req_clk > host->plat->msmsdcc_fmid))
			sel_clk = host->plat->msmsdcc_fmid;
		else
			sel_clk = req_clk;
	}

out:
	return sel_clk;
}

static inline unsigned int msmsdcc_get_min_sup_clk_rate(
				struct msmsdcc_host *host)
{
	if (host->plat->sup_clk_table && host->plat->sup_clk_cnt)
		return host->plat->sup_clk_table[0];
	else
		return host->plat->msmsdcc_fmin;
}

static inline unsigned int msmsdcc_get_max_sup_clk_rate(
				struct msmsdcc_host *host)
{
	if (host->plat->sup_clk_table && host->plat->sup_clk_cnt)
		return host->plat->sup_clk_table[host->plat->sup_clk_cnt - 1];
	else
		return host->plat->msmsdcc_fmax;
}

static int msmsdcc_setup_gpio(struct msmsdcc_host *host, bool enable)
{
	struct msm_mmc_gpio_data *curr;
	int i, rc = 0;

	curr = host->plat->pin_data->gpio_data;
	for (i = 0; i < curr->size; i++) {
		if (enable) {
			if (curr->gpio[i].is_always_on &&
				curr->gpio[i].is_enabled)
				continue;
			rc = gpio_request(curr->gpio[i].no,
						curr->gpio[i].name);
			if (rc) {
				pr_err("%s: gpio_request(%d, %s) failed %d\n",
					mmc_hostname(host->mmc),
					curr->gpio[i].no,
					curr->gpio[i].name, rc);
				goto free_gpios;
			}
			curr->gpio[i].is_enabled = true;
		} else {
			if (curr->gpio[i].is_always_on)
				continue;
			gpio_free(curr->gpio[i].no);
			curr->gpio[i].is_enabled = false;
		}
	}
	goto out;

free_gpios:
	for (; i >= 0; i--) {
		gpio_free(curr->gpio[i].no);
		curr->gpio[i].is_enabled = false;
	}
out:
	return rc;
}

static int msmsdcc_setup_pad(struct msmsdcc_host *host, bool enable)
{
	struct msm_mmc_pad_data *curr;
	int i;

	curr = host->plat->pin_data->pad_data;
	for (i = 0; i < curr->drv->size; i++) {
		if (is_sd_platform(host->plat) && host->mmc->ios.timing == MMC_TIMING_UHS_SDR104) {
			pr_info("mmc SD: %s sd set driving for SDR104\n",__func__);
			if (enable)
				msm_tlmm_set_hdrive(curr->drv->on_SDR104[i].no,
					curr->drv->on_SDR104[i].val);
			else
				msm_tlmm_set_hdrive(curr->drv->off[i].no,
					curr->drv->off[i].val);
		} else {
			if (enable)
				msm_tlmm_set_hdrive(curr->drv->on[i].no,
					curr->drv->on[i].val);
			else
				msm_tlmm_set_hdrive(curr->drv->off[i].no,
					curr->drv->off[i].val);
		}
	}

	for (i = 0; i < curr->pull->size; i++) {
		if (enable)
			msm_tlmm_set_pull(curr->pull->on[i].no,
				curr->pull->on[i].val);
		else
			msm_tlmm_set_pull(curr->pull->off[i].no,
				curr->pull->off[i].val);
	}

	return 0;
}

static u32 msmsdcc_setup_pins(struct msmsdcc_host *host, bool enable)
{
	int rc = 0;

	if(host->plat->config_sdgpio)
		return host->plat->config_sdgpio(enable);

	if (!host->plat->pin_data || host->plat->pin_data->cfg_sts == enable)
		return 0;

	if (host->plat->pin_data->is_gpio)
		rc = msmsdcc_setup_gpio(host, enable);
	else
		rc = msmsdcc_setup_pad(host, enable);

	if (!rc)
		host->plat->pin_data->cfg_sts = enable;

	return rc;
}

static int msmsdcc_cfg_mpm_sdiowakeup(struct msmsdcc_host *host,
				      unsigned mode)
{
	int ret = 0;
	unsigned int pin = host->plat->mpm_sdiowakeup_int;

	if (!pin)
		return 0;

	switch (mode) {
	case SDC_DAT1_DISABLE:
		ret = msm_mpm_enable_pin(pin, 0);
		break;
	case SDC_DAT1_ENABLE:
		ret = msm_mpm_set_pin_type(pin, IRQ_TYPE_LEVEL_LOW);
		ret = msm_mpm_enable_pin(pin, 1);
		break;
	case SDC_DAT1_ENWAKE:
		ret = msm_mpm_set_pin_wake(pin, 1);
		break;
	case SDC_DAT1_DISWAKE:
		ret = msm_mpm_set_pin_wake(pin, 0);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static u32 msmsdcc_setup_pwr(struct msmsdcc_host *host, struct mmc_ios *ios)
{
	u32 pwr = 0;
	int ret = 0;
	struct mmc_host *mmc = host->mmc;

	if (host->plat->translate_vdd && !host->sdio_gpio_lpm)
		ret = host->plat->translate_vdd(mmc_dev(mmc), ios->vdd);
	else if (!host->plat->translate_vdd && !host->sdio_gpio_lpm)
		ret = msmsdcc_setup_vreg(host, !!ios->vdd, false);

	if (ret) {
		pr_err("%s: Failed to setup voltage regulators\n",
				mmc_hostname(host->mmc));
		goto out;
	}

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		pwr = MCI_PWR_OFF;
		msmsdcc_cfg_mpm_sdiowakeup(host, SDC_DAT1_DISABLE);
		if (is_sd_platform(host->plat)) {
			if (!host->eject && mmc->card && mmc_sd_card_uhs(mmc->card))
				msmsdcc_set_vdd_io_vol(host, VDD_IO_LOW, 0);
			else
				msmsdcc_set_vdd_io_vol(host, VDD_IO_HIGH, 0);
		} else
			msmsdcc_set_vdd_io_vol(host, VDD_IO_LOW, 0);
		msmsdcc_update_io_pad_pwr_switch(host);
		msmsdcc_setup_pins(host, false);
		break;
	case MMC_POWER_UP:
		
		pwr = MCI_PWR_UP;
		msmsdcc_cfg_mpm_sdiowakeup(host, SDC_DAT1_ENABLE);

		msmsdcc_set_vdd_io_vol(host, VDD_IO_HIGH, 0);
		msmsdcc_update_io_pad_pwr_switch(host);
		msmsdcc_setup_pins(host, true);
		break;
	case MMC_POWER_ON:
		pwr = MCI_PWR_ON;
		break;
	}

out:
	return pwr;
}

static void msmsdcc_enable_irq_wake(struct msmsdcc_host *host)
{
	unsigned int wakeup_irq;

	wakeup_irq = (host->plat->sdiowakeup_irq) ?
			host->plat->sdiowakeup_irq :
			host->core_irqres->start;

	if (!host->irq_wake_enabled) {
		enable_irq_wake(wakeup_irq);
		host->irq_wake_enabled = true;
	}
}

static void msmsdcc_disable_irq_wake(struct msmsdcc_host *host)
{
	unsigned int wakeup_irq;

	wakeup_irq = (host->plat->sdiowakeup_irq) ?
			host->plat->sdiowakeup_irq :
			host->core_irqres->start;

	if (host->irq_wake_enabled) {
		disable_irq_wake(wakeup_irq);
		host->irq_wake_enabled = false;
	}
}

static unsigned int msmsdcc_get_bw_required(struct msmsdcc_host *host,
					    struct mmc_ios *ios)
{
	unsigned int bw;

	bw = host->clk_rate;
	if (ios->bus_width == MMC_BUS_WIDTH_4)
		bw /= 2;
	else if (ios->bus_width == MMC_BUS_WIDTH_1)
		bw /= 8;

	return bw;
}

static int msmsdcc_msm_bus_get_vote_for_bw(struct msmsdcc_host *host,
					   unsigned int bw)
{
	unsigned int *table = host->plat->msm_bus_voting_data->bw_vecs;
	unsigned int size = host->plat->msm_bus_voting_data->bw_vecs_size;
	int i;

	if (host->msm_bus_vote.is_max_bw_needed && bw)
		return host->msm_bus_vote.max_bw_vote;

	for (i = 0; i < size; i++) {
		if (bw <= table[i])
			break;
	}

	if (i && (i == size))
		i--;

	return i;
}

static int msmsdcc_msm_bus_register(struct msmsdcc_host *host)
{
	int rc = 0;
	struct msm_bus_scale_pdata *use_cases;

	if (host->plat->msm_bus_voting_data &&
	    host->plat->msm_bus_voting_data->use_cases &&
	    host->plat->msm_bus_voting_data->bw_vecs &&
	    host->plat->msm_bus_voting_data->bw_vecs_size) {
		use_cases = host->plat->msm_bus_voting_data->use_cases;
		host->msm_bus_vote.client_handle =
				msm_bus_scale_register_client(use_cases);
	} else {
		return 0;
	}

	if (!host->msm_bus_vote.client_handle) {
		pr_err("%s: msm_bus_scale_register_client() failed\n",
		       mmc_hostname(host->mmc));
		rc = -EFAULT;
	} else {
		
		host->msm_bus_vote.min_bw_vote =
				msmsdcc_msm_bus_get_vote_for_bw(host, 0);
		host->msm_bus_vote.max_bw_vote =
				msmsdcc_msm_bus_get_vote_for_bw(host, UINT_MAX);
	}

	return rc;
}

static void msmsdcc_msm_bus_unregister(struct msmsdcc_host *host)
{
	if (host->msm_bus_vote.client_handle)
		msm_bus_scale_unregister_client(
			host->msm_bus_vote.client_handle);
}

static inline int msmsdcc_msm_bus_set_vote(struct msmsdcc_host *host,
					     int vote,
					     unsigned long flags)
{
	ktime_t start, diff;
	int rc = 0;

	if (vote != host->msm_bus_vote.curr_vote) {
		spin_unlock_irqrestore(&host->lock, flags);
		start = ktime_get();
		rc = msm_bus_scale_client_update_request(
				host->msm_bus_vote.client_handle, vote);
		diff = ktime_sub(ktime_get(), start);
		if (rc)
			pr_err("%s: msm_bus_scale_client_update_request() failed."
			       " bus_client_handle=0x%x, vote=%d, err=%d\n",
			       mmc_hostname(host->mmc),
			       host->msm_bus_vote.client_handle, vote, rc);
		if (ktime_to_ms(diff) > 500)
			pr_info("%s: msm_bus_scale_client_update_request spends %lld ms\n",
					mmc_hostname(host->mmc), ktime_to_ms(diff));
		spin_lock_irqsave(&host->lock, flags);
		if (!rc)
			host->msm_bus_vote.curr_vote = vote;
	}

	return rc;
}

static void msmsdcc_msm_bus_work(struct work_struct *work)
{
	struct msmsdcc_host *host = container_of(work,
					struct msmsdcc_host,
					msm_bus_vote.vote_work.work);
	unsigned long flags;

	if (!host->msm_bus_vote.client_handle)
		return;

	spin_lock_irqsave(&host->lock, flags);
	
	if (!host->curr.mrq)
		msmsdcc_msm_bus_set_vote(host,
			host->msm_bus_vote.min_bw_vote, flags);
	else
		pr_warning("%s: %s: SDCC transfer in progress. skipping"
			   " bus voting to 0 bandwidth\n",
			   mmc_hostname(host->mmc), __func__);
	spin_unlock_irqrestore(&host->lock, flags);
}

static void msmsdcc_msm_bus_cancel_work_and_set_vote(
					struct msmsdcc_host *host,
					struct mmc_ios *ios)
{
	unsigned long flags;
	unsigned int bw;
	int vote;

	if (!host->msm_bus_vote.client_handle)
		return;

	bw = ios ? msmsdcc_get_bw_required(host, ios) : 0;

	cancel_delayed_work_sync(&host->msm_bus_vote.vote_work);
	spin_lock_irqsave(&host->lock, flags);
	vote = msmsdcc_msm_bus_get_vote_for_bw(host, bw);
	msmsdcc_msm_bus_set_vote(host, vote, flags);
	spin_unlock_irqrestore(&host->lock, flags);
}

static void msmsdcc_msm_bus_queue_work(struct msmsdcc_host *host)
{
	unsigned long flags;

	if (!host->msm_bus_vote.client_handle)
		return;

	spin_lock_irqsave(&host->lock, flags);
	if (host->msm_bus_vote.min_bw_vote != host->msm_bus_vote.curr_vote)
		queue_delayed_work(system_nrt_wq,
				   &host->msm_bus_vote.vote_work,
				   msecs_to_jiffies(MSM_MMC_BUS_VOTING_DELAY));
	spin_unlock_irqrestore(&host->lock, flags);
}


static void
msmsdcc_cfg_sdio_wakeup(struct msmsdcc_host *host, bool enable_wakeup_irq)
{
	struct mmc_host *mmc = host->mmc;

	if (!(mmc->card && mmc_card_sdio(mmc->card))
			|| host->plat->is_sdio_al_client)
		goto out;

	if (!host->sdcc_suspended) {
		if (enable_wakeup_irq) {
			writel_relaxed(MCI_SDIOINTMASK, host->base + MMCIMASK0);
			mb();
		} else {
			writel_relaxed(MCI_SDIOINTMASK, host->base + MMCICLEAR);
			msmsdcc_sync_reg_wr(host);
		}
		goto out;
	} else if (!mmc_card_wake_sdio_irq(mmc)) {
		goto out;
	}

	if (enable_wakeup_irq) {
		if (!host->plat->sdiowakeup_irq) {
			writel_relaxed(MCI_SDIOINTMASK,
					host->base + MMCIMASK0);
			mb();
			msmsdcc_cfg_mpm_sdiowakeup(host, SDC_DAT1_ENWAKE);
			
			msmsdcc_enable_irq_wake(host);
		} else {
			
			writel_relaxed(0, host->base + MMCIMASK0);
			mb();
			if (host->sdio_wakeupirq_disabled) {
				host->sdio_wakeupirq_disabled = 0;
				
				msmsdcc_enable_irq_wake(host);
				enable_irq(host->plat->sdiowakeup_irq);
			}
		}
	} else {
		if (!host->plat->sdiowakeup_irq) {
			writel_relaxed(MCI_SDIOINTMASK, host->base + MMCICLEAR);
			msmsdcc_sync_reg_wr(host);
			msmsdcc_cfg_mpm_sdiowakeup(host, SDC_DAT1_DISWAKE);
			msmsdcc_disable_irq_wake(host);
		} else if (!host->sdio_wakeupirq_disabled) {
			disable_irq_nosync(host->plat->sdiowakeup_irq);
			msmsdcc_disable_irq_wake(host);
			host->sdio_wakeupirq_disabled = 1;
		}
	}
out:
	return;
}

static void
msmsdcc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	u32 clk = 0, pwr = 0;
	int rc;
	unsigned long flags;
	unsigned int clock;



	mutex_lock(&host->clk_mutex);
	DBG(host, "ios->clock = %u\n", ios->clock);
	spin_lock_irqsave(&host->lock, flags);

#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s:msmsdcc_set_ios\n", mmc_hostname(host->mmc), __func__);

	if (!host->sdcc_irq_disabled && !(is_wimax_platform(host->plat) && mmc_wimax_get_status())) {
#else
	if (!host->sdcc_irq_disabled) {
#endif
		disable_irq_nosync(host->core_irqres->start);
		host->sdcc_irq_disabled = 1;
	}
	spin_unlock_irqrestore(&host->lock, flags);

	
	synchronize_irq(host->core_irqres->start);

	pwr = msmsdcc_setup_pwr(host, ios);

	spin_lock_irqsave(&host->lock, flags);
	if (ios->clock) {
#ifdef CONFIG_WIMAX
			if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
				mmc_wimax_enable_host_wakeup(0);
				if (mmc_wimax_get_disable_irq_config() == 2)
		            wimax_enable_irq(host->mmc);
			}
#endif
		spin_unlock_irqrestore(&host->lock, flags);
		rc = msmsdcc_setup_clocks(host, true);
		if (rc)
			goto out;

#ifdef CONFIG_WIFI_MMC
	if (is_wifi_platform(host->plat)) {
		
			printk(KERN_INFO "[WIFI] [MMC] %s [WIFI] %s clks is ON\n", __func__, mmc_hostname(host->mmc));
		
	}
#endif
#ifdef CONFIG_WIMAX
	#if SDC_CLK_VERBOSE
			if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
				
					printk(KERN_INFO "[WIMAX] [MMC] %s [WiMAX] %s wimax clk is ON\n", __func__, mmc_hostname(host->mmc));
				
			}
	#endif
#endif

		spin_lock_irqsave(&host->lock, flags);
		writel_relaxed(host->mci_irqenable, host->base + MMCIMASK0);
		mb();
		msmsdcc_cfg_sdio_wakeup(host, false);
		clock = msmsdcc_get_sup_clk_rate(host, ios->clock);

		if (ios->timing == MMC_TIMING_UHS_DDR50) {
			u32 clk;

			clk = readl_relaxed(host->base + MMCICLOCK);
			clk &= ~(0x7 << 14); 
			clk |= (3 << 14); 
			writel_relaxed(clk, host->base + MMCICLOCK);
			msmsdcc_sync_reg_wr(host);

			if (!host->ddr_doubled_clk_rate ||
				(host->ddr_doubled_clk_rate &&
				(host->ddr_doubled_clk_rate != ios->clock))) {
				host->ddr_doubled_clk_rate =
					msmsdcc_get_sup_clk_rate(
						host, (ios->clock * 2));
				clock = host->ddr_doubled_clk_rate;
			}
		} else {
			host->ddr_doubled_clk_rate = 0;
		}

		if (clock != host->clk_rate) {
			if (is_sd_platform(host->plat) && ios->timing == MMC_TIMING_UHS_SDR104) {
				msmsdcc_setup_pad(host, 1);
				pr_info("mmc SD: %s sd set driving for SDR104\n",__func__);
			}
			spin_unlock_irqrestore(&host->lock, flags);
			rc = clk_set_rate(host->clk, clock);
			spin_lock_irqsave(&host->lock, flags);
			if (rc < 0)
				pr_err("%s: failed to set clk rate %u\n",
						mmc_hostname(mmc), clock);
			host->clk_rate = clock;
			host->reg_write_delay =
				(1 + ((3 * USEC_PER_SEC) /
				      (host->clk_rate ? host->clk_rate :
				       msmsdcc_get_min_sup_clk_rate(host))));
		}
		mb();
		msmsdcc_delay(host);
		clk |= MCI_CLK_ENABLE;
#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			pr_info("[WIMAX] [MMC] %s: %s clk |= MCI_CLK_ENABLE\n", mmc_hostname(host->mmc), __func__);
#endif

	}
	if (ios->bus_width == MMC_BUS_WIDTH_8)
		clk |= MCI_CLK_WIDEBUS_8;
	else if (ios->bus_width == MMC_BUS_WIDTH_4)
		clk |= MCI_CLK_WIDEBUS_4;
	else
		clk |= MCI_CLK_WIDEBUS_1;

	if (msmsdcc_is_pwrsave(host))
		clk |= MCI_CLK_PWRSAVE;

	clk |= MCI_CLK_FLOWENA;

	host->tuning_needed = 0;
	
	host->cont_tuning_cnt = 0;
	if (is_wifi_platform(host->plat)) {
		mmc_tuning_fail = 0;
		smp_mb();
	}
	
	if (host->clk_rate > (100 * 1000 * 1000) &&
	    (ios->timing == MMC_TIMING_UHS_SDR104 ||
	    ios->timing == MMC_TIMING_MMC_HS200)) {
		
		clk |= (4 << 14);
		host->tuning_needed = 1;
	} else {
		if (ios->timing == MMC_TIMING_UHS_DDR50)
			clk |= (3 << 14);
		else
			clk |= (2 << 14); 
		host->tuning_done = false;
		if (atomic_read(&host->clks_on)) {
			
			writel_relaxed((readl_relaxed(host->base +
				MCI_DLL_CONFIG) | MCI_DLL_RST),
				host->base + MCI_DLL_CONFIG);
			
			writel_relaxed((readl_relaxed(host->base +
				MCI_DLL_CONFIG) | MCI_DLL_PDN),
				host->base + MCI_DLL_CONFIG);
		}
	}

	
	clk |= (2 << 23);

	
#ifdef CONFIG_ARCH_APQ8064
	if (ios->vdd) {
		if (is_wifi_slot(host->plat) && host->pdev_id == 3) {
			host->io_pad_pwr_switch = 1;
		}
	}
#endif
	

	if (host->io_pad_pwr_switch)
		clk |= IO_PAD_PWR_SWITCH;

	
	if (atomic_read(&host->clks_on)) {
		if (readl_relaxed(host->base + MMCICLOCK) != clk) {
			writel_relaxed(clk, host->base + MMCICLOCK);
			msmsdcc_sync_reg_wr(host);
		}
		if (readl_relaxed(host->base + MMCIPOWER) != pwr) {
			host->pwr = pwr;
			writel_relaxed(pwr, host->base + MMCIPOWER);
			msmsdcc_sync_reg_wr(host);
		}
	}

	if (!(clk & MCI_CLK_ENABLE) && atomic_read(&host->clks_on)) {
		msmsdcc_cfg_sdio_wakeup(host, true);
#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
			mmc_wimax_enable_host_wakeup(1);
			if (mmc_wimax_get_disable_irq_config() == 2)
				wimax_disable_irq(host->mmc);
		}
#endif
		spin_unlock_irqrestore(&host->lock, flags);
		msmsdcc_setup_clocks(host, false);

#ifdef CONFIG_WIFI_MMC
	if (is_wifi_platform(host->plat)) {
		
			printk(KERN_INFO "[WIFI] [MMC] %s [WIFI] %s clks is OFF\n", __func__, mmc_hostname(host->mmc));
		
	}
#endif
#ifdef CONFIG_WIMAX
	#if SDC_CLK_VERBOSE
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
			
				printk(KERN_INFO "[WIMAX] [MMC] %s [WiMAX] %s wimax clks is OFF\n", __func__, mmc_hostname(host->mmc));
		}
	#endif
#endif

		spin_lock_irqsave(&host->lock, flags);
	}

	if (host->tuning_in_progress)
		WARN(!atomic_read(&host->clks_on),
			"tuning_in_progress but SDCC clocks are OFF\n");

	
	if (ios->power_mode != MMC_POWER_OFF && host->sdcc_irq_disabled) {
		if(is_sd_platform(host->plat)) {
			if (atomic_read(&host->clks_on)) {
				enable_irq(host->core_irqres->start);
				host->sdcc_irq_disabled = 0;
			}
#ifdef CONFIG_WIMAX
		} else if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
			pr_info("[WIMAX] [MMC] %s: %s, ios->power_mode=%d clks_on=%d\n", mmc_hostname(host->mmc), __func__,ios->power_mode,atomic_read(&host->clks_on));
			if (atomic_read(&host->clks_on) && (!host->sdcc_irq_disabled)) {
				pr_info("[WIMAX] [MMC] %s: %s wimax enable_irq , host->sdcc_irq_disabled=%d\n", mmc_hostname(host->mmc), __func__,host->sdcc_irq_disabled);
				enable_irq(host->core_irqres->start);
				host->sdcc_irq_disabled = 0;
			}

		} else {
			if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
				pr_info("[WIMAX] [MMC] %s: %s Non-wimax enable_irq ios->power_mode=%d\n", mmc_hostname(host->mmc), __func__,ios->power_mode);
#else
		} else {
#endif
			enable_irq(host->core_irqres->start);
			host->sdcc_irq_disabled = 0;
		}
	}
	spin_unlock_irqrestore(&host->lock, flags);
out:
	mutex_unlock(&host->clk_mutex);
}

int msmsdcc_set_pwrsave(struct mmc_host *mmc, int pwrsave)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	u32 clk;

	clk = readl_relaxed(host->base + MMCICLOCK);
	pr_debug("Changing to pwr_save=%d", pwrsave);
	if (pwrsave && msmsdcc_is_pwrsave(host))
		clk |= MCI_CLK_PWRSAVE;
	else
		clk &= ~MCI_CLK_PWRSAVE;
	writel_relaxed(clk, host->base + MMCICLOCK);
	msmsdcc_sync_reg_wr(host);

	return 0;
}

static int msmsdcc_get_ro(struct mmc_host *mmc)
{
	int status = -ENOSYS;
	struct msmsdcc_host *host = mmc_priv(mmc);

	if (host->plat->wpswitch) {
		status = host->plat->wpswitch(mmc_dev(mmc));
	} else if (host->plat->wpswitch_gpio) {
		status = gpio_request(host->plat->wpswitch_gpio,
					"SD_WP_Switch");
		if (status) {
			pr_err("%s: %s: Failed to request GPIO %d\n",
				mmc_hostname(mmc), __func__,
				host->plat->wpswitch_gpio);
		} else {
			status = gpio_direction_input(
					host->plat->wpswitch_gpio);
			if (!status) {
				msleep(300);
				status = gpio_get_value_cansleep(
						host->plat->wpswitch_gpio);
				status ^= !host->plat->is_wpswitch_active_low;
			}
			gpio_free(host->plat->wpswitch_gpio);
		}
	}

	if (status < 0)
		status = -ENOSYS;
	pr_debug("%s: Card read-only status %d\n", __func__, status);

	return status;
}

static void msmsdcc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;

	/*
	 * We may come here with clocks turned off in that case don't
	 * attempt to write into MASK0 register. While turning on the
	 * clocks mci_irqenable will be written to MASK0 register.
	 */

	spin_lock_irqsave(&host->lock, flags);
	if (enable) {
		host->mci_irqenable |= MCI_SDIOINTOPERMASK;
		if (atomic_read(&host->clks_on)) {
			writel_relaxed(readl_relaxed(host->base + MMCIMASK0) |
				MCI_SDIOINTOPERMASK, host->base + MMCIMASK0);
			mb();
		}
	} else {
		host->mci_irqenable &= ~MCI_SDIOINTOPERMASK;
		if (atomic_read(&host->clks_on)) {
			writel_relaxed(readl_relaxed(host->base + MMCIMASK0) &
				~MCI_SDIOINTOPERMASK, host->base + MMCIMASK0);
			mb();
		}
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static void msmsdcc_print_pin_info(struct msmsdcc_host *host)
{
	struct msm_mmc_pad_data *curr;
	char *pin_name[] = {"CLK", "CMD", "DATA"};
	char *pull_setting[] = {"NO_PULL", "PULL_DOWN", "KEEPER", "PULL_UP"};
	int i = 0;

	if(!host->plat->pin_data ||!host->plat->pin_data->pad_data || host->plat->pin_data->is_gpio)
		return;

	curr = host->plat->pin_data->pad_data;

	printk("%s, dump gpio: ", mmc_hostname(host->mmc));
	for (i = 0; i < curr->drv->size; i++) {
		printk("%s : %d mA ", pin_name[i],
			(msm_tlmm_get_hdrive(curr->drv->on[i].no)+1)*2);
	}
	for (i = 0; i < curr->pull->size; i++) {
		printk("%s : %s ", pin_name[i],
			pull_setting[msm_tlmm_get_pull(curr->drv->on[i].no)]);
	}
	printk("\n");
}

#ifdef CONFIG_PM_RUNTIME
static void msmsdcc_print_rpm_info(struct msmsdcc_host *host)
{
	struct device *dev = mmc_dev(host->mmc);

	pr_err("%s: PM: sdcc_suspended=%d, pending_resume=%d, sdcc_suspending=%d\n",
		mmc_hostname(host->mmc), host->sdcc_suspended,
		host->pending_resume, host->sdcc_suspending);
	pr_err("%s: RPM: runtime_status=%d, usage_count=%d,"
		" is_suspended=%d, disable_depth=%d, runtime_error=%d,"
		" request_pending=%d, request=%d\n",
		mmc_hostname(host->mmc), dev->power.runtime_status,
		atomic_read(&dev->power.usage_count),
		dev->power.is_suspended, dev->power.disable_depth,
		dev->power.runtime_error, dev->power.request_pending,
		dev->power.request);
	if (is_sd_platform(host->plat)) {
		pr_info("%s: SD RPM DUMP: enable time %lld, enable_count %d, disable time %lld, disable count %d, rpm suspend time %lld, rpm resume time %lld\n",
		mmc_hostname(host->mmc), ktime_to_us(host->enable_t), atomic_read(&host->enable_count_usage), ktime_to_us(host->disable_t), atomic_read(&host->disable_count_usage), ktime_to_us(host->rpm_sus_t), ktime_to_us(host->rpm_res_t));
	}
}

static int msmsdcc_enable(struct mmc_host *mmc)
{
	int rc = 0;
	struct device *dev = mmc->parent;
	struct msmsdcc_host *host = mmc_priv(mmc);


	msmsdcc_pm_qos_update_latency(host, 1);

	if (mmc->card && mmc_card_sdio(mmc->card))
		goto out;

	if (is_mmc_platform(host->plat) || is_sd_platform(host->plat))
		goto get_sync;

	if (host->sdcc_suspended && host->pending_resume &&
			!pm_runtime_suspended(dev)) {
		host->pending_resume = false;
		pm_runtime_get_noresume(dev);
		rc = msmsdcc_runtime_resume(dev);
		goto skip_get_sync;
	}

	if (dev->power.runtime_status == RPM_SUSPENDING) {
		if (mmc->suspend_task == current) {
			pm_runtime_get_noresume(dev);
			goto out;
		}
	} else if (dev->power.runtime_status == RPM_RESUMING) {
		pm_runtime_get_noresume(dev);
		goto out;
	}

get_sync:
	if (is_sd_platform(host->plat))
		host->enable_t = ktime_get();
	rc = pm_runtime_get_sync(dev);
	if (is_sd_platform(host->plat))
		atomic_set(&host->enable_count_usage, atomic_read(&dev->power.usage_count));

skip_get_sync:
	if (rc < 0) {
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, rc);
		msmsdcc_print_rpm_info(host);
		return rc;
	}
out:
	msmsdcc_msm_bus_cancel_work_and_set_vote(host, &mmc->ios);
	return 0;
}

static int msmsdcc_disable(struct mmc_host *mmc)
{
	int rc;
	struct msmsdcc_host *host = mmc_priv(mmc);
	struct device *dev = mmc->parent;


	msmsdcc_pm_qos_update_latency(host, 0);

	if (mmc->card && mmc_card_sdio(mmc->card)) {
		rc = 0;
		goto out;
	}

	if (host->plat->disable_runtime_pm)
		return -ENOTSUPP;

	if (is_sd_platform(host->plat))
		host->disable_t = ktime_get();

	rc = pm_runtime_put_sync(mmc->parent);

	if (is_sd_platform(host->plat))
		atomic_set(&host->disable_count_usage, atomic_read(&dev->power.usage_count));

	if (rc < 0) {
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, rc);
		msmsdcc_print_rpm_info(host);
	}

out:
	msmsdcc_msm_bus_queue_work(host);
	return 0;
}
#else
static void msmsdcc_print_rpm_info(struct msmsdcc_host *host) {}

static int msmsdcc_enable(struct mmc_host *mmc)
{
	struct device *dev = mmc->parent;
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int rc = 0;

	msmsdcc_pm_qos_update_latency(host, 1);

	if (mmc->card && mmc_card_sdio(mmc->card)) {
		rc = 0;
		goto out;
	}

	if (host->sdcc_suspended && host->pending_resume) {
		host->pending_resume = false;
		rc = msmsdcc_runtime_resume(dev);
		goto out;
	}

	mutex_lock(&host->clk_mutex);
	rc = msmsdcc_setup_clocks(host, true);

#ifdef CONFIG_WIFI_MMC
	if (is_wifi_platform(host->plat)) {
		if (host->clks_on) {
			printk(KERN_INFO "[WIFI] [MMC] %s [WIFI] %s clks is ON\n", __func__, mmc_hostname(host->mmc));
		}
	}
#endif
	mutex_unlock(&host->clk_mutex);

out:
	if (rc < 0) {
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, rc);
		msmsdcc_pm_qos_update_latency(host, 0);
		return rc;
	}
	msmsdcc_msm_bus_cancel_work_and_set_vote(host, &mmc->ios);
	return 0;
}

static int msmsdcc_disable(struct mmc_host *mmc)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int rc = 0;

	msmsdcc_pm_qos_update_latency(host, 0);

	if (mmc->card && mmc_card_sdio(mmc->card))
		goto out;

	mutex_lock(&host->clk_mutex);
	rc = msmsdcc_setup_clocks(host, false);

#ifdef CONFIG_WIFI_MMC
	if (is_wifi_platform(host->plat)) {
		if (!host->clks_on) {
			printk(KERN_INFO "[WIFI] [MMC] %s [WIFI] %s clks is OFF\n", __func__, mmc_hostname(host->mmc));
		}
	}
#endif
	mutex_unlock(&host->clk_mutex);

	if (rc) {
		msmsdcc_pm_qos_update_latency(host, 1);
		return rc;
	}
out:
	msmsdcc_msm_bus_queue_work(host);
	return rc;
}
#endif

static int msmsdcc_switch_io_voltage(struct mmc_host *mmc,
				     struct mmc_ios *ios)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int rc = 0;

	switch (ios->signal_voltage) {
	case MMC_SIGNAL_VOLTAGE_330:
		
		rc = msmsdcc_set_vdd_io_vol(host, VDD_IO_HIGH, 0);
		if (!rc)
			msmsdcc_update_io_pad_pwr_switch(host);
		goto out;
	case MMC_SIGNAL_VOLTAGE_180:
		break;
	case MMC_SIGNAL_VOLTAGE_120:
		rc = msmsdcc_set_vdd_io_vol(host, VDD_IO_SET_LEVEL, 1200000);
		if (!rc)
			msmsdcc_update_io_pad_pwr_switch(host);
		goto out;
	default:
		
		rc = -EINVAL;
		goto out;
	}

	spin_lock_irqsave(&host->lock, flags);

	if (readl_relaxed(host->base + MCI_TEST_INPUT) & (0xF << 1)) {
		rc = -EAGAIN;
		pr_err("%s: %s: MCIDATIN_3_0 is still not all zeros",
			mmc_hostname(mmc), __func__);
		goto out_unlock;
	}

	
	writel_relaxed((readl_relaxed(host->base + MMCICLOCK) |
			MCI_CLK_PWRSAVE), host->base + MMCICLOCK);
	msmsdcc_sync_reg_wr(host);
	spin_unlock_irqrestore(&host->lock, flags);

	rc = msmsdcc_set_vdd_io_vol(host, VDD_IO_LOW, 0);
	if (rc)
		goto out;

	msmsdcc_update_io_pad_pwr_switch(host);

	
	usleep_range(5000, 5500);

	spin_lock_irqsave(&host->lock, flags);
	
	writel_relaxed((readl_relaxed(host->base + MMCICLOCK)
			& ~MCI_CLK_PWRSAVE), host->base + MMCICLOCK);
	msmsdcc_sync_reg_wr(host);
	spin_unlock_irqrestore(&host->lock, flags);

	usleep_range(1000, 1500);

	spin_lock_irqsave(&host->lock, flags);
	if ((readl_relaxed(host->base + MCI_TEST_INPUT) & (0xF << 1))
				!= (0xF << 1)) {
		pr_err("%s: %s: MCIDATIN_3_0 are still not all ones",
			mmc_hostname(mmc), __func__);
		rc = -EAGAIN;
		goto out_unlock;
	}

out_unlock:
	
	writel_relaxed((readl_relaxed(host->base + MMCICLOCK) |
			MCI_CLK_PWRSAVE), host->base + MMCICLOCK);
	msmsdcc_sync_reg_wr(host);
	spin_unlock_irqrestore(&host->lock, flags);
out:
	return rc;
}

static inline void msmsdcc_cm_sdc4_dll_set_freq(struct msmsdcc_host *host)
{
	u32 mclk_freq = 0;

	
	if (host->clk_rate <= 112000000)
		mclk_freq = 0;
	else if (host->clk_rate <= 125000000)
		mclk_freq = 1;
	else if (host->clk_rate <= 137000000)
		mclk_freq = 2;
	else if (host->clk_rate <= 150000000)
		mclk_freq = 3;
	else if (host->clk_rate <= 162000000)
		mclk_freq = 4;
	else if (host->clk_rate <= 175000000)
		mclk_freq = 5;
	else if (host->clk_rate <= 187000000)
		mclk_freq = 6;
	else if (host->clk_rate <= 200000000)
		mclk_freq = 7;

	writel_relaxed(((readl_relaxed(host->base + MCI_DLL_CONFIG)
			& ~(7 << 24)) | (mclk_freq << 24)),
			host->base + MCI_DLL_CONFIG);
}

static int msmsdcc_init_cm_sdc4_dll(struct msmsdcc_host *host)
{
	int rc = 0;
	unsigned long flags;
	u32 wait_cnt;

	spin_lock_irqsave(&host->lock, flags);
	writel_relaxed((readl_relaxed(host->base + MMCICLOCK)
			& ~MCI_CLK_PWRSAVE), host->base + MMCICLOCK);
	msmsdcc_sync_reg_wr(host);

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			| MCI_DLL_RST), host->base + MCI_DLL_CONFIG);

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			| MCI_DLL_PDN), host->base + MCI_DLL_CONFIG);

	msmsdcc_cm_sdc4_dll_set_freq(host);

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			& ~MCI_DLL_RST), host->base + MCI_DLL_CONFIG);

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			& ~MCI_DLL_PDN), host->base + MCI_DLL_CONFIG);

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			| MCI_DLL_EN), host->base + MCI_DLL_CONFIG);

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			| MCI_CK_OUT_EN), host->base + MCI_DLL_CONFIG);

	wait_cnt = 50;
	
	while (!(readl_relaxed(host->base + MCI_DLL_STATUS) & MCI_DLL_LOCK)) {
		
		if (--wait_cnt == 0) {
			pr_err("%s: %s: DLL failed to LOCK\n",
				mmc_hostname(host->mmc), __func__);
			rc = -ETIMEDOUT;
			goto out;
		}
		
		udelay(1);
	}

out:
	
	writel_relaxed((readl_relaxed(host->base + MMCICLOCK) |
			MCI_CLK_PWRSAVE), host->base + MMCICLOCK);
	msmsdcc_sync_reg_wr(host);
	spin_unlock_irqrestore(&host->lock, flags);

	return rc;
}

static inline int msmsdcc_dll_poll_ck_out_en(struct msmsdcc_host *host,
						u8 poll)
{
	int rc = 0;
	u32 wait_cnt = 50;
	u8 ck_out_en = 0;

	
	ck_out_en = !!(readl_relaxed(host->base + MCI_DLL_CONFIG) &
			MCI_CK_OUT_EN);

	while (ck_out_en != poll) {
		if (--wait_cnt == 0) {
			pr_err("%s: %s: CK_OUT_EN bit is not %d\n",
				mmc_hostname(host->mmc), __func__, poll);
			rc = -ETIMEDOUT;
			goto out;
		}
		udelay(1);

		ck_out_en = !!(readl_relaxed(host->base + MCI_DLL_CONFIG) &
			MCI_CK_OUT_EN);
	}
out:
	return rc;
}

static int msmsdcc_enable_cdr_cm_sdc4_dll(struct msmsdcc_host *host)
{
	int rc = 0;
	u32 config;

	config = readl_relaxed(host->base + MCI_DLL_CONFIG);
	config |= MCI_CDR_EN;
	config &= ~(MCI_CDR_EXT_EN | MCI_CK_OUT_EN);
	writel_relaxed(config, host->base + MCI_DLL_CONFIG);

	
	rc = msmsdcc_dll_poll_ck_out_en(host, 0);
	if (rc)
		goto err_out;

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			| MCI_CK_OUT_EN), host->base + MCI_DLL_CONFIG);

	
	rc = msmsdcc_dll_poll_ck_out_en(host, 1);
	if (rc)
		goto err_out;

	goto out;

err_out:
	pr_err("%s: %s: Failed\n", mmc_hostname(host->mmc), __func__);
out:
	return rc;
}

static int msmsdcc_config_cm_sdc4_dll_phase(struct msmsdcc_host *host,
						u8 phase)
{
	int rc = 0;
	u8 grey_coded_phase_table[] = {0x0, 0x1, 0x3, 0x2, 0x6, 0x7, 0x5, 0x4,
					0xC, 0xD, 0xF, 0xE, 0xA, 0xB, 0x9,
					0x8};
	unsigned long flags;
	u32 config;

	spin_lock_irqsave(&host->lock, flags);

	config = readl_relaxed(host->base + MCI_DLL_CONFIG);
	config &= ~(MCI_CDR_EN | MCI_CK_OUT_EN);
	config |= (MCI_CDR_EXT_EN | MCI_DLL_EN);
	writel_relaxed(config, host->base + MCI_DLL_CONFIG);

	
	rc = msmsdcc_dll_poll_ck_out_en(host, 0);
	if (rc)
		goto err_out;

	writel_relaxed(((readl_relaxed(host->base + MCI_DLL_CONFIG)
			& ~(0xF << 20))
			| (grey_coded_phase_table[phase] << 20)),
			host->base + MCI_DLL_CONFIG);

	
	writel_relaxed((readl_relaxed(host->base + MCI_DLL_CONFIG)
			| MCI_CK_OUT_EN), host->base + MCI_DLL_CONFIG);

	
	rc = msmsdcc_dll_poll_ck_out_en(host, 1);
	if (rc)
		goto err_out;

	config = readl_relaxed(host->base + MCI_DLL_CONFIG);
	config |= MCI_CDR_EN;
	config &= ~MCI_CDR_EXT_EN;
	writel_relaxed(config, host->base + MCI_DLL_CONFIG);
	goto out;

err_out:
	pr_err("%s: %s: Failed to set DLL phase: %d\n",
		mmc_hostname(host->mmc), __func__, phase);
out:
	spin_unlock_irqrestore(&host->lock, flags);
	return rc;
}

static int find_most_appropriate_phase(struct msmsdcc_host *host,
				u8 *phase_table, u8 total_phases)
{
	#define MAX_PHASES 16
	int ret;
	u8 ranges[MAX_PHASES][MAX_PHASES] = { {0}, {0} };
	u8 phases_per_row[MAX_PHASES] = {0};
	int row_index = 0, col_index = 0, selected_row_index = 0, curr_max = 0;
	int i, cnt, phase_0_raw_index = 0, phase_15_raw_index = 0;
	bool phase_0_found = false, phase_15_found = false;

	if (!total_phases || (total_phases > MAX_PHASES)) {
		pr_err("%s: %s: invalid argument: total_phases=%d\n",
			mmc_hostname(host->mmc), __func__, total_phases);
		return -EINVAL;
	}

	for (cnt = 0; cnt < total_phases; cnt++) {
		ranges[row_index][col_index] = phase_table[cnt];
		phases_per_row[row_index] += 1;
		col_index++;

		if ((cnt + 1) == total_phases) {
			continue;
		
		} else if ((phase_table[cnt] + 1) != phase_table[cnt + 1]) {
			row_index++;
			col_index = 0;
		}
	}

	if (row_index >= MAX_PHASES)
		return -EINVAL;

	
	if (!ranges[0][0]) {
		phase_0_found = true;
		phase_0_raw_index = 0;
		
		for (cnt = 1; cnt <= row_index; cnt++) {
			if (phases_per_row[cnt]) {
				for (i = 0; i < phases_per_row[cnt]; i++) {
					if (ranges[cnt][i] == 15) {
						phase_15_found = true;
						phase_15_raw_index = cnt;
						break;
					}
				}
			}
		}
	}

	
	if (phase_0_found && phase_15_found) {
		
		u8 phases_0 = phases_per_row[phase_0_raw_index];
		
		u8 phases_15 = phases_per_row[phase_15_raw_index];

		if (phases_0 + phases_15 >= MAX_PHASES)
			return -EINVAL;

		
		i = phases_15;
		for (cnt = 0; cnt < phases_0; cnt++) {
			ranges[phase_15_raw_index][i] =
				ranges[phase_0_raw_index][cnt];
			if (++i >= MAX_PHASES)
				break;
		}

		phases_per_row[phase_0_raw_index] = 0;
		phases_per_row[phase_15_raw_index] = phases_15 + phases_0;
	}

	for (cnt = 0; cnt <= row_index; cnt++) {
		if (phases_per_row[cnt] > curr_max) {
			curr_max = phases_per_row[cnt];
			selected_row_index = cnt;
		}
	}

	i = ((curr_max * 3) / 4);
	if (i)
		i--;

	ret = (int)ranges[selected_row_index][i];

	if (ret >= MAX_PHASES) {
		ret = -EINVAL;
		pr_err("%s: %s: invalid phase selected=%d\n",
			mmc_hostname(host->mmc), __func__, ret);
	}

	return ret;
}

static int msmsdcc_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
	int rc = 0;
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long	flags;
	u8 phase = 0, *data_buf, tuned_phases[16], tuned_phase_cnt = 0;
	const u32 *tuning_block_pattern = tuning_block_64;
	int size = sizeof(tuning_block_64); 

	pr_debug("%s: Enter %s\n", mmc_hostname(mmc), __func__);

	
	if (!host->tuning_needed) {
		rc = 0;
		goto exit;
	}
	
	
	host->cont_tuning_cnt++;
	if (host->cont_tuning_cnt > MAX_CONTINUOUS_TUNING_COUNT) {
		if (host->cont_tuning_cnt == (MAX_CONTINUOUS_TUNING_COUNT+1))
		pr_err("%s: %s: too many continuous tuning, abort tuning!!count=%d\n", mmc_hostname(mmc), __func__, MAX_CONTINUOUS_TUNING_COUNT);
		if (is_wifi_platform(host->plat)) {
			mmc_tuning_fail = 1;
			smp_mb();
		}
		rc = 0;
		goto exit;
	}
	


	spin_lock_irqsave(&host->lock, flags);
	WARN(!host->pwr, "SDCC power is turned off\n");
	WARN(!atomic_read(&host->clks_on), "SDCC clocks are turned off\n");
	WARN(host->sdcc_irq_disabled, "SDCC IRQ is disabled\n");

	host->tuning_in_progress = 1;
	if ((opcode == MMC_SEND_TUNING_BLOCK_HS200) &&
		(mmc->ios.bus_width == MMC_BUS_WIDTH_8)) {
		tuning_block_pattern = tuning_block_128;
		size = sizeof(tuning_block_128);
	}
	spin_unlock_irqrestore(&host->lock, flags);

	
	rc = msmsdcc_init_cm_sdc4_dll(host);
	if (rc)
		goto out;

	data_buf = kmalloc(size, GFP_KERNEL);
	if (!data_buf) {
		rc = -ENOMEM;
		goto out;
	}

	phase = 0;
	do {
		struct mmc_command cmd = {0};
		struct mmc_data data = {0};
		struct mmc_request mrq = {
			.cmd = &cmd,
			.data = &data
		};
		struct scatterlist sg;

		
		rc = msmsdcc_config_cm_sdc4_dll_phase(host, phase);
		if (rc)
			goto kfree;

		cmd.opcode = opcode;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		data.blksz = size;
		data.blocks = 1;
		data.flags = MMC_DATA_READ;
		data.timeout_ns = 1000 * 1000 * 1000; 

		data.sg = &sg;
		data.sg_len = 1;
		sg_init_one(&sg, data_buf, size);
		memset(data_buf, 0, size);
		mmc_wait_for_req(mmc, &mrq);

		if (!cmd.error && !data.error &&
			!memcmp(data_buf, tuning_block_pattern, size)) {
			
			tuned_phases[tuned_phase_cnt++] = phase;
			pr_debug("%s: %s: found good phase = %d\n",
				mmc_hostname(mmc), __func__, phase);
		}
	} while (++phase < 16);

	if (tuned_phase_cnt) {
		rc = find_most_appropriate_phase(host, tuned_phases,
							tuned_phase_cnt);
		if (rc < 0)
			goto kfree;
		else
			phase = (u8)rc;

		rc = msmsdcc_config_cm_sdc4_dll_phase(host, phase);
		if (rc)
			goto kfree;
		pr_debug("%s: %s: finally setting the tuning phase to %d\n",
				mmc_hostname(mmc), __func__, phase);
	} else {
		
		pr_err("%s: %s: no tuning point found\n",
			mmc_hostname(mmc), __func__);
		msmsdcc_dump_sdcc_state(host);
		rc = -EAGAIN;
	}

kfree:
	kfree(data_buf);
out:
	spin_lock_irqsave(&host->lock, flags);
	host->tuning_in_progress = 0;
	if (!rc) {
		
		host->cont_tuning_cnt = 0;
		if (is_wifi_platform(host->plat)) 
			mmc_tuning_fail= 0;
		
		host->tuning_done = true;
		pr_err("%s: %s: tuning done with phase %d\n",
			mmc_hostname(mmc), __func__, phase);
	}
	spin_unlock_irqrestore(&host->lock, flags);
exit:
	pr_debug("%s: Exit %s\n", mmc_hostname(mmc), __func__);
	return rc;
}

static const struct mmc_host_ops msmsdcc_ops = {
	.enable		= msmsdcc_enable,
	.disable	= msmsdcc_disable,
	.pre_req        = msmsdcc_pre_req,
	.post_req       = msmsdcc_post_req,
	.request	= msmsdcc_request,
	.set_ios	= msmsdcc_set_ios,
	.get_ro		= msmsdcc_get_ro,
	.enable_sdio_irq = msmsdcc_enable_sdio_irq,
	.start_signal_voltage_switch = msmsdcc_switch_io_voltage,
	.execute_tuning = msmsdcc_execute_tuning,
};

static unsigned int
msmsdcc_slot_status(struct msmsdcc_host *host)
{
	int status;
	unsigned int gpio_no = host->plat->status_gpio;

	status = gpio_request(gpio_no, "SD_HW_Detect");
	if (status) {
		pr_err("%s: %s: Failed to request GPIO %d\n",
			mmc_hostname(host->mmc), __func__, gpio_no);
	} else {
		status = gpio_direction_input(gpio_no);
		if (!status) {
			status = gpio_get_value_cansleep(gpio_no);
			if (host->plat->is_status_gpio_active_low)
				status = !status;
		}
		gpio_free(gpio_no);
	}
	return status;
}

static int msmsdcc_sdc_get_status(struct mmc_host *mmc)
{
       struct msmsdcc_host *host = mmc_priv(mmc);
       unsigned int status = 0;

       if (host->plat->status || host->plat->status_gpio) {
               if (host->plat->status)
                       status = host->plat->status(mmc_dev(host->mmc));
               else
                       status = msmsdcc_slot_status(host);
       } else {
               pr_debug("%s: fail to get card status %s\n", mmc_hostname(mmc), __func__);
       }
       return status;
}

static const struct mmc_host_ops msmsdcc_ops_sd = {
	.enable		= msmsdcc_enable,
	.disable	= msmsdcc_disable,
	.pre_req        = msmsdcc_pre_req,
	.post_req       = msmsdcc_post_req,
	.request	= msmsdcc_request,
	.set_ios	= msmsdcc_set_ios,
	.get_ro		= msmsdcc_get_ro,
	.enable_sdio_irq = msmsdcc_enable_sdio_irq,
	.start_signal_voltage_switch = msmsdcc_switch_io_voltage,
	.execute_tuning = msmsdcc_execute_tuning,
	.get_cd = msmsdcc_sdc_get_status,
};

static void
msmsdcc_check_status(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	unsigned int status;

	if (host->plat->status || host->plat->status_gpio) {
		if (host->plat->status)
			status = host->plat->status(mmc_dev(host->mmc));
		else
			status = msmsdcc_slot_status(host);

		host->eject = !status;

		if (status ^ host->oldstat) {
#if SD_DEBOUNCE_DEBUG
			if (is_sd_platform(host->plat)) {
				irq_diff = ktime_sub(ktime_get(), last_irq);
				last_irq = ktime_get();
			}
#endif

			if (host->plat->status)
				pr_info("%s: Slot status change detected "
					"(%d -> %d)\n",
					mmc_hostname(host->mmc),
					host->oldstat, status);
			else if (host->plat->is_status_gpio_active_low)
				pr_info("%s: Slot status change detected "
					"(%d -> %d) and the card detect GPIO"
					" is ACTIVE_LOW\n",
					mmc_hostname(host->mmc),
					host->oldstat, status);
			else
				pr_info("%s: Slot status change detected "
					"(%d -> %d) and the card detect GPIO"
					" is ACTIVE_HIGH\n",
					mmc_hostname(host->mmc),
					host->oldstat, status);

			if (is_sd_platform(host->plat))
				mmc_detect_change(host->mmc, msecs_to_jiffies(SD_detect_debounce_time));
			else
				mmc_detect_change(host->mmc, 0);
		}
		host->oldstat = status;
	} else {
		mmc_detect_change(host->mmc, 0);
	}
}

static irqreturn_t
msmsdcc_platform_status_irq(int irq, void *dev_id)
{
	struct msmsdcc_host *host = dev_id;

	pr_debug("%s: %d\n", __func__, irq);
	msmsdcc_check_status((unsigned long) host);
	return IRQ_HANDLED;
}

static irqreturn_t
msmsdcc_platform_sdiowakeup_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;

	pr_debug("%s: SDIO Wake up IRQ : %d\n", mmc_hostname(host->mmc), irq);
	spin_lock(&host->lock);
	if (!host->sdio_wakeupirq_disabled) {
		disable_irq_nosync(irq);
		if (host->sdcc_suspended) {
			wake_lock(&host->sdio_wlock);
			msmsdcc_disable_irq_wake(host);
		}
		host->sdio_wakeupirq_disabled = 1;
	}
	if (host->plat->is_sdio_al_client) {
		wake_lock(&host->sdio_wlock);
		spin_unlock(&host->lock);
		mmc_signal_sdio_irq(host->mmc);
		goto out_unlocked;
	}
	spin_unlock(&host->lock);

out_unlocked:
	return IRQ_HANDLED;
}

static void
msmsdcc_status_notify_cb(int card_present, void *dev_id)
{
	struct msmsdcc_host *host = dev_id;

	pr_debug("%s: card_present %d\n", mmc_hostname(host->mmc),
	       card_present);
	msmsdcc_check_status((unsigned long) host);
}

static int
msmsdcc_init_dma(struct msmsdcc_host *host)
{
	memset(&host->dma, 0, sizeof(struct msmsdcc_dma_data));
	host->dma.host = host;
	host->dma.channel = -1;
	host->dma.crci = -1;

	if (!host->dmares)
		return -ENODEV;

	host->dma.nc = dma_alloc_coherent(NULL,
					  sizeof(struct msmsdcc_nc_dmadata),
					  &host->dma.nc_busaddr,
					  GFP_KERNEL);
	if (host->dma.nc == NULL) {
		pr_err("Unable to allocate DMA buffer\n");
		return -ENOMEM;
	}
	memset(host->dma.nc, 0x00, sizeof(struct msmsdcc_nc_dmadata));
	host->dma.cmd_busaddr = host->dma.nc_busaddr;
	host->dma.cmdptr_busaddr = host->dma.nc_busaddr +
				offsetof(struct msmsdcc_nc_dmadata, cmdptr);
	host->dma.channel = host->dmares->start;
	host->dma.crci = host->dma_crci_res->start;

	return 0;
}

#ifdef CONFIG_MMC_MSM_SPS_SUPPORT
static int msmsdcc_sps_init_ep_conn(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep,
				bool is_producer)
{
	int rc = 0;
	struct sps_pipe *sps_pipe_handle;
	struct sps_connect *sps_config = &ep->config;
	struct sps_register_event *sps_event = &ep->event;

	
	sps_pipe_handle = sps_alloc_endpoint();
	if (!sps_pipe_handle) {
		pr_err("%s: sps_alloc_endpoint() failed!!! is_producer=%d",
			   mmc_hostname(host->mmc), is_producer);
		rc = -ENOMEM;
		goto out;
	}

	
	rc = sps_get_config(sps_pipe_handle, sps_config);
	if (rc) {
		pr_err("%s: sps_get_config() failed!!! pipe_handle=0x%x,"
			" rc=%d", mmc_hostname(host->mmc),
			(u32)sps_pipe_handle, rc);
		goto get_config_err;
	}

	
	if (is_producer) {
		sps_config->source = host->sps.bam_handle;
		sps_config->destination = SPS_DEV_HANDLE_MEM;
		
		sps_config->mode = SPS_MODE_SRC;
		sps_config->options =
			SPS_O_AUTO_ENABLE | SPS_O_EOT | SPS_O_ACK_TRANSFERS;
	} else {
		sps_config->source = SPS_DEV_HANDLE_MEM;
		sps_config->destination = host->sps.bam_handle;
		sps_config->mode = SPS_MODE_DEST;
		sps_config->options =
			SPS_O_AUTO_ENABLE | SPS_O_EOT | SPS_O_ACK_TRANSFERS;
	}

	
	sps_config->src_pipe_index = host->sps.src_pipe_index;
	
	sps_config->dest_pipe_index = host->sps.dest_pipe_index;
	sps_config->event_thresh = 0x10;

	
	sps_config->desc.size = SPS_MAX_DESC_FIFO_SIZE -
		(SPS_MAX_DESC_FIFO_SIZE % SPS_MAX_DESC_LENGTH);
	sps_config->desc.base = dma_alloc_coherent(mmc_dev(host->mmc),
						sps_config->desc.size,
						&sps_config->desc.phys_base,
						GFP_KERNEL);

	if (!sps_config->desc.base) {
		rc = -ENOMEM;
		pr_err("%s: dma_alloc_coherent() failed!!! Can't allocate buffer\n"
			, mmc_hostname(host->mmc));
		goto get_config_err;
	}
	memset(sps_config->desc.base, 0x00, sps_config->desc.size);

	
	rc = sps_connect(sps_pipe_handle, sps_config);
	if (rc) {
		pr_err("%s: sps_connect() failed!!! pipe_handle=0x%x,"
			" rc=%d", mmc_hostname(host->mmc),
			(u32)sps_pipe_handle, rc);
		goto sps_connect_err;
	}

	sps_event->mode = SPS_TRIGGER_CALLBACK;
	sps_event->options = SPS_O_EOT;
	sps_event->callback = msmsdcc_sps_complete_cb;
	sps_event->xfer_done = NULL;
	sps_event->user = (void *)host;

	
	rc = sps_register_event(sps_pipe_handle, sps_event);
	if (rc) {
		pr_err("%s: sps_connect() failed!!! pipe_handle=0x%x,"
			" rc=%d", mmc_hostname(host->mmc),
			(u32)sps_pipe_handle, rc);
		goto reg_event_err;
	}
	
	ep->pipe_handle = sps_pipe_handle;
	pr_debug("%s: %s, success !!! %s: pipe_handle=0x%x,"
		" desc_fifo.phys_base=0x%x\n", mmc_hostname(host->mmc),
		__func__, is_producer ? "READ" : "WRITE",
		(u32)sps_pipe_handle, sps_config->desc.phys_base);
	goto out;

reg_event_err:
	sps_disconnect(sps_pipe_handle);
sps_connect_err:
	dma_free_coherent(mmc_dev(host->mmc),
			sps_config->desc.size,
			sps_config->desc.base,
			sps_config->desc.phys_base);
get_config_err:
	sps_free_endpoint(sps_pipe_handle);
out:
	return rc;
}

static void msmsdcc_sps_exit_ep_conn(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep)
{
	struct sps_pipe *sps_pipe_handle = ep->pipe_handle;
	struct sps_connect *sps_config = &ep->config;
	struct sps_register_event *sps_event = &ep->event;

	sps_event->xfer_done = NULL;
	sps_event->callback = NULL;
	sps_register_event(sps_pipe_handle, sps_event);
	sps_disconnect(sps_pipe_handle);
	dma_free_coherent(mmc_dev(host->mmc),
			sps_config->desc.size,
			sps_config->desc.base,
			sps_config->desc.phys_base);
	sps_free_endpoint(sps_pipe_handle);
}

static int msmsdcc_sps_reset_ep(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep)
{
	int rc = 0;
	struct sps_pipe *sps_pipe_handle = ep->pipe_handle;

	rc = sps_disconnect(sps_pipe_handle);
	if (rc) {
		pr_err("%s: %s: sps_disconnect() failed!!! pipe_handle=0x%x,"
			" rc=%d", mmc_hostname(host->mmc), __func__,
			(u32)sps_pipe_handle, rc);
		goto out;
	}
 out:
	return rc;
}

static int msmsdcc_sps_restore_ep(struct msmsdcc_host *host,
				struct msmsdcc_sps_ep_conn_data *ep)
{
	int rc = 0;
	struct sps_pipe *sps_pipe_handle = ep->pipe_handle;
	struct sps_connect *sps_config = &ep->config;
	struct sps_register_event *sps_event = &ep->event;

	
	rc = sps_connect(sps_pipe_handle, sps_config);
	if (rc) {
		pr_err("%s: %s: sps_connect() failed!!! pipe_handle=0x%x,"
			" rc=%d", mmc_hostname(host->mmc), __func__,
			(u32)sps_pipe_handle, rc);
		goto out;
	}

	
	rc = sps_register_event(sps_pipe_handle, sps_event);
	if (rc) {
		pr_err("%s: %s: sps_register_event() failed!!!"
			" pipe_handle=0x%x, rc=%d",
			mmc_hostname(host->mmc), __func__,
			(u32)sps_pipe_handle, rc);
		goto reg_event_err;
	}
	goto out;

reg_event_err:
	sps_disconnect(sps_pipe_handle);
out:
	return rc;
}

static void
msmsdcc_sps_bam_global_irq_cb(enum sps_callback_case sps_cb_case, void *user)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)user;
	struct mmc_request *mrq;
	unsigned long flags;
	int32_t error = 0;

	BUG_ON(!host);
	BUG_ON(!is_sps_mode(host));

	if (sps_cb_case == SPS_CALLBACK_BAM_ERROR_IRQ) {
		
		if (is_mmc_platform(host->plat))
			host->sps.reset_bam = true;
		else {
			host->sps.pipe_reset_pending = true;
			host->sps.reset_device = true;
		}
		pr_err("%s: BAM Global ERROR IRQ happened\n",
			mmc_hostname(host->mmc));
		error = EAGAIN;
	} else if (sps_cb_case == SPS_CALLBACK_BAM_HRESP_ERR_IRQ) {
		pr_err("%s: BAM HRESP_ERR_IRQ happened\n",
			mmc_hostname(host->mmc));
		error = EACCES;
	} else {
		pr_err("%s: BAM global IRQ callback received, type:%d\n",
			mmc_hostname(host->mmc), (u32) sps_cb_case);
		error = EIO;
	}

	spin_lock_irqsave(&host->lock, flags);

	mrq = host->curr.mrq;

	if (mrq && mrq->cmd) {
		msmsdcc_dump_sdcc_state(host);

		if (!mrq->cmd->error)
			mrq->cmd->error = -error;
		if (host->curr.data) {
			if (mrq->data && !mrq->data->error)
				mrq->data->error = -error;
			host->curr.data_xfered = 0;
			if (host->sps.sg && is_sps_mode(host)) {
				
				msmsdcc_sps_exit_curr_xfer(host);
			} else {
				
				pr_err("%s: something is seriously wrong. "\
					"Funtion: %s, line: %d\n",
					mmc_hostname(host->mmc),
					__func__, __LINE__);
			}
		} else {
			
			pr_err("%s: something is seriously wrong. Funtion: "\
				"%s, line: %d\n", mmc_hostname(host->mmc),
				__func__, __LINE__);
		}
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static int msmsdcc_sps_init(struct msmsdcc_host *host)
{
	int rc = 0;
	struct sps_bam_props bam = {0};

	host->bam_base = ioremap(host->bam_memres->start,
				resource_size(host->bam_memres));
	if (!host->bam_base) {
		pr_err("%s: BAM ioremap() failed!!! phys_addr=0x%x,"
			" size=0x%x", mmc_hostname(host->mmc),
			host->bam_memres->start,
			(host->bam_memres->end -
			host->bam_memres->start));
		rc = -ENOMEM;
		goto out;
	}

	bam.phys_addr = host->bam_memres->start;
	bam.virt_addr = host->bam_base;
	bam.event_threshold = 0x10;	
	bam.summing_threshold = SPS_MIN_XFER_SIZE;
	
	bam.irq = (u32)host->bam_irqres->start;
	bam.manage = SPS_BAM_MGR_LOCAL;
	if (is_mmc_platform(host->plat)) {
		bam.callback = msmsdcc_sps_bam_global_irq_cb;
		bam.user = (void *)host;
	}

	pr_info("%s: bam physical base=0x%x\n", mmc_hostname(host->mmc),
			(u32)bam.phys_addr);
	pr_info("%s: bam virtual base=0x%x\n", mmc_hostname(host->mmc),
			(u32)bam.virt_addr);

	
	rc = sps_register_bam_device(&bam, &host->sps.bam_handle);
	if (rc) {
		pr_err("%s: sps_register_bam_device() failed!!! err=%d",
			   mmc_hostname(host->mmc), rc);
		goto reg_bam_err;
	}
	pr_info("%s: BAM device registered. bam_handle=0x%x",
		mmc_hostname(host->mmc), host->sps.bam_handle);

	host->sps.src_pipe_index = SPS_SDCC_PRODUCER_PIPE_INDEX;
	host->sps.dest_pipe_index = SPS_SDCC_CONSUMER_PIPE_INDEX;

	rc = msmsdcc_sps_init_ep_conn(host, &host->sps.prod,
					SPS_PROD_PERIPHERAL);
	if (rc)
		goto sps_reset_err;
	rc = msmsdcc_sps_init_ep_conn(host, &host->sps.cons,
					SPS_CONS_PERIPHERAL);
	if (rc)
		goto cons_conn_err;

	pr_info("%s: Qualcomm MSM SDCC-BAM at 0x%016llx irq %d\n",
		mmc_hostname(host->mmc),
		(unsigned long long)host->bam_memres->start,
		(unsigned int)host->bam_irqres->start);
	goto out;

cons_conn_err:
	msmsdcc_sps_exit_ep_conn(host, &host->sps.prod);
sps_reset_err:
	sps_deregister_bam_device(host->sps.bam_handle);
reg_bam_err:
	iounmap(host->bam_base);
out:
	return rc;
}

static void msmsdcc_sps_exit(struct msmsdcc_host *host)
{
	msmsdcc_sps_exit_ep_conn(host, &host->sps.cons);
	msmsdcc_sps_exit_ep_conn(host, &host->sps.prod);
	sps_deregister_bam_device(host->sps.bam_handle);
	iounmap(host->bam_base);
}
#endif 

static ssize_t
show_polling(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int poll;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	poll = !!(mmc->caps & MMC_CAP_NEEDS_POLL);
	spin_unlock_irqrestore(&host->lock, flags);

	return snprintf(buf, PAGE_SIZE, "%d\n", poll);
}

static ssize_t
store_polling(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int value;
	unsigned long flags;

	sscanf(buf, "%d", &value);

	spin_lock_irqsave(&host->lock, flags);
	if (value) {
		mmc->caps |= MMC_CAP_NEEDS_POLL;
		mmc_detect_change(host->mmc, 0);
	} else {
		mmc->caps &= ~MMC_CAP_NEEDS_POLL;
	}
#ifdef CONFIG_HAS_EARLYSUSPEND
	host->polling_enabled = mmc->caps & MMC_CAP_NEEDS_POLL;
#endif
	spin_unlock_irqrestore(&host->lock, flags);
	return count;
}

static ssize_t
show_sdcc_to_mem_max_bus_bw(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);

	return snprintf(buf, PAGE_SIZE, "%u\n",
			host->msm_bus_vote.is_max_bw_needed);
}

static ssize_t
store_sdcc_to_mem_max_bus_bw(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	uint32_t value;
	unsigned long flags;

	if (!kstrtou32(buf, 0, &value)) {
		spin_lock_irqsave(&host->lock, flags);
		host->msm_bus_vote.is_max_bw_needed = !!value;
		spin_unlock_irqrestore(&host->lock, flags);
	}

	return count;
}

static inline void set_auto_cmd_setting(struct device *dev,
					 const char *buf,
					 bool is_cmd19)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned int long flags;
	int temp;

	if (!kstrtou32(buf, 0, &temp)) {
		spin_lock_irqsave(&host->lock, flags);
		if (is_cmd19)
			host->en_auto_cmd19 = !!temp;
		spin_unlock_irqrestore(&host->lock, flags);
	}
}

static ssize_t
show_enable_auto_cmd19(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);

	return snprintf(buf, PAGE_SIZE, "%d\n", host->en_auto_cmd19);
}

static ssize_t
store_enable_auto_cmd19(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	set_auto_cmd_setting(dev, buf, true);

	return count;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void msmsdcc_early_suspend(struct early_suspend *h)
{
	struct msmsdcc_host *host =
		container_of(h, struct msmsdcc_host, early_suspend);
	unsigned long flags;

#ifdef CONFIG_WIFI_MMC
	if (is_wifi_platform(host->plat))
		pr_info("[WIFI] [MMC] %s: %s enter\n", mmc_hostname(host->mmc), __func__);
#endif
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s enter\n", mmc_hostname(host->mmc), __func__);
#endif

	spin_lock_irqsave(&host->lock, flags);
	host->polling_enabled = host->mmc->caps & MMC_CAP_NEEDS_POLL;
	host->mmc->caps &= ~MMC_CAP_NEEDS_POLL;
	spin_unlock_irqrestore(&host->lock, flags);

#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s leave\n", mmc_hostname(host->mmc), __func__);
#endif

};

static void msmsdcc_late_resume(struct early_suspend *h)
{
	struct msmsdcc_host *host =
		container_of(h, struct msmsdcc_host, early_suspend);
	unsigned long flags;

#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			pr_info("[WIMAX] [MMC] %s: %s enter\n", mmc_hostname(host->mmc), __func__);
#endif

	if (host->polling_enabled) {
		spin_lock_irqsave(&host->lock, flags);
		host->mmc->caps |= MMC_CAP_NEEDS_POLL;
		mmc_detect_change(host->mmc, 0);
		spin_unlock_irqrestore(&host->lock, flags);
	}

#ifdef CONFIG_WIFI_MMC
	if (is_wifi_platform(host->plat))
		pr_info("[WIFI] [MMC] %s: %s leave\n", mmc_hostname(host->mmc), __func__);
#endif
#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			pr_info("[WIMAX] [MMC] %s: %s leave\n", mmc_hostname(host->mmc), __func__);
#endif


};
#endif

static void msmsdcc_print_regs(const char *name, void __iomem *base,
			       u32 phys_base, unsigned int no_of_regs)
{
	unsigned int i;

	if (!base)
		return;

	pr_err("===== %s: Register Dumps @phys_base=0x%x, @virt_base=0x%x"
		" =====\n", name, phys_base, (u32)base);
	for (i = 0; i < no_of_regs; i = i + 4) {
		pr_err("Reg=0x%.2x: 0x%.8x, 0x%.8x, 0x%.8x, 0x%.8x\n", i*4,
			(u32)readl_relaxed(base + i*4),
			(u32)readl_relaxed(base + ((i+1)*4)),
			(u32)readl_relaxed(base + ((i+2)*4)),
			(u32)readl_relaxed(base + ((i+3)*4)));
	}
}

static void msmsdcc_dump_sdcc_state(struct msmsdcc_host *host)
{
	
	pr_err("%s: SDCC PWR is %s\n", mmc_hostname(host->mmc),
		(host->pwr ? "ON" : "OFF"));
	pr_err("%s: SDCC clks are %s, MCLK rate=%d\n",
		mmc_hostname(host->mmc),
		(atomic_read(&host->clks_on) ? "ON" : "OFF"),
		(u32)clk_get_rate(host->clk));
	pr_err("%s: SDCC irq is %s\n", mmc_hostname(host->mmc),
		(host->sdcc_irq_disabled ? "disabled" : "enabled"));

	
	if (atomic_read(&host->clks_on)) {
		msmsdcc_print_regs("SDCC-CORE", host->base,
				   host->core_memres->start, 28);
		pr_err("%s: MCI_TEST_INPUT = 0x%.8x\n",
			mmc_hostname(host->mmc),
			readl_relaxed(host->base + MCI_TEST_INPUT));
	}

	if (host->curr.data) {
		if (!msmsdcc_is_dma_possible(host, host->curr.data))
			pr_err("%s: PIO mode\n", mmc_hostname(host->mmc));
		else if (is_dma_mode(host))
			pr_err("%s: ADM mode: busy=%d, chnl=%d, crci=%d\n",
				mmc_hostname(host->mmc), host->dma.busy,
				host->dma.channel, host->dma.crci);
		else if (is_sps_mode(host)) {
			if (host->sps.busy && atomic_read(&host->clks_on))
				msmsdcc_print_regs("SDCC-DML", host->dml_base,
						   host->dml_memres->start,
						   16);
			pr_err("%s: SPS mode: busy=%d\n",
				mmc_hostname(host->mmc), host->sps.busy);

#ifdef CONFIG_SPS
			
			sps_get_bam_debug_info(host->sps.bam_handle, 3);
			sps_get_bam_debug_info(host->sps.bam_handle, 4);
			sps_get_bam_debug_info(host->sps.bam_handle, 1);
#endif
		}

		pr_err("%s: xfer_size=%d, data_xfered=%d, xfer_remain=%d\n",
			mmc_hostname(host->mmc), host->curr.xfer_size,
			host->curr.data_xfered, host->curr.xfer_remain);
	}

	if (host->sps.reset_bam)
		pr_err("%s: SPS BAM reset failed: sps reset_bam=%d\n",
			mmc_hostname(host->mmc), host->sps.reset_bam);

	pr_err("%s: got_dataend=%d, prog_enable=%d,"
		" wait_for_auto_prog_done=%d, got_auto_prog_done=%d,"
		" req_tout_ms=%d\n", mmc_hostname(host->mmc),
		host->curr.got_dataend, host->prog_enable,
		host->curr.wait_for_auto_prog_done,
		host->curr.got_auto_prog_done, host->curr.req_tout_ms);
	msmsdcc_print_rpm_info(host);
}

static void msmsdcc_req_tout_timer_hdlr(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	struct mmc_request *mrq;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	if (host->dummy_52_sent) {
		printk("%s: %s: dummy CMD52 timeout\n",
				mmc_hostname(host->mmc), __func__);
		host->dummy_52_sent = 0;
	}

	mrq = host->curr.mrq;

	if (mrq && mrq->cmd) {
		printk("%s: CMD%d: Request timeout\n", mmc_hostname(host->mmc),
				mrq->cmd->opcode);
		msmsdcc_dump_sdcc_state(host);

		if (!mrq->cmd->error)
			mrq->cmd->error = -ETIMEDOUT;
		host->dummy_52_needed = 0;
		if (host->curr.data) {
			if (mrq->data && !mrq->data->error)
				mrq->data->error = -ETIMEDOUT;
			host->curr.data_xfered = 0;
			if (host->dma.sg && is_dma_mode(host)) {
				msm_dmov_flush(host->dma.channel, 0);
			} else if (host->sps.sg && is_sps_mode(host)) {
				
				msmsdcc_sps_exit_curr_xfer(host);
			} else {
				msmsdcc_reset_and_restore(host);
				msmsdcc_stop_data(host);
				if (mrq->data && mrq->data->stop)
					msmsdcc_start_command(host,
							mrq->data->stop, 0);
				else
					msmsdcc_request_end(host, mrq);
			}
		} else {
			host->prog_enable = 0;
			host->curr.wait_for_auto_prog_done = false;
			msmsdcc_reset_and_restore(host);
			msmsdcc_request_end(host, mrq);
		}
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

#define MAX_PROP_SIZE 32
static int msmsdcc_dt_parse_vreg_info(struct device *dev,
		struct msm_mmc_reg_data **vreg_data, const char *vreg_name)
{
	int len, ret = 0;
	const __be32 *prop;
	char prop_name[MAX_PROP_SIZE];
	struct msm_mmc_reg_data *vreg;
	struct device_node *np = dev->of_node;

	snprintf(prop_name, MAX_PROP_SIZE, "%s-supply", vreg_name);
	if (of_parse_phandle(np, prop_name, 0)) {
		vreg = devm_kzalloc(dev, sizeof(*vreg), GFP_KERNEL);
		if (!vreg) {
			dev_err(dev, "No memory for vreg: %s\n", vreg_name);
			ret = -ENOMEM;
			goto err;
		}

		vreg->name = vreg_name;

		snprintf(prop_name, MAX_PROP_SIZE,
				"qcom,sdcc-%s-always_on", vreg_name);
		if (of_get_property(np, prop_name, NULL))
			vreg->always_on = true;

		snprintf(prop_name, MAX_PROP_SIZE,
				"qcom,sdcc-%s-lpm_sup", vreg_name);
		if (of_get_property(np, prop_name, NULL))
			vreg->lpm_sup = true;

		snprintf(prop_name, MAX_PROP_SIZE,
				"qcom,sdcc-%s-voltage_level", vreg_name);
		prop = of_get_property(np, prop_name, &len);
		if (!prop || (len != (2 * sizeof(__be32)))) {
			dev_warn(dev, "%s %s property\n",
				prop ? "invalid format" : "no", prop_name);
		} else {
			vreg->low_vol_level = be32_to_cpup(&prop[0]);
			vreg->high_vol_level = be32_to_cpup(&prop[1]);
		}

		snprintf(prop_name, MAX_PROP_SIZE,
				"qcom,sdcc-%s-current_level", vreg_name);
		prop = of_get_property(np, prop_name, &len);
		if (!prop || (len != (2 * sizeof(__be32)))) {
			dev_warn(dev, "%s %s property\n",
				prop ? "invalid format" : "no", prop_name);
		} else {
			vreg->lpm_uA = be32_to_cpup(&prop[0]);
			vreg->hpm_uA = be32_to_cpup(&prop[1]);
		}

		*vreg_data = vreg;
		dev_dbg(dev, "%s: %s %s vol=[%d %d]uV, curr=[%d %d]uA\n",
			vreg->name, vreg->always_on ? "always_on," : "",
			vreg->lpm_sup ? "lpm_sup," : "", vreg->low_vol_level,
			vreg->high_vol_level, vreg->lpm_uA, vreg->hpm_uA);
	}

err:
	return ret;
}

static struct mmc_platform_data *msmsdcc_populate_pdata(struct device *dev)
{
	int i, ret;
	struct mmc_platform_data *pdata;
	struct device_node *np = dev->of_node;
	u32 bus_width = 0, current_limit = 0;
	u32 *clk_table, *sup_voltages;
	int clk_table_len, sup_volt_len, len;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "could not allocate memory for platform data\n");
		goto err;
	}

	of_property_read_u32(np, "qcom,sdcc-bus-width", &bus_width);
	if (bus_width == 8) {
		pdata->mmc_bus_width = MMC_CAP_8_BIT_DATA;
	} else if (bus_width == 4) {
		pdata->mmc_bus_width = MMC_CAP_4_BIT_DATA;
	} else {
		dev_notice(dev, "Invalid bus width, default to 1 bit mode\n");
		pdata->mmc_bus_width = 0;
	}

	if (of_get_property(np, "qcom,sdcc-sup-voltages", &sup_volt_len)) {
		size_t sz;
		sz = sup_volt_len / sizeof(*sup_voltages);
		if (sz > 0) {
			sup_voltages = devm_kzalloc(dev,
					sz * sizeof(*sup_voltages), GFP_KERNEL);
			if (!sup_voltages) {
				dev_err(dev, "No memory for supported voltage\n");
				goto err;
			}

			ret = of_property_read_u32_array(np,
				"qcom,sdcc-sup-voltages", sup_voltages, sz);
			if (ret < 0) {
				dev_err(dev, "error while reading voltage"
						"ranges %d\n", ret);
				goto err;
			}
		} else {
			dev_err(dev, "No supported voltages\n");
			goto err;
		}
		for (i = 0; i < sz; i += 2) {
			u32 mask;

			mask = mmc_vddrange_to_ocrmask(sup_voltages[i],
					sup_voltages[i + 1]);
			if (!mask)
				dev_err(dev, "Invalide voltage range %d\n", i);
			pdata->ocr_mask |= mask;
		}
		dev_dbg(dev, "OCR mask=0x%x\n", pdata->ocr_mask);
	} else {
		dev_err(dev, "Supported voltage range not specified\n");
	}

	if (of_get_property(np, "qcom,sdcc-clk-rates", &clk_table_len)) {
		size_t sz;
		sz = clk_table_len / sizeof(*clk_table);

		if (sz > 0) {
			clk_table = devm_kzalloc(dev, sz * sizeof(*clk_table),
					GFP_KERNEL);
			if (!clk_table) {
				dev_err(dev, "No memory for clock table\n");
				goto err;
			}

			ret = of_property_read_u32_array(np,
				"qcom,sdcc-clk-rates", clk_table, sz);
			if (ret < 0) {
				dev_err(dev, "error while reading clk"
						"table %d\n", ret);
				goto err;
			}
		} else {
			dev_err(dev, "clk_table not specified\n");
			goto err;
		}
		pdata->sup_clk_table = clk_table;
		pdata->sup_clk_cnt = sz;
	} else {
		dev_err(dev, "Supported clock rates not specified\n");
	}

	pdata->vreg_data = devm_kzalloc(dev,
			sizeof(struct msm_mmc_slot_reg_data), GFP_KERNEL);
	if (!pdata->vreg_data) {
		dev_err(dev, "could not allocate memory for vreg_data\n");
		goto err;
	}

	if (msmsdcc_dt_parse_vreg_info(dev,
			&pdata->vreg_data->vdd_data, "vdd"))
		goto err;

	if (msmsdcc_dt_parse_vreg_info(dev,
			&pdata->vreg_data->vdd_io_data, "vdd-io"))
		goto err;

	len = of_property_count_strings(np, "qcom,sdcc-bus-speed-mode");

	for (i = 0; i < len; i++) {
		const char *name = NULL;

		of_property_read_string_index(np,
			"qcom,sdcc-bus-speed-mode", i, &name);
		if (!name)
			continue;

		if (!strncmp(name, "SDR12", sizeof("SDR12")))
			pdata->uhs_caps |= MMC_CAP_UHS_SDR12;
		else if (!strncmp(name, "SDR25", sizeof("SDR25")))
			pdata->uhs_caps |= MMC_CAP_UHS_SDR25;
		else if (!strncmp(name, "SDR50", sizeof("SDR50")))
			pdata->uhs_caps |= MMC_CAP_UHS_SDR50;
		else if (!strncmp(name, "DDR50", sizeof("DDR50")))
			pdata->uhs_caps |= MMC_CAP_UHS_DDR50;
		else if (!strncmp(name, "SDR104", sizeof("SDR104")))
			pdata->uhs_caps |= MMC_CAP_UHS_SDR104;
		else if (!strncmp(name, "HS200_1p8v", sizeof("HS200_1p8v")))
			pdata->uhs_caps2 |= MMC_CAP2_HS200_1_8V_SDR;
		else if (!strncmp(name, "HS200_1p2v", sizeof("HS200_1p2v")))
			pdata->uhs_caps2 |= MMC_CAP2_HS200_1_2V_SDR;
		else if (!strncmp(name, "DDR_1p8v", sizeof("DDR_1p8v")))
			pdata->uhs_caps |= MMC_CAP_1_8V_DDR
						| MMC_CAP_UHS_DDR50;
		else if (!strncmp(name, "DDR_1p2v", sizeof("DDR_1p2v")))
			pdata->uhs_caps |= MMC_CAP_1_2V_DDR
						| MMC_CAP_UHS_DDR50;
	}

	of_property_read_u32(np, "qcom,sdcc-current-limit", &current_limit);
	if (current_limit == 800)
		pdata->uhs_caps |= MMC_CAP_MAX_CURRENT_800;
	else if (current_limit == 600)
		pdata->uhs_caps |= MMC_CAP_MAX_CURRENT_600;
	else if (current_limit == 400)
		pdata->uhs_caps |= MMC_CAP_MAX_CURRENT_400;
	else if (current_limit == 200)
		pdata->uhs_caps |= MMC_CAP_MAX_CURRENT_200;

	if (of_get_property(np, "qcom,sdcc-xpc", NULL))
		pdata->xpc_cap = true;
	if (of_get_property(np, "qcom,sdcc-nonremovable", NULL))
		pdata->nonremovable = true;
	if (of_get_property(np, "qcom,sdcc-disable_cmd23", NULL))
		pdata->disable_cmd23 = true;

	return pdata;
err:
	return NULL;
}

static int msmsdcc_proc_wperf_show(char *page, char **start, off_t off,
		int count, int *eof, void *data)
{
	struct mmc_host *host = (struct mmc_host*) data;
	if (!host || !host->card)
		return 0;
	return sprintf(page, "%d", host->card->wr_perf);
}

static int msmsdcc_proc_burst_show(char *page, char **start, off_t off,
		int count, int *eof, void *data)
{
	struct mmc_host *host = (struct mmc_host*) data;
	if (!host)
		return 0;
	return sprintf(page, "%d", host->burst_mode);
}

static int msmsdcc_proc_burst_set(struct file *file, const char __user *buffer,
		unsigned long count, void *data)
{
	struct mmc_host *host = (struct mmc_host*) data;
	char *envp[3] = {"SWITCH_NAME=camera_burst",0, 0};
	if (!host || !host->card || !host->card->mmcblk_dev)
		return count;
	host->burst_mode = (buffer[0] == '0') ? 0 : 1;
	pr_info("%s: %d\n", __func__, host->burst_mode);
	if (!host->burst_mode) {
		envp[1] = "SWITCH_STATE=0";
	} else {
		envp[1] = "SWITCH_STATE=1";
	}

	kobject_uevent_env(&host->card->mmcblk_dev->kobj, KOBJ_CHANGE, envp);
	return count;
}

static int msmsdcc_proc_speed_class(char *page, char **start, off_t off,
		int count, int *eof, void *data)
{
	struct mmc_host *host = (struct mmc_host*) data;
	if (!host || !host->card)
		return 0;
	return sprintf(page, "%d", host->card->speed_class);
}

static int msmsdcc_proc_bkops_show(char *page, char **start, off_t off,
		int count, int *eof, void *data)
{
	struct mmc_host *host = (struct mmc_host*) data;
	if (!host)
		return 0;
	return sprintf(page, "%d", host->bkops_trigger);
}

static int msmsdcc_proc_bkops_set(struct file *file, const char __user *buffer,
		unsigned long count, void *data)
{
	int value;
	struct mmc_host *host = (struct mmc_host*) data;
	if (!host)
		return count;

	sscanf(buffer, "%d", &value);
	host->bkops_trigger = value;
	pr_info("%s: %d\n", __func__, value);

	return count;
}

static void mmc_bkops_alarm_timer_handler(struct alarm *alarm)
{
	printk(KERN_ERR "[eMMC] %s()\n", __func__);

}

static int
msmsdcc_probe(struct platform_device *pdev)
{
	struct mmc_platform_data *plat;
	struct msmsdcc_host *host;
	struct mmc_host *mmc;
	unsigned long flags;
	struct resource *core_irqres = NULL;
	struct resource *bam_irqres = NULL;
	struct resource *core_memres = NULL;
	struct resource *dml_memres = NULL;
	struct resource *bam_memres = NULL;
	struct resource *dmares = NULL;
	struct resource *dma_crci_res = NULL;
	int ret = 0;
	int i;
	u32 version;

	if (pdev->dev.of_node) {
		plat = msmsdcc_populate_pdata(&pdev->dev);
		of_property_read_u32((&pdev->dev)->of_node,
				"cell-index", &pdev->id);
	} else {
		plat = pdev->dev.platform_data;
	}

	
	if (!plat) {
		pr_err("%s: Platform data not available\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	if (pdev->id < 1 || pdev->id > 5)
		return -EINVAL;

	if (plat->is_sdio_al_client && !plat->sdiowakeup_irq) {
		pr_err("%s: No wakeup IRQ for sdio_al client\n", __func__);
		return -EINVAL;
	}

	if (pdev->resource == NULL || pdev->num_resources < 2) {
		pr_err("%s: Invalid resource\n", __func__);
		return -ENXIO;
	}
	if (pdev->dev.of_node) {
		core_memres = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		dml_memres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		bam_memres = platform_get_resource(pdev, IORESOURCE_MEM, 2);
		core_irqres = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
		bam_irqres = platform_get_resource(pdev, IORESOURCE_IRQ, 1);
	} else {
		for (i = 0; i < pdev->num_resources; i++) {
			if (pdev->resource[i].flags & IORESOURCE_MEM) {
				if (!strncmp(pdev->resource[i].name,
						"sdcc_dml_addr",
						sizeof("sdcc_dml_addr")))
					dml_memres = &pdev->resource[i];
				else if (!strncmp(pdev->resource[i].name,
						"sdcc_bam_addr",
						sizeof("sdcc_bam_addr")))
					bam_memres = &pdev->resource[i];
				else
					core_memres = &pdev->resource[i];

			}
			if (pdev->resource[i].flags & IORESOURCE_IRQ) {
				if (!strncmp(pdev->resource[i].name,
						"sdcc_bam_irq",
						sizeof("sdcc_bam_irq")))
					bam_irqres = &pdev->resource[i];
				else
					core_irqres = &pdev->resource[i];
			}
			if (pdev->resource[i].flags & IORESOURCE_DMA) {
				if (!strncmp(pdev->resource[i].name,
						"sdcc_dma_chnl",
						sizeof("sdcc_dma_chnl")))
					dmares = &pdev->resource[i];
				else if (!strncmp(pdev->resource[i].name,
						"sdcc_dma_crci",
						sizeof("sdcc_dma_crci")))
					dma_crci_res = &pdev->resource[i];
			}
		}
	}

	if (!core_irqres || !core_memres) {
		pr_err("%s: Invalid sdcc core resource\n", __func__);
		return -ENXIO;
	}

	if ((bam_memres && !dml_memres) ||
		(!bam_memres && dml_memres) ||
		((bam_memres && dml_memres) && !bam_irqres)) {
		pr_err("%s: Invalid sdcc BAM/DML resource\n", __func__);
		return -ENXIO;
	}

	mmc = mmc_alloc_host(sizeof(struct msmsdcc_host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto out;
	}

	host = mmc_priv(mmc);
	host->pdev_id = pdev->id;
	host->plat = plat;
	host->mmc = mmc;
	host->curr.cmd = NULL;

	if (!plat->disable_bam && bam_memres && dml_memres && bam_irqres)
		set_hw_caps(host, MSMSDCC_SPS_BAM_SUP);
	else if (dmares)
		set_hw_caps(host, MSMSDCC_DMA_SUP);

	host->base = ioremap(core_memres->start,
			resource_size(core_memres));
	if (!host->base) {
		ret = -ENOMEM;
		goto host_free;
	}

	host->core_irqres = core_irqres;
	host->bam_irqres = bam_irqres;
	host->core_memres = core_memres;
	host->dml_memres = dml_memres;
	host->bam_memres = bam_memres;
	host->dmares = dmares;
	host->dma_crci_res = dma_crci_res;
	spin_lock_init(&host->lock);
	mutex_init(&host->clk_mutex);
	mutex_init(&host->rpm_mutex);
	host->enable_t = ktime_get();
	host->disable_t = host->enable_t;
	host->rpm_sus_t = host->enable_t;
	host->rpm_res_t = host->enable_t;

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	if (plat->embedded_sdio)
		mmc_set_embedded_sdio_data(mmc,
					   &plat->embedded_sdio->cis,
					   &plat->embedded_sdio->cccr,
					   plat->embedded_sdio->funcs,
					   plat->embedded_sdio->num_funcs);
#endif

	tasklet_init(&host->dma_tlet, msmsdcc_dma_complete_tlet,
			(unsigned long)host);

	tasklet_init(&host->sps.tlet, msmsdcc_sps_complete_tlet,
			(unsigned long)host);
	if (is_dma_mode(host)) {
		
		ret = msmsdcc_init_dma(host);
		if (ret)
			goto ioremap_free;
	} else {
		host->dma.channel = -1;
		host->dma.crci = -1;
	}

	if (is_mmc_platform(host->plat) || is_sd_platform(host->plat))
		mmc->tp_enable = 1;
	host->bus_clk = clk_get(&pdev->dev, "bus_clk");
	if (!IS_ERR_OR_NULL(host->bus_clk)) {
		
		ret = clk_set_rate(host->bus_clk, INT_MAX);
		if (ret)
			goto bus_clk_put;
		ret = clk_prepare_enable(host->bus_clk);
		if (ret)
			goto bus_clk_put;
	}

	host->pclk = clk_get(&pdev->dev, "iface_clk");
	if (!IS_ERR(host->pclk)) {
		ret = clk_prepare_enable(host->pclk);
		if (ret)
			goto pclk_put;

		host->pclk_rate = clk_get_rate(host->pclk);
	}

	host->clk = clk_get(&pdev->dev, "core_clk");
	if (IS_ERR(host->clk)) {
		ret = PTR_ERR(host->clk);
		goto pclk_disable;
	}

	ret = clk_set_rate(host->clk, msmsdcc_get_min_sup_clk_rate(host));
	if (ret) {
		pr_err("%s: Clock rate set failed (%d)\n", __func__, ret);
		goto clk_put;
	}

	ret = clk_prepare_enable(host->clk);
	if (ret)
		goto clk_put;

	host->clk_rate = clk_get_rate(host->clk);
	if (!host->clk_rate)
		dev_err(&pdev->dev, "Failed to read MCLK\n");

	set_default_hw_caps(host);

	host->reg_write_delay =
		(1 + ((3 * USEC_PER_SEC) /
		      msmsdcc_get_min_sup_clk_rate(host)));

	atomic_set(&host->clks_on, 1);
	
	msmsdcc_hard_reset(host);

#define MSM_MMC_DEFAULT_CPUDMA_LATENCY 200 
	
	if (host->plat->cpu_dma_latency)
		host->cpu_dma_latency = host->plat->cpu_dma_latency;
	else
		host->cpu_dma_latency = MSM_MMC_DEFAULT_CPUDMA_LATENCY;
	pm_qos_add_request(&host->pm_qos_req_dma,
			PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);

	ret = msmsdcc_msm_bus_register(host);
	if (ret)
		goto pm_qos_remove;

	if (host->msm_bus_vote.client_handle)
		INIT_DELAYED_WORK(&host->msm_bus_vote.vote_work,
				  msmsdcc_msm_bus_work);

	ret = msmsdcc_vreg_init(host, true);
	if (ret) {
		pr_err("%s: msmsdcc_vreg_init() failed (%d)\n", __func__, ret);
		goto clk_disable;
	}


	
	if (is_sps_mode(host)) {
		
		ret = msmsdcc_sps_init(host);
		if (ret)
			goto vreg_deinit;
		
		ret = msmsdcc_dml_init(host);
		if (ret)
			goto sps_exit;
	}
	mmc_dev(mmc)->dma_mask = &dma_mask;
	if (is_mmc_platform(host->plat)) {
		version = readl_relaxed(host->base + MCI_VERSION);
		if (version == 0x06000018) {
			
			host->hw_caps &= ~MSMSDCC_SOFT_RESET;
			pr_info("%s: hard_reset\n", mmc_hostname(host->mmc));
			msmsdcc_hard_reset(host);
			if (is_sps_mode(host))
				host->sps.reset_bam = true;
		}
	}

	if (is_sd_platform(host->plat) && (plat->status || plat->status_gpio) && plat->status_irq)
			mmc->ops = &msmsdcc_ops_sd;
	else
			mmc->ops = &msmsdcc_ops;
	mmc->f_min = msmsdcc_get_min_sup_clk_rate(host);
	mmc->f_max = msmsdcc_get_max_sup_clk_rate(host);
	mmc->ocr_avail = plat->ocr_mask;
	mmc->clkgate_delay = MSM_MMC_CLK_GATE_DELAY;

	mmc->pm_caps |= MMC_PM_KEEP_POWER | MMC_PM_WAKE_SDIO_IRQ;
	mmc->caps |= plat->mmc_bus_width;
	mmc->caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED;
	mmc->caps |= MMC_CAP_WAIT_WHILE_BUSY | MMC_CAP_ERASE;

	if (is_sd_platform(host->plat) || is_mmc_platform(host->plat))
		mmc->caps |= MMC_CAP_ERASE;

	if (!plat->disable_cmd23 && is_auto_prog_done(host))
		mmc->caps |= MMC_CAP_CMD23;

	mmc->caps |= plat->uhs_caps;
	mmc->caps2 |= plat->uhs_caps2;
	if (plat->xpc_cap)
		mmc->caps |= (MMC_CAP_SET_XPC_330 | MMC_CAP_SET_XPC_300 |
				MMC_CAP_SET_XPC_180);

	if (plat->pack_cmd_support) {
		mmc->caps2 |= MMC_CAP2_PACKED_WR;
		mmc->caps2 |= MMC_CAP2_PACKED_WR_CONTROL;
	}

	if (is_sd_platform(host->plat))
		mmc->caps2 |= MMC_CAP2_BOOTPART_NOACC;
	else
		mmc->caps2 |= (MMC_CAP2_BOOTPART_NOACC | MMC_CAP2_DETECT_ON_ERR);

	if (plat->sanitize_support)
		mmc->caps2 |= MMC_CAP2_SANITIZE;

	if (plat->hc_erase_group_def)
		mmc->caps2 |= MMC_CAP2_HC_ERASE_SZ;
	if (plat->nonremovable)
		mmc->caps |= MMC_CAP_NONREMOVABLE;
	mmc->caps |= MMC_CAP_SDIO_IRQ;

	if (plat->bkops_support) {
		mmc->caps2 |= MMC_CAP2_INIT_BKOPS | MMC_CAP2_BKOPS;
		
		alarm_init(&mmc->bkops_timer.bkops_alarm_timer,
		ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
		mmc_bkops_alarm_timer_handler);
	}
	if (plat->cache_support)
		mmc->caps2 |= MMC_CAP2_CACHE_CTRL;

	if (plat->is_sdio_al_client || is_mmc_platform(host->plat) || is_sd_platform(host->plat))
		mmc->pm_flags |= MMC_PM_IGNORE_PM_NOTIFY;

	
#ifdef CONFIG_WIMAX
	if (is_wifi_slot(host->plat) || is_wimax_platform(host->plat)) {
#else
	if (is_wifi_slot(host->plat)) {
#endif
		mmc->pm_flags |= MMC_PM_IGNORE_PM_NOTIFY;
	}

	

	mmc->max_segs = msmsdcc_get_nr_sg(host);
	mmc->max_blk_size = MMC_MAX_BLK_SIZE;
	mmc->max_blk_count = MMC_MAX_BLK_CNT;

	mmc->max_req_size = MMC_MAX_REQ_SIZE;
	mmc->max_seg_size = mmc->max_req_size;

	writel_relaxed(0, host->base + MMCIMASK0);
	writel_relaxed(MCI_CLEAR_STATIC_MASK, host->base + MMCICLEAR);
	msmsdcc_sync_reg_wr(host);

	writel_relaxed(MCI_IRQENABLE, host->base + MMCIMASK0);
	mb();
	host->mci_irqenable = MCI_IRQENABLE;

	ret = request_irq(core_irqres->start, msmsdcc_irq, IRQF_SHARED,
			  DRIVER_NAME " (cmd)", host);
	if (ret)
		goto dml_exit;

	ret = request_irq(core_irqres->start, msmsdcc_pio_irq, IRQF_SHARED,
			  DRIVER_NAME " (pio)", host);
	if (ret)
		goto irq_free;

	disable_irq(core_irqres->start);
	host->sdcc_irq_disabled = 1;

	if (plat->sdiowakeup_irq) {
		wake_lock_init(&host->sdio_wlock, WAKE_LOCK_SUSPEND,
				mmc_hostname(mmc));
		ret = request_irq(plat->sdiowakeup_irq,
			msmsdcc_platform_sdiowakeup_irq,
			IRQF_SHARED | IRQF_TRIGGER_LOW,
			DRIVER_NAME "sdiowakeup", host);
		if (ret) {
			pr_err("Unable to get sdio wakeup IRQ %d (%d)\n",
				plat->sdiowakeup_irq, ret);
			goto pio_irq_free;
		} else {
			spin_lock_irqsave(&host->lock, flags);
			if (!host->sdio_wakeupirq_disabled) {
				disable_irq_nosync(plat->sdiowakeup_irq);
				host->sdio_wakeupirq_disabled = 1;
			}
			spin_unlock_irqrestore(&host->lock, flags);
		}
	}

	if (host->plat->mpm_sdiowakeup_int) {
		wake_lock_init(&host->sdio_wlock, WAKE_LOCK_SUSPEND,
				mmc_hostname(mmc));
	}

	wake_lock_init(&host->sdio_suspend_wlock, WAKE_LOCK_SUSPEND,
			mmc_hostname(mmc));

	if (plat->status || plat->status_gpio) {
		if (plat->status)
			host->oldstat = plat->status(mmc_dev(host->mmc));
		else
			host->oldstat = msmsdcc_slot_status(host);

		host->eject = !host->oldstat;
	}

	if (plat->status_irq) {
		irq_set_irq_wake(plat->status_irq, 1);
		ret = request_threaded_irq(plat->status_irq, NULL,
				  msmsdcc_platform_status_irq,
				  plat->irq_flags,
				  DRIVER_NAME " (slot)",
				  host);
		if (ret) {
			pr_err("Unable to get slot IRQ %d (%d)\n",
			       plat->status_irq, ret);
			goto sdiowakeup_irq_free;
		}
	} else if (plat->register_status_notify) {
		plat->register_status_notify(msmsdcc_status_notify_cb, host);
	} else if (!plat->status)
		pr_err("%s: No card detect facilities available\n",
		       mmc_hostname(mmc));

	mmc_set_drvdata(pdev, mmc);

	ret = pm_runtime_set_active(&(pdev)->dev);
	if (ret < 0)
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, ret);
	pm_suspend_ignore_children(&(pdev)->dev, true);

#ifdef CONFIG_MMC_UNSAFE_RESUME
	pm_runtime_enable(&(pdev)->dev);
#else
	if (mmc->caps & MMC_CAP_NONREMOVABLE) {
		pm_runtime_enable(&(pdev)->dev);
	}
#endif
	setup_timer(&host->req_tout_timer, msmsdcc_req_tout_timer_hdlr,
			(unsigned long)host);

	mmc_add_host(mmc);

#ifdef CONFIG_HAS_EARLYSUSPEND
	host->early_suspend.suspend = msmsdcc_early_suspend;
	host->early_suspend.resume  = msmsdcc_late_resume;
	host->early_suspend.level   = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	register_early_suspend(&host->early_suspend);
#endif

	pr_info("%s: Qualcomm MSM SDCC-core at 0x%016llx irq %d,%d dma %d"
		" dmacrcri %d\n", mmc_hostname(mmc),
		(unsigned long long)core_memres->start,
		(unsigned int) core_irqres->start,
		(unsigned int) plat->status_irq, host->dma.channel,
		host->dma.crci);

	pr_info("%s: Controller capabilities: 0x%.8x\n",
			mmc_hostname(mmc), host->hw_caps);
	pr_info("%s: Platform slot type: %s\n", mmc_hostname(mmc),
		(plat->slot_type) ? mmc_type_str(*plat->slot_type) : "N/A");
	pr_info("%s: 8 bit data mode %s\n", mmc_hostname(mmc),
		(mmc->caps & MMC_CAP_8_BIT_DATA ? "enabled" : "disabled"));
	pr_info("%s: 4 bit data mode %s\n", mmc_hostname(mmc),
	       (mmc->caps & MMC_CAP_4_BIT_DATA ? "enabled" : "disabled"));
	pr_info("%s: polling status mode %s\n", mmc_hostname(mmc),
	       (mmc->caps & MMC_CAP_NEEDS_POLL ? "enabled" : "disabled"));
	pr_info("%s: MMC clock %u -> %u Hz, PCLK %u Hz\n",
	       mmc_hostname(mmc), msmsdcc_get_min_sup_clk_rate(host),
		msmsdcc_get_max_sup_clk_rate(host), host->pclk_rate);
	pr_info("%s: Slot eject status = %d\n", mmc_hostname(mmc),
	       host->eject);
	pr_info("%s: Power save feature enable = %d\n",
	       mmc_hostname(mmc), msmsdcc_pwrsave);
#if SD_DEBOUNCE_DEBUG
	if (is_sd_platform(host->plat)) {
		last_irq = ktime_set(0, 0);
		detect_wq = ktime_set(0, 0);
		irq_diff = ktime_set(0, 0);
		pr_info("%s: SD detect pin de-bounce time = %d ms\n",
			mmc_hostname(mmc), SD_detect_debounce_time);
	}
#endif
	if (is_dma_mode(host) && host->dma.channel != -1
			&& host->dma.crci != -1) {
		pr_info("%s: DM non-cached buffer at %p, dma_addr 0x%.8x\n",
		       mmc_hostname(mmc), host->dma.nc, host->dma.nc_busaddr);
		pr_info("%s: DM cmd busaddr 0x%.8x, cmdptr busaddr 0x%.8x\n",
		       mmc_hostname(mmc), host->dma.cmd_busaddr,
		       host->dma.cmdptr_busaddr);
	} else if (is_sps_mode(host)) {
		pr_info("%s: SPS-BAM data transfer mode available\n",
			mmc_hostname(mmc));
	} else
		pr_info("%s: PIO transfer enabled\n", mmc_hostname(mmc));

#if defined(CONFIG_DEBUG_FS)
	msmsdcc_dbg_createhost(host);
#endif

	host->max_bus_bw.show = show_sdcc_to_mem_max_bus_bw;
	host->max_bus_bw.store = store_sdcc_to_mem_max_bus_bw;
	sysfs_attr_init(&host->max_bus_bw.attr);
	host->max_bus_bw.attr.name = "max_bus_bw";
	host->max_bus_bw.attr.mode = S_IRUGO | S_IWUSR;
	ret = device_create_file(&pdev->dev, &host->max_bus_bw);
	if (ret)
		goto platform_irq_free;

	if (!plat->status_irq) {
		host->polling.show = show_polling;
		host->polling.store = store_polling;
		sysfs_attr_init(&host->polling.attr);
		host->polling.attr.name = "polling";
		host->polling.attr.mode = S_IRUGO | S_IWUSR;
		ret = device_create_file(&pdev->dev, &host->polling);
		if (ret)
			goto remove_max_bus_bw_file;
	}
	if (is_mmc_platform(host->plat)) {
		host->wr_perf_proc = create_proc_entry("emmc_wr_perf", 0444, NULL);
		if (host->wr_perf_proc) {
			host->wr_perf_proc->read_proc = msmsdcc_proc_wperf_show;
			host->wr_perf_proc->data = (void *) host->mmc;
		} else
			pr_warning("%s: Failed to create emmc_wr_perf entry\n",
				mmc_hostname(host->mmc));

		host->burst_proc = create_proc_entry("emmc_burst", 0664, NULL);
		if (host->burst_proc) {
			host->burst_proc->read_proc = msmsdcc_proc_burst_show;
			host->burst_proc->write_proc = msmsdcc_proc_burst_set;
			host->burst_proc->data = (void *) host->mmc;
		} else
			pr_warning("%s: Failed to create emmc_burst entry\n",
				mmc_hostname(host->mmc));

		host->bkops_proc = create_proc_entry("emmc_bkops", 0664, NULL);
		if (host->bkops_proc) {
			host->bkops_proc->read_proc = msmsdcc_proc_bkops_show;
			host->bkops_proc->write_proc = msmsdcc_proc_bkops_set;
			host->bkops_proc->data = (void *) host->mmc;
		} else
			pr_warning("%s: Failed to create emmc_bkops entry\n",
				mmc_hostname(host->mmc));
	}
	if(is_sd_platform(host->plat)) {
		host->speed_class = create_proc_entry("sd_speed_class", 0444, NULL);
		if (host->speed_class) {
			host->speed_class->read_proc = msmsdcc_proc_speed_class;
			host->speed_class->data = (void *) host->mmc;
		} else
			pr_warning("%s: Failed to create sd_speed_class entry\n",
				mmc_hostname(host->mmc));
	}
	if (!is_auto_cmd19(host))
		goto exit;

	
	host->auto_cmd19_attr.show = show_enable_auto_cmd19;
	host->auto_cmd19_attr.store = store_enable_auto_cmd19;
	sysfs_attr_init(&host->auto_cmd19_attr.attr);
	host->auto_cmd19_attr.attr.name = "enable_auto_cmd19";
	host->auto_cmd19_attr.attr.mode = S_IRUGO | S_IWUSR;
	ret = device_create_file(&pdev->dev, &host->auto_cmd19_attr);
	if (ret)
		goto remove_polling_file;

exit:
	return 0;
 remove_polling_file:
	device_remove_file(&pdev->dev, &host->polling);
 remove_max_bus_bw_file:
	device_remove_file(&pdev->dev, &host->max_bus_bw);
 platform_irq_free:
	del_timer_sync(&host->req_tout_timer);
	pm_runtime_disable(&(pdev)->dev);
	pm_runtime_set_suspended(&(pdev)->dev);

	if (plat->status_irq)
		free_irq(plat->status_irq, host);
 sdiowakeup_irq_free:
	wake_lock_destroy(&host->sdio_suspend_wlock);
	if (plat->sdiowakeup_irq)
		free_irq(plat->sdiowakeup_irq, host);
 pio_irq_free:
	if (plat->sdiowakeup_irq)
		wake_lock_destroy(&host->sdio_wlock);
	free_irq(core_irqres->start, host);
 irq_free:
	free_irq(core_irqres->start, host);
 dml_exit:
	if (is_sps_mode(host))
		msmsdcc_dml_exit(host);
 sps_exit:
	if (is_sps_mode(host))
		msmsdcc_sps_exit(host);
 vreg_deinit:
	msmsdcc_vreg_init(host, false);
 clk_disable:
	clk_disable(host->clk);
	msmsdcc_msm_bus_unregister(host);
 pm_qos_remove:
	if (host->cpu_dma_latency)
		pm_qos_remove_request(&host->pm_qos_req_dma);
 clk_put:
	clk_put(host->clk);
 pclk_disable:
	if (!IS_ERR(host->pclk))
		clk_disable_unprepare(host->pclk);
 pclk_put:
	if (!IS_ERR(host->pclk))
		clk_put(host->pclk);
	if (!IS_ERR_OR_NULL(host->bus_clk))
		clk_disable_unprepare(host->bus_clk);
 bus_clk_put:
	if (!IS_ERR_OR_NULL(host->bus_clk))
		clk_put(host->bus_clk);
	if (is_dma_mode(host)) {
		if (host->dmares)
			dma_free_coherent(NULL,
				sizeof(struct msmsdcc_nc_dmadata),
				host->dma.nc, host->dma.nc_busaddr);
	}
 ioremap_free:
	iounmap(host->base);
 host_free:
	mmc_free_host(mmc);
 out:
	return ret;
}

static void msmsdcc_shutdown(struct platform_device *pdev)
{
	struct mmc_host *mmc = mmc_get_drvdata(pdev);
	struct msmsdcc_host *host;
	int err;
	if (!mmc)
		return;
	host = mmc_priv(mmc);
	if (host && is_mmc_platform(host->plat) && mmc->card && mmc->card->cid.manfid == 0x90 && mmc->card->ext_csd.sectors == 15155200) {
		pr_info("%s: %s\n", mmc_hostname(mmc), __func__);
		mmc_claim_host(mmc->card->host);
		err = mmc_switch(mmc->card, EXT_CSD_CMD_SET_NORMAL,
			EXT_CSD_BUS_WIDTH, EXT_CSD_BUS_WIDTH_4, 1000);
		if (err)
			pr_err("%s: switch cmd err %d\n", mmc_hostname(mmc), err);
		mmc_release_host(mmc->card->host);
		pr_info("%s: %s return\n", mmc_hostname(mmc), __func__);
	}
}

static int msmsdcc_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = mmc_get_drvdata(pdev);
	struct mmc_platform_data *plat;
	struct msmsdcc_host *host;

	if (!mmc)
		return -ENXIO;

	if (pm_runtime_suspended(&(pdev)->dev))
		pm_runtime_resume(&(pdev)->dev);

	host = mmc_priv(mmc);

	DBG(host, "Removing SDCC device = %d\n", pdev->id);
	plat = host->plat;

	if (is_auto_cmd19(host))
		device_remove_file(&pdev->dev, &host->auto_cmd19_attr);
	device_remove_file(&pdev->dev, &host->max_bus_bw);
	if (!plat->status_irq)
		device_remove_file(&pdev->dev, &host->polling);

	del_timer_sync(&host->req_tout_timer);
	tasklet_kill(&host->dma_tlet);
	tasklet_kill(&host->sps.tlet);
	mmc_remove_host(mmc);

	if (plat->status_irq)
		free_irq(plat->status_irq, host);

	wake_lock_destroy(&host->sdio_suspend_wlock);
	if (plat->sdiowakeup_irq) {
		wake_lock_destroy(&host->sdio_wlock);
		irq_set_irq_wake(plat->sdiowakeup_irq, 0);
		free_irq(plat->sdiowakeup_irq, host);
	}

	free_irq(host->core_irqres->start, host);
	free_irq(host->core_irqres->start, host);

	clk_put(host->clk);
	if (!IS_ERR(host->pclk))
		clk_put(host->pclk);
	if (!IS_ERR_OR_NULL(host->bus_clk))
		clk_put(host->bus_clk);

	if (host->cpu_dma_latency)
		pm_qos_remove_request(&host->pm_qos_req_dma);

	if (host->msm_bus_vote.client_handle) {
		msmsdcc_msm_bus_cancel_work_and_set_vote(host, NULL);
		msmsdcc_msm_bus_unregister(host);
	}

	msmsdcc_vreg_init(host, false);

	if (is_dma_mode(host)) {
		if (host->dmares)
			dma_free_coherent(NULL,
					sizeof(struct msmsdcc_nc_dmadata),
					host->dma.nc, host->dma.nc_busaddr);
	}

	if (is_sps_mode(host)) {
		msmsdcc_dml_exit(host);
		msmsdcc_sps_exit(host);
	}

	iounmap(host->base);
	mmc_free_host(mmc);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&host->early_suspend);
#endif
	pm_runtime_disable(&(pdev)->dev);
	pm_runtime_set_suspended(&(pdev)->dev);
	if (host->wr_perf_proc)
		remove_proc_entry("emmc_wr_perf", NULL);
	if (host->burst_proc)
		remove_proc_entry("emmc_burst", NULL);
	if(host->speed_class)
		remove_proc_entry("sd_speed_class", NULL);

	return 0;
}

#ifdef CONFIG_MSM_SDIO_AL
int msmsdcc_sdio_al_lpm(struct mmc_host *mmc, bool enable)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int rc = 0;

	mutex_lock(&host->clk_mutex);
	spin_lock_irqsave(&host->lock, flags);
	pr_debug("%s: %sabling LPM\n", mmc_hostname(mmc),
			enable ? "En" : "Dis");

	if (enable) {
		if (!host->sdcc_irq_disabled) {
			writel_relaxed(0, host->base + MMCIMASK0);
			disable_irq_nosync(host->core_irqres->start);
			host->sdcc_irq_disabled = 1;
		}
		rc = msmsdcc_setup_clocks(host, false);
		if (rc)
			goto out;

		if (host->plat->sdio_lpm_gpio_setup &&
				!host->sdio_gpio_lpm) {
			spin_unlock_irqrestore(&host->lock, flags);
			host->plat->sdio_lpm_gpio_setup(mmc_dev(mmc), 0);
			spin_lock_irqsave(&host->lock, flags);
			host->sdio_gpio_lpm = 1;
		}

		if (host->sdio_wakeupirq_disabled) {
			msmsdcc_enable_irq_wake(host);
			enable_irq(host->plat->sdiowakeup_irq);
			host->sdio_wakeupirq_disabled = 0;
		}
	} else {
		rc = msmsdcc_setup_clocks(host, true);
		if (rc)
			goto out;

		if (!host->sdio_wakeupirq_disabled) {
			disable_irq_nosync(host->plat->sdiowakeup_irq);
			host->sdio_wakeupirq_disabled = 1;
			msmsdcc_disable_irq_wake(host);
		}

		if (host->plat->sdio_lpm_gpio_setup &&
				host->sdio_gpio_lpm) {
			spin_unlock_irqrestore(&host->lock, flags);
			host->plat->sdio_lpm_gpio_setup(mmc_dev(mmc), 1);
			spin_lock_irqsave(&host->lock, flags);
			host->sdio_gpio_lpm = 0;
		}

		if (host->sdcc_irq_disabled && atomic_read(&host->clks_on)) {
			writel_relaxed(host->mci_irqenable,
				       host->base + MMCIMASK0);
			mb();
			enable_irq(host->core_irqres->start);
			host->sdcc_irq_disabled = 0;
		}
	}
out:
	spin_unlock_irqrestore(&host->lock, flags);
	mutex_unlock(&host->clk_mutex);
	return rc;
}
#else
int msmsdcc_sdio_al_lpm(struct mmc_host *mmc, bool enable)
{
	return 0;
}
#endif

#ifdef CONFIG_PM
#ifdef CONFIG_MMC_CLKGATE
static inline void msmsdcc_gate_clock(struct msmsdcc_host *host)
{
	struct mmc_host *mmc = host->mmc;
	unsigned long flags;

	mmc_host_clk_hold(mmc);
	spin_lock_irqsave(&mmc->clk_lock, flags);
	mmc->clk_old = mmc->ios.clock;
	mmc->ios.clock = 0;
	mmc->clk_gated = true;
	spin_unlock_irqrestore(&mmc->clk_lock, flags);
	mmc_set_ios(mmc);
	mmc_host_clk_release(mmc);
}

static inline void msmsdcc_ungate_clock(struct msmsdcc_host *host)
{
	struct mmc_host *mmc = host->mmc;

	mmc_host_clk_hold(mmc);
	mmc->ios.clock = host->clk_rate;
	mmc_set_ios(mmc);
	mmc_host_clk_release(mmc);
}
#else
static inline void msmsdcc_gate_clock(struct msmsdcc_host *host)
{
	struct mmc_host *mmc = host->mmc;

	mmc->ios.clock = 0;
	mmc_set_ios(mmc);
}

static inline void msmsdcc_ungate_clock(struct msmsdcc_host *host)
{
	struct mmc_host *mmc = host->mmc;

	mmc->ios.clock = host->clk_rate;
	mmc_set_ios(mmc);
}
#endif

static int
msmsdcc_suspend(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;
	unsigned long flags;

	if (host->plat->is_sdio_al_client) {
		rc = 0;
		goto out;
	}

	if (mmc) {

#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s enter\n", mmc_hostname(host->mmc), __func__);
#endif

		host->sdcc_suspending = 1;
		mmc->suspend_task = current;

		pm_runtime_get_noresume(dev);
		
		if (unlikely(work_busy(&mmc->detect.work)))
			rc = -EAGAIN;
		else
		
		{
#ifdef CONFIG_WIMAX
			if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
				if (mmc_wimax_get_disable_irq_config() == 1)
					wimax_disable_irq(host->mmc);

			
			if (!is_wifi_slot(host->plat) && !is_wimax_platform(host->plat)) {

				pr_info("[MMC] %s: %s mmc_suspend_host\n", mmc_hostname(host->mmc), __func__);
#else
			
			if (!is_wifi_slot(host->plat)) {
#endif
				rc = mmc_suspend_host(mmc);
				if (!rc && is_mmc_platform(host->plat))
					msmsdcc_gate_clock(host);
			}
		}
		
		pm_runtime_put_noidle(dev);

		if (!rc) {
			spin_lock_irqsave(&host->lock, flags);
			host->sdcc_suspended = true;
			spin_unlock_irqrestore(&host->lock, flags);
			if (mmc->card && mmc_card_sdio(mmc->card) &&
				mmc->ios.clock) {
				msmsdcc_gate_clock(host);
			}
		}
		host->sdcc_suspending = 0;
		mmc->suspend_task = NULL;
		if (rc && wake_lock_active(&host->sdio_suspend_wlock))
			wake_unlock(&host->sdio_suspend_wlock);
	}
out:
	
	msmsdcc_msm_bus_cancel_work_and_set_vote(host, NULL);
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s leave\n", mmc_hostname(host->mmc), __func__);
#endif
	return rc;
}

static int
msmsdcc_resume(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;

#ifdef CONFIG_WIFI_MMC
	if (is_wifi_platform(host->plat)) {
		printk(KERN_DEBUG "[WIFI] [MMC] %s: %s enter,", mmc_hostname(host->mmc), __func__);
		PRINTRTC;
	}
#endif
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s enter\n", mmc_hostname(host->mmc), __func__);
#endif

	if (host->plat->is_sdio_al_client)
		return 0;

	if (mmc) {
		if (mmc->card && mmc_card_sdio(mmc->card) &&
				mmc_card_keep_power(mmc)) {
			msmsdcc_ungate_clock(host);
		}

#ifdef CONFIG_WIMAX
	
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
		
		pr_info("[WIMAX] [MMC] %s: %s msmsdcc_ungate_clock 2\n", mmc_hostname(host->mmc), __func__);
		msmsdcc_ungate_clock(host);
		writel_relaxed(host->mci_irqenable | host->cmd_pio_irqmask,
							host->base + MMCIMASK0);
		mb();
	}
#endif

		
#ifdef CONFIG_WIMAX
		
		if (!is_wifi_slot(host->plat) && !is_wimax_platform(host->plat)) {
			pr_info("[MMC] %s: %s mmc_resume_host\n", mmc_hostname(host->mmc), __func__);
#else
		
		if (!is_wifi_slot(host->plat)) {
#endif
			mmc_resume_host(mmc);
		}
		


		spin_lock_irqsave(&host->lock, flags);
		mmc->pm_flags = 0;
		host->sdcc_suspended = false;
		spin_unlock_irqrestore(&host->lock, flags);

		if (mmc->card && mmc_card_sdio(mmc->card)) {
			if ((host->plat->mpm_sdiowakeup_int ||
					host->plat->sdiowakeup_irq) &&
					wake_lock_active(&host->sdio_wlock))
				wake_lock_timeout(&host->sdio_wlock, 1);
		}

		wake_unlock(&host->sdio_suspend_wlock);
	}
	pr_debug("%s: %s: end\n", mmc_hostname(mmc), __func__);
#ifdef CONFIG_WIMAX
       

#ifdef WIMAX_RESUME_IRQ_IN_SYSTEM_RESUME
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			if (mmc_wimax_get_disable_irq_config() == 1)
				wimax_enable_irq(host->mmc);
#endif

	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s leave\n", mmc_hostname(host->mmc), __func__);
#endif
	return 0;
}

static int
msmsdcc_runtime_suspend_simple(struct msmsdcc_host *host)
{
	unsigned long		flags;

	if (host->curr.mrq) {
		WARN(host->curr.mrq, "Request in progress\n");
		return 0;
	}
	mutex_lock(&host->clk_mutex);
	msmsdcc_setup_clocks(host, false);
	mutex_unlock(&host->clk_mutex);

	spin_lock_irqsave(&host->lock, flags);
	if (!host->sdcc_irq_disabled && host->mmc->card) {
		disable_irq_nosync(host->core_irqres->start);
		host->sdcc_irq_disabled = 1;
	}
	spin_unlock_irqrestore(&host->lock, flags);

	return 0;
}

static int
msmsdcc_runtime_resume_simple(struct msmsdcc_host *host)
{
	unsigned long		flags;

	spin_lock_irqsave(&host->lock, flags);
	if (host->sdcc_irq_disabled) {
		enable_irq(host->core_irqres->start);
		host->sdcc_irq_disabled = 0;
	}
	spin_unlock_irqrestore(&host->lock, flags);

	mutex_lock(&host->clk_mutex);
	msmsdcc_setup_clocks(host, true);
	mutex_unlock(&host->clk_mutex);

	return 0;
}

static int
msmsdcc_runtime_suspend(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;
	unsigned long flags;

	if (host->plat->is_sdio_al_client) {
		rc = 0;
		goto out;
	}

	pr_debug("%s: %s: start\n", mmc_hostname(mmc), __func__);
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s enter\n", mmc_hostname(host->mmc), __func__);
#endif

	if (is_sd_platform(host->plat))
		mutex_lock(&host->rpm_mutex);

	if (is_sd_platform(host->plat)) {
		if (host->curr.mrq) {
			pr_info("Request in progress, stop runtime suspend\n");
			msmsdcc_print_rpm_info(host);
			mutex_unlock(&host->rpm_mutex);
			return 0;
		}
	}

	if (mmc) {
		if (is_mmc_platform(host->plat) && host->curr.mrq) {
			pr_info("%s: %s: Request in progress, stop runtime suspend (usage=%d)\n",
					mmc_hostname(mmc), __func__,
					atomic_read(&dev->power.usage_count));
			return 0;
		}
#ifdef CONFIG_WIFI_MMC
    	if (is_wifi_platform(host->plat)) {
    	    
    	    if (mmc->card && mmc_card_sdio(mmc->card) && host->is_runtime_resumed)
    		  return 0;
    	}
		if (is_wifi_platform(host->plat)) {
			printk("[MMC][WIFI] %s: %s: start\n", mmc_hostname(mmc), __func__);
		}
#endif
#ifdef CONFIG_WIMAX
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
			
			if (mmc->card && mmc_card_sdio(mmc->card) && host->is_runtime_resumed_wimax)
			return 0;
		}
		if (is_wimax_platform(host->plat)&& mmc_wimax_get_status()) {
			printk("[MMC][WIMAX] %s: %s: start\n", mmc_hostname(mmc), __func__);
		}
#endif

		if (is_mmc_platform(host->plat))
			return msmsdcc_runtime_suspend_simple(host);

		host->sdcc_suspending = 1;
		mmc->suspend_task = current;

		pm_runtime_get_noresume(dev);
		
		if (unlikely(work_busy(&mmc->detect.work)))
			rc = -EAGAIN;
		else
		
		{
#ifdef CONFIG_WIMAX
			if (!is_wifi_slot(host->plat) && !is_sd_platform(host->plat) && !is_wimax_platform(host->plat)) {
#else
			if (!is_wifi_slot(host->plat) && !is_sd_platform(host->plat)) {
#endif
				pr_info("[MMC] %s: %s mmc_suspend_host\n", mmc_hostname(host->mmc), __func__);
				rc = mmc_suspend_host(mmc);
			}
		}
		
		pm_runtime_put_noidle(dev);

		if (!rc) {
			spin_lock_irqsave(&host->lock, flags);
			host->sdcc_suspended = true;
			spin_unlock_irqrestore(&host->lock, flags);
			if (mmc->card && ((mmc_card_sdio(mmc->card) &&
				mmc->ios.clock) || mmc_card_sd(mmc->card))) {
				if (is_sd_platform(host->plat))
					host->rpm_sus_t = ktime_get();
				msmsdcc_gate_clock(host);
				
				spin_lock_irqsave(&host->lock, flags);
				if (mmc_card_sd(mmc->card) && !host->sdcc_irq_disabled) {
					disable_irq_nosync(host->core_irqres->start);
					host->sdcc_irq_disabled = 1;
					spin_unlock_irqrestore(&host->lock, flags);
					
					synchronize_irq(host->core_irqres->start);
					spin_lock_irqsave(&host->lock, flags);
				}
				spin_unlock_irqrestore(&host->lock, flags);
			}
		}
		host->sdcc_suspending = 0;
		mmc->suspend_task = NULL;
		if (rc && wake_lock_active(&host->sdio_suspend_wlock))
			wake_unlock(&host->sdio_suspend_wlock);
	}
	if (is_sd_platform(host->plat))
		mutex_unlock(&host->rpm_mutex);
	pr_debug("%s: %s: ends with err=%d\n", mmc_hostname(mmc), __func__, rc);
out:
	
	msmsdcc_msm_bus_cancel_work_and_set_vote(host, NULL);

#ifdef CONFIG_WIFI
	if (is_wifi_platform(host->plat) && mmc_wimax_get_status()) {
		host->is_runtime_resumed = true;
	}
#endif
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		host->is_runtime_resumed_wimax = true;

	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s leave\n", mmc_hostname(host->mmc), __func__);
#endif

	return rc;
}

static int
msmsdcc_runtime_resume(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;

	if (host->plat->is_sdio_al_client)
		return 0;

	pr_debug("%s: %s: start\n", mmc_hostname(mmc), __func__);
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s enter\n", mmc_hostname(host->mmc), __func__);
#endif

	if (is_sd_platform(host->plat))
		mutex_lock(&host->rpm_mutex);
#ifdef CONFIG_WIFI_MMC
    	if (is_wifi_platform(host->plat)) {
    	    printk("[MMC][WIFI] %s: %s: start\n", mmc_hostname(mmc), __func__);
    	}
#endif

	if (mmc) {
		if (is_mmc_platform(host->plat))
			return msmsdcc_runtime_resume_simple(host);

		if (mmc->card && ((mmc_card_sdio(mmc->card) &&
				mmc_card_keep_power(mmc)) || mmc_card_sd(mmc->card))) {
			if (is_sd_platform(host->plat))
				host->rpm_res_t = ktime_get();
			msmsdcc_ungate_clock(host);
			
			spin_lock_irqsave(&host->lock, flags);
			if (mmc_card_sd(mmc->card) && host->sdcc_irq_disabled) {
				enable_irq(host->core_irqres->start);
				host->sdcc_irq_disabled = 0;
			}
			spin_unlock_irqrestore(&host->lock, flags);
		}

		
#ifdef CONFIG_WIMAX
		if (!is_wifi_slot(host->plat) && !is_sd_platform(host->plat) && !is_wimax_platform(host->plat)) {
#else
		if (!is_wifi_slot(host->plat) && !is_sd_platform(host->plat)) {
#endif
			pr_info("[MMC] %s: %s mmc_suspend_host\n", mmc_hostname(host->mmc), __func__);
			mmc_resume_host(mmc);
		}
		


		spin_lock_irqsave(&host->lock, flags);
		mmc->pm_flags = 0;
		host->sdcc_suspended = false;
		spin_unlock_irqrestore(&host->lock, flags);

		if (mmc->card && mmc_card_sdio(mmc->card)) {
			if ((host->plat->mpm_sdiowakeup_int ||
					host->plat->sdiowakeup_irq) &&
					wake_lock_active(&host->sdio_wlock))
				wake_lock_timeout(&host->sdio_wlock, 1);
		}

		wake_unlock(&host->sdio_suspend_wlock);
	}
	if (is_sd_platform(host->plat))
		mutex_unlock(&host->rpm_mutex);
	pr_debug("%s: %s: end\n", mmc_hostname(mmc), __func__);

#ifdef CONFIG_WIFI_MMC
    	if (is_wifi_platform(host->plat)) {
    		host->is_runtime_resumed = false;
    	}
#endif

#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		host->is_runtime_resumed_wimax = false;

	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s leave\n", mmc_hostname(host->mmc), __func__);
#endif
	return 0;
}

static int msmsdcc_runtime_idle(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);

	if (host->plat->is_sdio_al_client)
		return 0;

	
	if (is_mmc_platform(host->plat))
		pm_schedule_suspend(dev, MSM_MMC_IDLE_TIMEOUT_EMMC);
#ifdef CONFIG_WIMAX
	else if (is_wimax_platform(host->plat))
	    pm_schedule_suspend(dev, MSM_MMC_WIMAX_IDLE_TIMEOUT);
#endif
	else
		pm_schedule_suspend(dev, MSM_MMC_IDLE_TIMEOUT);

	return -EAGAIN;
}

static int emmc_bkops_prerun = 0;
#define EMMC_BKOPS_PRERUN_TIMER	180000
static int msmsdcc_pm_prepare_suspend(struct device *dev)
{
	int err = 0;
	long alarm_sec;
	ktime_t interval;
	ktime_t next_alarm;
	struct timespec xtime;
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);

	pr_info("%s: %s enter\n", mmc_hostname(host->mmc), __func__);
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
		pr_info("[WIMAX] [MMC] %s: %s enter , return!\n", mmc_hostname(host->mmc), __func__);
		return err;
	}
#endif

	if (!is_mmc_platform(host->plat) || !mmc->card || !mmc->card->ext_csd.bkops_en) {
		if (is_mmc_platform(host->plat) && mmc->card) {
			pr_info("%s: %s leave (bkops_en %d)\n", mmc_hostname(host->mmc), __func__, mmc->card->ext_csd.bkops_en);
			if (mmc->bkops_trigger)
				mmc->bkops_trigger = 0;
		}
		pr_info("%s: %s leave\n", mmc_hostname(host->mmc), __func__);
		return err;
	}

	mmc_claim_host(mmc);

	mmc->bkops_alarm_set = 0;
	if (mmc->bkops_trigger || mmc->bkops_timer.need_bkops || emmc_bkops_prerun) {
		if (emmc_bkops_prerun) {
			mmc->bkops_timer.need_bkops = EMMC_BKOPS_PRERUN_TIMER; 
			emmc_bkops_prerun = 0;
			mmc->bkops_trigger = 0;
		}
		if (!mmc->bkops_timer.need_bkops) {
			mmc->bkops_timer.need_bkops = mmc->bkops_trigger; 
			mmc->bkops_timer.bkops_start = 0;
			mmc->bkops_trigger = 0;
		}
		if (mmc->bkops_timer.need_bkops >= 1000) {
			if (!mmc_card_doing_bkops(mmc->card)) {
				err = mmc_card_start_bkops(mmc);
				if (!err)
					mmc->bkops_started = 1;
				else
					mmc->bkops_started = 0;
			} else {
				mmc->bkops_started = 1;
				pr_info("%s: bkops already start\n", mmc_hostname(host->mmc));
			}
		} else {
			mmc->bkops_started = 0;
			mmc->bkops_timer.need_bkops = 0;
		}
	}

	if (mmc->bkops_started) {
		mmc->bkops_trigger = 0;
		pr_info("%s: bkops remain %d\n", mmc_hostname(host->mmc),
				mmc->bkops_timer.need_bkops);
		alarm_sec = ((u32)mmc->bkops_timer.need_bkops + 999) / 1000;
		interval = ktime_set(alarm_sec, 0);
		next_alarm = ktime_add(alarm_get_elapsed_realtime(), interval);

		alarm_start_range(&mmc->bkops_timer.bkops_alarm_timer,
			next_alarm, ktime_add(next_alarm, ktime_set(0, 0)));
		mmc->bkops_alarm_set = 1;

		xtime = CURRENT_TIME;
		mmc->bkops_timer.bkops_start = xtime.tv_sec * MSEC_PER_SEC +
					xtime.tv_nsec / NSEC_PER_MSEC;
	}
	mmc_release_host(mmc);
	pr_info("%s: %s leave err = %d\n", mmc_hostname(host->mmc), __func__, err);
	return err;
}

static void msmsdcc_pm_complete(struct device *dev)
{
	int err = 0;
	unsigned long flags;
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);

	pr_info("%s: %s enter\n", mmc_hostname(host->mmc), __func__);
#ifdef CONFIG_WIMAX
#ifndef WIMAX_RESUME_IRQ_IN_SYSTEM_RESUME
		if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
			if (mmc_wimax_get_disable_irq_config() == 1)
				wimax_enable_irq(host->mmc);
#endif
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status()) {
		pr_info("[WIMAX] [MMC] %s: %s enter, return!!\n", mmc_hostname(host->mmc), __func__);
		return ;
	}
#endif

	if (is_mmc_platform(host->plat)) {
		if (mmc->bkops_alarm_set) {
			alarm_cancel(&mmc->bkops_timer.bkops_alarm_timer);
			spin_lock_irqsave(&mmc->lock, flags);
			mmc->bkops_alarm_set = 0;
			spin_unlock_irqrestore(&mmc->lock, flags);
		}
		if (mmc->bkops_started) {
			msmsdcc_ungate_clock(host);
			err = mmc_bkops_resume_task(mmc);
			if (!err) {
				spin_lock_irqsave(&mmc->lock, flags);
				mmc->bkops_started = 0;
				if (mmc_card_doing_bkops(mmc->card))
					mmc_card_clr_doing_bkops(mmc->card);
				spin_unlock_irqrestore(&mmc->lock, flags);
				if (mmc_bus_needs_resume(mmc)) {
					spin_lock_irqsave(&mmc->lock, flags);
					mmc->bus_resume_flags &= ~MMC_BUSRESUME_NEEDS_RESUME;
					spin_unlock_irqrestore(&mmc->lock, flags);
					err = msmsdcc_suspend(dev);
					
					if (err)
						pr_info("%s: %s suspend_host fail\n", mmc_hostname(host->mmc), __func__);
					else
						pr_info("%s: %s suspend_host success\n", mmc_hostname(host->mmc), __func__);
				}
			} else
				pr_info("%s: %s mmc_bkops_resume_task fail\n", mmc_hostname(host->mmc), __func__);

		}
	}
	pr_info("%s: %s leave\n", mmc_hostname(host->mmc), __func__);
	return;
}

static int msmsdcc_pm_suspend(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;
	pr_info("%s: %s enter\n", mmc_hostname(mmc), __func__);
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s ...\n", mmc_hostname(host->mmc), __func__);
#endif

	if (host->plat->is_sdio_al_client)
		return 0;


	if (!is_sd_platform(host->plat) && host->plat->status_irq)
		disable_irq(host->plat->status_irq);

	if (!pm_runtime_suspended(dev) || is_sd_platform(host->plat) || is_mmc_platform(host->plat))
		rc = msmsdcc_suspend(dev);

	pr_info("%s: %s leave rc %d\n", mmc_hostname(mmc), __func__, rc);
	return rc;
}

static int msmsdcc_suspend_noirq(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;


	if (atomic_read(&host->clks_on) && !host->plat->is_sdio_al_client) {
		pr_warn("%s: clocks are on after suspend, aborting system "
				"suspend\n", mmc_hostname(mmc));
		rc = -EAGAIN;
	}

	return rc;
}

static int msmsdcc_pm_resume(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;
	pr_info("%s: %s enter\n", mmc_hostname(mmc), __func__);
#ifdef CONFIG_WIMAX
	if (is_wimax_platform(host->plat) && mmc_wimax_get_status())
		pr_info("[WIMAX] [MMC] %s: %s ...\n", mmc_hostname(host->mmc), __func__);
#endif

	if (host->plat->is_sdio_al_client)
		return 0;

	if (mmc->card && (mmc_card_sdio(mmc->card) || mmc_card_sd(mmc->card) || mmc_card_mmc(mmc->card))) {
		rc = msmsdcc_resume(dev);
		host->pending_resume = true;
	} else
		host->pending_resume = true;

	if (!is_sd_platform(host->plat) && host->plat->status_irq) {
		msmsdcc_check_status((unsigned long)host);
		enable_irq(host->plat->status_irq);
	}

	pr_info("%s: %s leave\n", mmc_hostname(mmc), __func__);
	return rc;
}

#else
static int msmsdcc_runtime_suspend(struct device *dev)
{
	return 0;
}
static int msmsdcc_runtime_idle(struct device *dev)
{
	return 0;
}
static int msmsdcc_pm_suspend(struct device *dev)
{
	return 0;
}
static int msmsdcc_pm_resume(struct device *dev)
{
	return 0;
}
static int msmsdcc_suspend_noirq(struct device *dev)
{
	return 0;
}
static int msmsdcc_runtime_resume(struct device *dev)
{
	return 0;
}
static int msmsdcc_pm_prepare_suspend(struct device *dev)
{
	reutrn 0;
}
static int msmsdcc_pm_complete(struct device *dev)
{
	return 0;
}
#endif

static const struct dev_pm_ops msmsdcc_dev_pm_ops = {
	.runtime_suspend = msmsdcc_runtime_suspend,
	.runtime_resume  = msmsdcc_runtime_resume,
	.runtime_idle    = msmsdcc_runtime_idle,
	.suspend 	 = msmsdcc_pm_suspend,
	.resume		 = msmsdcc_pm_resume,
	.suspend_noirq	 = msmsdcc_suspend_noirq,
	.prepare 	= msmsdcc_pm_prepare_suspend,
	.complete 	= msmsdcc_pm_complete,
};

static const struct of_device_id msmsdcc_dt_match[] = {
	{.compatible = "qcom,msm-sdcc"},

};
MODULE_DEVICE_TABLE(of, msmsdcc_dt_match);

static struct platform_driver msmsdcc_driver = {
	.probe		= msmsdcc_probe,
	.remove		= msmsdcc_remove,
	.shutdown	= msmsdcc_shutdown,
	.driver		= {
		.name	= "msm_sdcc",
		.pm	= &msmsdcc_dev_pm_ops,
		.of_match_table = msmsdcc_dt_match,
	},
};

static int __init msmsdcc_init(void)
{
#if defined(CONFIG_DEBUG_FS)
	int ret = 0;
	ret = msmsdcc_dbg_init();
	if (ret) {
		pr_err("Failed to create debug fs dir \n");
		return ret;
	}
#endif
	return platform_driver_register(&msmsdcc_driver);
}

static void __exit msmsdcc_exit(void)
{
	platform_driver_unregister(&msmsdcc_driver);

#if defined(CONFIG_DEBUG_FS)
	debugfs_remove(debugfs_file);
	debugfs_remove(debugfs_dir);
#endif
}

module_init(msmsdcc_init);
module_exit(msmsdcc_exit);

MODULE_DESCRIPTION("Qualcomm Multimedia Card Interface driver");
MODULE_LICENSE("GPL");

#if defined(CONFIG_DEBUG_FS)

static int
msmsdcc_dbg_state_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t
msmsdcc_dbg_state_read(struct file *file, char __user *ubuf,
		       size_t count, loff_t *ppos)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *) file->private_data;
	char buf[200];
	int max, i;

	i = 0;
	max = sizeof(buf) - 1;

	i += scnprintf(buf + i, max - i, "STAT: %p %p %p\n", host->curr.mrq,
		       host->curr.cmd, host->curr.data);
	if (host->curr.cmd) {
		struct mmc_command *cmd = host->curr.cmd;

		i += scnprintf(buf + i, max - i, "CMD : %.8x %.8x %.8x\n",
			      cmd->opcode, cmd->arg, cmd->flags);
	}
	if (host->curr.data) {
		struct mmc_data *data = host->curr.data;
		i += scnprintf(buf + i, max - i,
			      "DAT0: %.8x %.8x %.8x %.8x %.8x %.8x\n",
			      data->timeout_ns, data->timeout_clks,
			      data->blksz, data->blocks, data->error,
			      data->flags);
		i += scnprintf(buf + i, max - i, "DAT1: %.8x %.8x %.8x %p\n",
			      host->curr.xfer_size, host->curr.xfer_remain,
			      host->curr.data_xfered, host->dma.sg);
	}

	return simple_read_from_buffer(ubuf, count, ppos, buf, i);
}

static const struct file_operations msmsdcc_dbg_state_ops = {
	.read	= msmsdcc_dbg_state_read,
	.open	= msmsdcc_dbg_state_open,
};

static void msmsdcc_dbg_createhost(struct msmsdcc_host *host)
{
	if (debugfs_dir) {
		debugfs_file = debugfs_create_file(mmc_hostname(host->mmc),
							0644, debugfs_dir, host,
							&msmsdcc_dbg_state_ops);
	}
}

static int __init msmsdcc_dbg_init(void)
{
	int err;

	debugfs_dir = debugfs_create_dir("msmsdcc", 0);
	if (IS_ERR(debugfs_dir)) {
		err = PTR_ERR(debugfs_dir);
		debugfs_dir = NULL;
		return err;
	}

	return 0;
}
#endif
