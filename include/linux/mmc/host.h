/*
 *  linux/include/linux/mmc/host.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Host driver specific definitions.
 */
#ifndef LINUX_MMC_HOST_H
#define LINUX_MMC_HOST_H

#include <linux/leds.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/fault-inject.h>
#include <linux/wakelock.h>

#include <linux/mmc/core.h>
#include <linux/mmc/pm.h>
#include <linux/android_alarm.h>

#define SD_DEBOUNCE_DEBUG	1
struct mmc_ios {
	unsigned int	clock;			
	unsigned short	vdd;


	unsigned char	bus_mode;		

#define MMC_BUSMODE_OPENDRAIN	1
#define MMC_BUSMODE_PUSHPULL	2

	unsigned char	chip_select;		

#define MMC_CS_DONTCARE		0
#define MMC_CS_HIGH		1
#define MMC_CS_LOW		2

	unsigned char	power_mode;		

#define MMC_POWER_OFF		0
#define MMC_POWER_UP		1
#define MMC_POWER_ON		2

	unsigned char	bus_width;		

#define MMC_BUS_WIDTH_1		0
#define MMC_BUS_WIDTH_4		2
#define MMC_BUS_WIDTH_8		3

	unsigned char	timing;			

#define MMC_TIMING_LEGACY	0
#define MMC_TIMING_MMC_HS	1
#define MMC_TIMING_SD_HS	2
#define MMC_TIMING_UHS_SDR12	MMC_TIMING_LEGACY
#define MMC_TIMING_UHS_SDR25	MMC_TIMING_SD_HS
#define MMC_TIMING_UHS_SDR50	3
#define MMC_TIMING_UHS_SDR104	4
#define MMC_TIMING_UHS_DDR50	5
#define MMC_TIMING_MMC_HS200	6

	unsigned char	ddr;			

#define MMC_SDR_MODE		0
#define MMC_1_2V_DDR_MODE	1
#define MMC_1_8V_DDR_MODE	2
#define MMC_1_2V_SDR_MODE	3
#define MMC_1_8V_SDR_MODE	4

	unsigned char	signal_voltage;		

#define MMC_SIGNAL_VOLTAGE_330	0
#define MMC_SIGNAL_VOLTAGE_180	1
#define MMC_SIGNAL_VOLTAGE_120	2

	unsigned char	drv_type;		

#define MMC_SET_DRIVER_TYPE_B	0
#define MMC_SET_DRIVER_TYPE_A	1
#define MMC_SET_DRIVER_TYPE_C	2
#define MMC_SET_DRIVER_TYPE_D	3
};

struct mmc_host_ops {
	int (*enable)(struct mmc_host *host);
	int (*disable)(struct mmc_host *host);
	void	(*post_req)(struct mmc_host *host, struct mmc_request *req,
			    int err);
	void	(*pre_req)(struct mmc_host *host, struct mmc_request *req,
			   bool is_first_req);
	void	(*request)(struct mmc_host *host, struct mmc_request *req);
	void	(*set_ios)(struct mmc_host *host, struct mmc_ios *ios);
	int	(*get_ro)(struct mmc_host *host);
	int	(*get_cd)(struct mmc_host *host);

	void	(*enable_sdio_irq)(struct mmc_host *host, int enable);

	
	void	(*init_card)(struct mmc_host *host, struct mmc_card *card);

	int	(*start_signal_voltage_switch)(struct mmc_host *host, struct mmc_ios *ios);

	
	int	(*execute_tuning)(struct mmc_host *host, u32 opcode);
	void	(*enable_preset_value)(struct mmc_host *host, bool enable);
	int	(*select_drive_strength)(unsigned int max_dtr, int host_drv, int card_drv);
	void	(*hw_reset)(struct mmc_host *host);
};

struct mmc_card;
struct device;

struct background_ops_timer {
	u64	bkops_start;
	int	need_bkops;
	struct alarm bkops_alarm_timer;
};

struct mmc_async_req {
	
	struct mmc_request	*mrq;
	ktime_t rq_stime;
	int (*err_check) (struct mmc_card *, struct mmc_async_req *);
};

struct mmc_hotplug {
	unsigned int irq;
	void *handler_priv;
};

struct mmc_host {
	struct device		*parent;
	struct device		class_dev;
	int			index;
	const struct mmc_host_ops *ops;
	unsigned int		f_min;
	unsigned int		f_max;
	unsigned int		f_init;
	u32			ocr_avail;
	u32			ocr_avail_sdio;	
	u32			ocr_avail_sd;	
	u32			ocr_avail_mmc;	
	struct notifier_block	pm_notify;
	int					tp_enable;
	int					burst_mode;
	struct background_ops_timer bkops_timer;
	int					bkops_count;

#define MMC_VDD_165_195		0x00000080	
#define MMC_VDD_20_21		0x00000100	
#define MMC_VDD_21_22		0x00000200	
#define MMC_VDD_22_23		0x00000400	
#define MMC_VDD_23_24		0x00000800	
#define MMC_VDD_24_25		0x00001000	
#define MMC_VDD_25_26		0x00002000	
#define MMC_VDD_26_27		0x00004000	
#define MMC_VDD_27_28		0x00008000	
#define MMC_VDD_28_29		0x00010000	
#define MMC_VDD_29_30		0x00020000	
#define MMC_VDD_30_31		0x00040000	
#define MMC_VDD_31_32		0x00080000	
#define MMC_VDD_32_33		0x00100000	
#define MMC_VDD_33_34		0x00200000	
#define MMC_VDD_34_35		0x00400000	
#define MMC_VDD_35_36		0x00800000	

	unsigned long		caps;		

#define MMC_CAP_4_BIT_DATA	(1 << 0)	
#define MMC_CAP_MMC_HIGHSPEED	(1 << 1)	
#define MMC_CAP_SD_HIGHSPEED	(1 << 2)	
#define MMC_CAP_SDIO_IRQ	(1 << 3)	
#define MMC_CAP_SPI		(1 << 4)	
#define MMC_CAP_NEEDS_POLL	(1 << 5)	
#define MMC_CAP_8_BIT_DATA	(1 << 6)	

#define MMC_CAP_NONREMOVABLE	(1 << 8)	
#define MMC_CAP_WAIT_WHILE_BUSY	(1 << 9)	
#define MMC_CAP_ERASE		(1 << 10)	
#define MMC_CAP_1_8V_DDR	(1 << 11)	
						
#define MMC_CAP_1_2V_DDR	(1 << 12)	
						
#define MMC_CAP_POWER_OFF_CARD	(1 << 13)	
#define MMC_CAP_BUS_WIDTH_TEST	(1 << 14)	
#define MMC_CAP_UHS_SDR12	(1 << 15)	
#define MMC_CAP_UHS_SDR25	(1 << 16)	
#define MMC_CAP_UHS_SDR50	(1 << 17)	
#define MMC_CAP_UHS_SDR104	(1 << 18)	
#define MMC_CAP_UHS_DDR50	(1 << 19)	
#define MMC_CAP_SET_XPC_330	(1 << 20)	
#define MMC_CAP_SET_XPC_300	(1 << 21)	
#define MMC_CAP_SET_XPC_180	(1 << 22)	
#define MMC_CAP_DRIVER_TYPE_A	(1 << 23)	
#define MMC_CAP_DRIVER_TYPE_C	(1 << 24)	
#define MMC_CAP_DRIVER_TYPE_D	(1 << 25)	
#define MMC_CAP_MAX_CURRENT_200	(1 << 26)	
#define MMC_CAP_MAX_CURRENT_400	(1 << 27)	
#define MMC_CAP_MAX_CURRENT_600	(1 << 28)	
#define MMC_CAP_MAX_CURRENT_800	(1 << 29)	
#define MMC_CAP_CMD23		(1 << 30)	
#define MMC_CAP_HW_RESET	(1 << 31)	

	unsigned int		caps2;		

#define MMC_CAP2_BOOTPART_NOACC	(1 << 0)	
#define MMC_CAP2_CACHE_CTRL	(1 << 1)	
#define MMC_CAP2_POWEROFF_NOTIFY (1 << 2)	
#define MMC_CAP2_NO_MULTI_READ	(1 << 3)	
#define MMC_CAP2_NO_SLEEP_CMD	(1 << 4)	
#define MMC_CAP2_HS200_1_8V_SDR	(1 << 5)        
#define MMC_CAP2_HS200_1_2V_SDR	(1 << 6)        
#define MMC_CAP2_HS200		(MMC_CAP2_HS200_1_8V_SDR | \
				 MMC_CAP2_HS200_1_2V_SDR)
#define MMC_CAP2_BROKEN_VOLTAGE	(1 << 7)	
#define MMC_CAP2_DETECT_ON_ERR	(1 << 8)	
#define MMC_CAP2_HC_ERASE_SZ	(1 << 9)	
#define MMC_CAP2_PACKED_RD	(1 << 10)	
#define MMC_CAP2_PACKED_WR	(1 << 11)	
#define MMC_CAP2_PACKED_CMD	(MMC_CAP2_PACKED_RD | \
				 MMC_CAP2_PACKED_WR) 
#define MMC_CAP2_PACKED_WR_CONTROL (1 << 12) 
#define MMC_CAP2_SANITIZE	(1 << 13)		
#define MMC_CAP2_BKOPS		    (1 << 14)	
#define MMC_CAP2_INIT_BKOPS	    (1 << 15)	
#define MMC_CAP2_POWER_OFF_VCCQ_DURING_SUSPEND	(1 << 16)

	mmc_pm_flag_t		pm_caps;	

	int			clk_requests;	
	unsigned int		clk_delay;	
	bool			clk_gated;	
	struct delayed_work	clk_gate_work; 
	unsigned int		clk_old;	
	spinlock_t		clk_lock;	
	struct mutex		clk_gate_mutex;	
	struct device_attribute clkgate_delay_attr;
	unsigned long           clkgate_delay;

	
	unsigned int		max_seg_size;	
	unsigned short		max_segs;	
	unsigned short		unused;
	unsigned int		max_req_size;	
	unsigned int		max_blk_size;	
	unsigned int		max_blk_count;	
	unsigned int		max_discard_to;	

	
	spinlock_t		lock;		

	struct mmc_ios		ios;		
	u32			ocr;		

	
	unsigned int		use_spi_crc:1;
	unsigned int		claimed:1;	
	unsigned int		bus_dead:1;	
#ifdef CONFIG_MMC_DEBUG
	unsigned int		removed:1;	
#endif

	int			rescan_disable;	
	int			bkops_started;
	int 		hpi_issued;
	int 		bkops_trigger;
	int 		bkops_alarm_set;
	struct mmc_card		*card;		

	wait_queue_head_t	wq;
	struct task_struct	*claimer;	
	struct task_struct	*suspend_task;
	int			claim_cnt;	

	struct delayed_work	detect;
	struct delayed_work	remove;
	struct wake_lock	detect_wake_lock;
	int			detect_change;	
	struct mmc_hotplug	hotplug;

	const struct mmc_bus_ops *bus_ops;	
	unsigned int		bus_refs;	

	unsigned int		bus_resume_flags;
#define MMC_BUSRESUME_MANUAL_RESUME	(1 << 0)
#define MMC_BUSRESUME_NEEDS_RESUME	(1 << 1)
#define MMC_BUSRESUME_FAILS_RESUME	(1 << 2)

	unsigned int		sdio_irqs;
	struct task_struct	*sdio_irq_thread;
	bool			sdio_irq_pending;
	atomic_t		sdio_irq_thread_abort;

	mmc_pm_flag_t		pm_flags;	

#ifdef CONFIG_LEDS_TRIGGERS
	struct led_trigger	*led;		
#endif

#ifdef CONFIG_REGULATOR
	bool			regulator_enabled; 
#endif

	struct dentry		*debugfs_root;

	struct mmc_async_req	*areq;		

#ifdef CONFIG_FAIL_MMC_REQUEST
	struct fault_attr	fail_mmc_request;
#endif

	unsigned int		actual_clock;	

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	struct {
		struct sdio_cis			*cis;
		struct sdio_cccr		*cccr;
		struct sdio_embedded_func	*funcs;
		int				num_funcs;
	} embedded_sdio_data;
#endif

#ifdef CONFIG_MMC_PERF_PROFILING
	struct {

		unsigned long rbytes_drv;  
		unsigned long wbytes_drv;  
		ktime_t rtime_drv;	   
		ktime_t wtime_drv;	   
		ktime_t start;
	} perf;
#endif
	bool perf_enable;
	ktime_t rq_start;

	struct mmc_ios saved_ios;
	unsigned long		private[0] ____cacheline_aligned;
};

extern struct mmc_host *mmc_alloc_host(int extra, struct device *);
extern int mmc_add_host(struct mmc_host *);
extern void mmc_remove_host(struct mmc_host *);
extern void mmc_free_host(struct mmc_host *);

#ifdef CONFIG_MMC_EMBEDDED_SDIO
extern void mmc_set_embedded_sdio_data(struct mmc_host *host,
				       struct sdio_cis *cis,
				       struct sdio_cccr *cccr,
				       struct sdio_embedded_func *funcs,
				       int num_funcs);
#endif

static inline void *mmc_priv(struct mmc_host *host)
{
	return (void *)host->private;
}

#define mmc_host_is_spi(host)	((host)->caps & MMC_CAP_SPI)

#define mmc_dev(x)	((x)->parent)
#define mmc_classdev(x)	(&(x)->class_dev)
#define mmc_hostname(x)	(dev_name(&(x)->class_dev))
#define mmc_bus_needs_resume(host) ((host)->bus_resume_flags & MMC_BUSRESUME_NEEDS_RESUME)
#define mmc_bus_manual_resume(host) ((host)->bus_resume_flags & MMC_BUSRESUME_MANUAL_RESUME)
#define mmc_bus_fails_resume(host)  \
       ((host)->bus_resume_flags & MMC_BUSRESUME_FAILS_RESUME)

static inline void mmc_set_bus_resume_policy(struct mmc_host *host, int manual)
{
	if (manual)
		host->bus_resume_flags |= MMC_BUSRESUME_MANUAL_RESUME;
	else
		host->bus_resume_flags &= ~MMC_BUSRESUME_MANUAL_RESUME;
}

static inline void mmc_init_bus_resume_flags(struct mmc_host *host)
{
       host->bus_resume_flags = 0;
}

extern int mmc_resume_bus(struct mmc_host *host);

extern int mmc_suspend_host(struct mmc_host *);
extern int mmc_resume_host(struct mmc_host *);

#ifdef CONFIG_MMC_CPRM_SUPPORT
extern int mmc_read_sd_status(struct mmc_card *card);
#endif
extern int mmc_bkops_resume_task(struct mmc_host *);
extern int mmc_bkops_suspend_task(struct mmc_host *);
extern int mmc_power_save_host(struct mmc_host *host);
extern int mmc_power_restore_host(struct mmc_host *host);

extern void mmc_detect_change(struct mmc_host *, unsigned long delay);
extern void mmc_request_done(struct mmc_host *, struct mmc_request *);

extern int mmc_cache_ctrl(struct mmc_host *, u8);
int mmc_card_support_bkops(struct mmc_card *card);
int mmc_card_start_bkops(struct mmc_host *host);
static inline void mmc_signal_sdio_irq(struct mmc_host *host)
{
	host->ops->enable_sdio_irq(host, 0);
	host->sdio_irq_pending = true;
	wake_up_process(host->sdio_irq_thread);
}

struct regulator;

#ifdef CONFIG_REGULATOR
int mmc_regulator_get_ocrmask(struct regulator *supply);
int mmc_regulator_set_ocr(struct mmc_host *mmc,
			struct regulator *supply,
			unsigned short vdd_bit);
#else
static inline int mmc_regulator_get_ocrmask(struct regulator *supply)
{
	return 0;
}

static inline int mmc_regulator_set_ocr(struct mmc_host *mmc,
				 struct regulator *supply,
				 unsigned short vdd_bit)
{
	return 0;
}
#endif

int mmc_card_awake(struct mmc_host *host);
int mmc_card_sleep(struct mmc_host *host);
int mmc_card_can_sleep(struct mmc_host *host);

int mmc_host_enable(struct mmc_host *host);
int mmc_host_disable(struct mmc_host *host);
int mmc_host_lazy_disable(struct mmc_host *host);
int mmc_pm_notify(struct notifier_block *notify_block, unsigned long, void *);

extern bool mmc_assume_removable;

static inline int mmc_card_is_removable(struct mmc_host *host)
{
	return !(host->caps & MMC_CAP_NONREMOVABLE) && mmc_assume_removable;
}

static inline int mmc_card_keep_power(struct mmc_host *host)
{
	return host->pm_flags & MMC_PM_KEEP_POWER;
}

static inline int mmc_card_wake_sdio_irq(struct mmc_host *host)
{
	return host->pm_flags & MMC_PM_WAKE_SDIO_IRQ;
}

static inline int mmc_host_cmd23(struct mmc_host *host)
{
	return host->caps & MMC_CAP_CMD23;
}

static inline int mmc_boot_partition_access(struct mmc_host *host)
{
	return !(host->caps2 & MMC_CAP2_BOOTPART_NOACC);
}

#ifdef CONFIG_MMC_CLKGATE
void mmc_host_clk_hold(struct mmc_host *host);
void mmc_host_clk_release(struct mmc_host *host);
unsigned int mmc_host_clk_rate(struct mmc_host *host);

#else
static inline void mmc_host_clk_hold(struct mmc_host *host)
{
}

static inline void mmc_host_clk_release(struct mmc_host *host)
{
}

static inline unsigned int mmc_host_clk_rate(struct mmc_host *host)
{
	return host->ios.clock;
}
#endif
#endif 
