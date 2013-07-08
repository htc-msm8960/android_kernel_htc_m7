/* Copyright (c) 2008-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/ratelimit.h>
#include <linux/workqueue.h>
#include <linux/pm_runtime.h>
#include <linux/diagchar.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/of.h>
#include <linux/kmemleak.h>
#ifdef CONFIG_DIAG_OVER_USB
#include <mach/usbdiag.h>
#endif
#include <mach/msm_smd.h>
#include <mach/socinfo.h>
#include <mach/restart.h>
#include "diagmem.h"
#include "diagchar.h"
#include "diagfwd.h"
#include "diagfwd_cntl.h"
#include "diagchar_hdlc.h"
#ifdef CONFIG_DIAG_SDIO_PIPE
#include "diagfwd_sdio.h"
#endif
#include "diag_dci.h"
#include "diag_masks.h"
#include "diagfwd_bridge.h"

#define MODE_CMD		41
#define RESET_ID		2

int diag_debug_buf_idx;
unsigned char diag_debug_buf[1024];
static unsigned int buf_tbl_size = 8; 
int sdio_diag_initialized;
int smd_diag_initialized;
#if DIAG_XPST
static int diag_smd_function_mode;
#endif
struct diag_master_table entry;
smd_channel_t *ch_temp = NULL, *chlpass_temp = NULL, *ch_wcnss_temp = NULL;
struct diag_send_desc_type send = { NULL, NULL, DIAG_STATE_START, 0 };
struct diag_hdlc_dest_type enc = { NULL, NULL, 0 };

void encode_rsp_and_send(int buf_length)
{
	send.state = DIAG_STATE_START;
	send.pkt = driver->apps_rsp_buf;
	send.last = (void *)(driver->apps_rsp_buf + buf_length);
	send.terminate = 1;
	if (!driver->in_busy_1) {
		enc.dest = driver->buf_in_1;
		enc.dest_last = (void *)(driver->buf_in_1 + APPS_BUF_SIZE - 1);
		diag_hdlc_encode(&send, &enc);
		driver->write_ptr_1->buf = driver->buf_in_1;
		driver->write_ptr_1->length = (int)(enc.dest -
						(void *)(driver->buf_in_1));
		driver->in_busy_1 = 1;
		diag_device_write(driver->buf_in_1, MODEM_DATA,
					driver->write_ptr_1);
		memset(driver->apps_rsp_buf, '\0', APPS_BUF_SIZE);
	}
}

#ifdef CONFIG_OF
static int has_device_tree(void)
{
	struct device_node *node;

	node = of_find_node_by_path("/");
	if (node) {
		of_node_put(node);
		return 1;
	}
	return 0;
}
#else
static int has_device_tree(void)
{
	return 0;
}
#endif

int chk_config_get_id(void)
{
	
	if (machine_is_msm8x60_fusion() || machine_is_msm8x60_fusn_ffa())
		return 0;

	if (driver->use_device_tree) {
		if (machine_is_msm8974())
			return MSM8974_TOOLS_ID;
		else
			return 0;
	} else {
		switch (socinfo_get_msm_cpu()) {
		case MSM_CPU_8X60:
			return APQ8060_TOOLS_ID;
		case MSM_CPU_8960:
		case MSM_CPU_8960AB:
			return AO8960_TOOLS_ID;
		case MSM_CPU_8064:
		case MSM_CPU_8064AB:
			return APQ8064_TOOLS_ID;
		case MSM_CPU_8930:
		case MSM_CPU_8930AA:
		#if 0
		case MSM_CPU_8930AB:
		#endif
			return MSM8930_TOOLS_ID;
		case MSM_CPU_8974:
			return MSM8974_TOOLS_ID;
		case MSM_CPU_8625:
			return MSM8625_TOOLS_ID;
		default:
			return 0;
		}
	}
}

int chk_apps_only(void)
{
	if (driver->use_device_tree)
		return 1;

	switch (socinfo_get_msm_cpu()) {
	case MSM_CPU_8960:
	case MSM_CPU_8960AB:
	case MSM_CPU_8064:
	case MSM_CPU_8064AB:
	case MSM_CPU_8930:
	case MSM_CPU_8930AA:
	#if 0
	case MSM_CPU_8930AB:
	#endif
	case MSM_CPU_8627:
	case MSM_CPU_9615:
	case MSM_CPU_8974:
		return 1;
	default:
		return 0;
	}
}

int chk_apps_master(void)
{
	if (driver->use_device_tree)
		return 1;
	#if 0
	else if (soc_class_is_msm8960() || soc_class_is_msm8930() ||
		 soc_class_is_apq8064() || cpu_is_msm9615())
	#else
	else if (cpu_is_msm8960() || cpu_is_msm8930() || cpu_is_msm8930aa() ||
			cpu_is_msm9615() || cpu_is_apq8064() || cpu_is_msm8627() ||
			cpu_is_msm8960ab() || cpu_is_apq8064ab())
	#endif
		return 1;
	else
		return 0;
}

int chk_polling_response(void)
{
	if (!(driver->polling_reg_flag) && chk_apps_master())
		return 1;
	else if (!(driver->ch) && !(chk_apps_master()))
		return 1;
	else
		return 0;
}

void chk_logging_wakeup(void)
{
	int i;

	
	for (i = 0; i < driver->num_clients; i++)
		if (driver->client_map[i].pid ==
			driver->logging_process_id)
			break;

	if (i < driver->num_clients) {
		if ((driver->data_ready[i] & USERMODE_DIAGFWD) == 0) {
			driver->data_ready[i] |= USERMODE_DIAGFWD;
			pr_debug("diag: Force wakeup of logging process\n");
			wake_up_interruptible(&driver->wait_q);
		}
	}
}

void __diag_smd_send_req(void)
{
	void *buf = NULL, *temp_buf = NULL;
	int total_recd = 0, r = 0, pkt_len, *in_busy_ptr = NULL;
	int loop_count = 0, retry = 0;
	struct diag_request *write_ptr_modem = NULL;

#if DIAG_XPST && !defined(CONFIG_DIAGFWD_BRIDGE_CODE)
	int type;
#endif

	if (!driver->in_busy_1) {
		buf = driver->buf_in_1;
		write_ptr_modem = driver->write_ptr_1;
		in_busy_ptr = &(driver->in_busy_1);
	} else if (!driver->in_busy_2) {
		buf = driver->buf_in_2;
		write_ptr_modem = driver->write_ptr_2;
		in_busy_ptr = &(driver->in_busy_2);
	}

drop:
	if (driver->ch && buf) {
		temp_buf = buf;
		pkt_len = smd_cur_packet_size(driver->ch);

		while (pkt_len && (pkt_len != total_recd)) {
			loop_count++;
			r = smd_read_avail(driver->ch);
			pr_debug("diag: In %s, received pkt %d %d\n",
				__func__, r, total_recd);
			if (!r) {
				
				wait_event(driver->smd_wait_q,
					((driver->ch == 0) ||
					smd_read_avail(driver->ch)));
				
				if (driver->ch) {
					pr_debug("diag: In %s, return from wait_event\n",
						__func__);
					continue;
				} else {
					pr_debug("diag: In %s, return from wait_event ch closed\n",
						__func__);
					return;
				}
			}
			total_recd += r;
			if (total_recd > IN_BUF_SIZE) {
				if (total_recd < MAX_IN_BUF_SIZE) {
					pr_err("diag: In %s, SMD sending in packets up to %d bytes\n",
						__func__, total_recd);
					buf = krealloc(buf, total_recd,
							GFP_KERNEL);
				} else {
					pr_err("diag: In %s, SMD sending in packets more than %d bytes\n",
						__func__, MAX_IN_BUF_SIZE);
					return;
				}
			}
			if (pkt_len < r) {
				pr_err("diag: In %s, SMD sending incorrect pkt\n",
					__func__);
				return;
			}
			if (pkt_len > r)
				pr_debug("diag: In %s, SMD sending partial pkt %d %d %d %d\n",
					__func__, pkt_len, r, total_recd,
					loop_count);
			
			smd_read(driver->ch, temp_buf, r);
			temp_buf += r;
		}

		if (buf && driver->qxdm2sd_drop && (driver->logging_mode == USB_MODE)
				&& *((unsigned char *)buf) != 0xc8) {
			DIAG_DBUG("%s:Drop the diag payload :%d\n", __func__, retry);
			DIAGFWD_7K_RAWDATA(buf, "modem", DIAG_DBG_DROP);
			driver->in_busy_1 = 0;
			driver->in_busy_2 = 0;
			total_recd = 0;
			
			msleep(10);
			r = smd_read_avail(driver->ch);
			if (++retry > 20) {
				driver->qxdm2sd_drop = 0;
				return;
			}
			if (r)
				goto drop;
			else {
				DIAG_INFO("Nothing pending in SMD buffer\n");
				driver->qxdm2sd_drop = 0;
				return;
			}

		}

		if (total_recd > 0) {
			if (!buf)
				pr_err("diag: Out of diagmem for Modem\n");
			else {
				DIAGFWD_7K_RAWDATA(buf, "modem", DIAG_DBG_READ);
#if DIAG_XPST && !defined(CONFIG_DIAGFWD_BRIDGE_CODE)
				type = checkcmd_modem_epst(buf);
				if (type) {
					modem_to_userspace(buf, total_recd, type, 0);
					return;
				}
				if (driver->qxdmusb_drop && driver->logging_mode == USB_MODE)
					return;
#endif
				APPEND_DEBUG('j');
				write_ptr_modem->length = total_recd;
				*in_busy_ptr = 1;
				diag_device_write(buf, MODEM_DATA,
							 write_ptr_modem);
			}
		}
	} else if (driver->ch && !buf &&
		(driver->logging_mode == MEMORY_DEVICE_MODE)) {
		chk_logging_wakeup();
	}
}

int diag_device_write(void *buf, int proc_num, struct diag_request *write_ptr)
{
	int i, err = 0;
	int foundIndex = -1;

	pr_debug("proc_num: %d, logging_mode: %d\n",
		proc_num, driver->logging_mode);
	if (driver->logging_mode == MEMORY_DEVICE_MODE) {
		if (proc_num == APPS_DATA) {
			for (i = 0; i < driver->poolsize_write_struct; i++)
				if (driver->buf_tbl[i].length == 0) {
					driver->buf_tbl[i].buf = buf;
					driver->buf_tbl[i].length =
								 driver->used;
					foundIndex = i;
#ifdef DIAG_DEBUG
					pr_debug("diag: ENQUEUE buf ptr"
						   " and length is %x , %d\n",
						   (unsigned int)(driver->buf_
				tbl[i].buf), driver->buf_tbl[i].length);
#endif
					break;
				}
		}

#if defined(CONFIG_DIAGFWD_BRIDGE_CODE) && !defined(CONFIG_DIAG_HSIC_ON_LEGACY)
		else if (proc_num == HSIC_DATA) {
			unsigned long flags;

			spin_lock_irqsave(&driver->hsic_spinlock, flags);
			for (i = 0; i < driver->poolsize_hsic_write; i++) {
				if (driver->hsic_buf_tbl[i].length == 0) {
					driver->hsic_buf_tbl[i].buf = buf;
					driver->hsic_buf_tbl[i].length =
						diag_bridge[HSIC].write_len;
					driver->num_hsic_buf_tbl_entries++;
					foundIndex = i;
					break;
				}
			}
			spin_unlock_irqrestore(&driver->hsic_spinlock, flags);
			if (foundIndex == -1)
				err = -1;
			else
				pr_debug("diag: ENQUEUE HSIC buf ptr and length is %x , %d\n",
					(unsigned int)buf,
					 diag_bridge[HSIC].write_len);
			for (i = 0; i < driver->num_mdmclients; i++)
				if (driver->mdmclient_map[i].pid ==
						driver->mdm_logging_process_id)
					break;

			if (i < driver->num_mdmclients) {
				wake_lock_timeout(&driver->wake_lock, HZ / 2);
				driver->mdmdata_ready[i] |= USERMODE_DIAGFWD;
				pr_debug("diag_mdm: wake up logging process\n");
				wake_up_interruptible(&driver->mdmwait_q);

				return err;
			} else {
				if (foundIndex != -1) {
					pr_err_ratelimited("diag: In %s, cannot find logging process\n", __func__);
					i = foundIndex;
					driver->hsic_buf_tbl[i].buf = NULL;
					driver->hsic_buf_tbl[i].length = 0;
					driver->num_hsic_buf_tbl_entries--;
				}
				return -EINVAL;
			}
		} else if (proc_num == SMUX_DATA) {
			for (i = 0; i < driver->num_qscclients; i++)
				if (driver->qscclient_map[i].pid ==
						driver->qsc_logging_process_id)
					break;

			if (i < driver->num_qscclients) {
				wake_lock_timeout(&driver->wake_lock, HZ / 2);
				driver->qscdata_ready[i] |= USERMODE_DIAGFWD;
				pr_debug("diag_qsc: wake up logging process\n");
				wake_up_interruptible(&driver->qscwait_q);

				return err;
			} else
				return -EINVAL;
		}
#endif
		for (i = 0; i < driver->num_clients; i++)
			if (driver->client_map[i].pid ==
						 driver->logging_process_id)
				break;
		if (i < driver->num_clients) {
			driver->data_ready[i] |= USERMODE_DIAGFWD;
			pr_debug("diag: wake up logging process\n");
			wake_up_interruptible(&driver->wait_q);
		} else {
			if (foundIndex != -1) {
				pr_err_ratelimited("diag: In %s, cannot find logging process\n", __func__);
				i = foundIndex;
				driver->buf_tbl[i].buf = buf;
				driver->buf_tbl[i].length = 0;
			}
			return -EINVAL;
		}
	} else if (driver->logging_mode == NO_LOGGING_MODE) {
		if (proc_num == MODEM_DATA) {
			driver->in_busy_1 = 0;
			driver->in_busy_2 = 0;
			queue_work(driver->diag_wq, &(driver->
							diag_read_smd_work));
		} else if (proc_num == LPASS_DATA) {
			driver->in_busy_lpass_1 = 0;
			driver->in_busy_lpass_2 = 0;
			queue_work(driver->diag_wq, &(driver->
						diag_read_smd_lpass_work));
		}  else if (proc_num == WCNSS_DATA) {
			driver->in_busy_wcnss_1 = 0;
			driver->in_busy_wcnss_2 = 0;
			queue_work(driver->diag_wq, &(driver->
				diag_read_smd_wcnss_work));
		}
#ifdef CONFIG_DIAG_SDIO_PIPE
		else if (proc_num == SDIO_DATA) {
			driver->in_busy_sdio = 0;
			queue_work(driver->diag_sdio_wq,
				&(driver->diag_read_sdio_work));
		}
#endif
#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
		else if (proc_num == HSIC_DATA) {
			if (driver->hsic_ch)
				queue_work(diag_bridge[HSIC].wq,
					&(driver->diag_read_hsic_work));
		}
#endif
		err = -1;
	}
#ifdef CONFIG_DIAG_OVER_USB
	else if (driver->logging_mode == USB_MODE) {
		if (proc_num == APPS_DATA) {
			driver->write_ptr_svc = (struct diag_request *)
			(diagmem_alloc(driver, sizeof(struct diag_request),
				 POOL_TYPE_WRITE_STRUCT));
			if (driver->write_ptr_svc) {
				driver->write_ptr_svc->length = driver->used;
				driver->write_ptr_svc->buf = buf;
				err = usb_diag_write(driver->legacy_ch,
						driver->write_ptr_svc);
			} else
				err = -1;
		} else if (proc_num == MODEM_DATA) {
			write_ptr->buf = buf;
#ifdef DIAG_DEBUG
			printk(KERN_INFO "writing data to USB,"
				"pkt length %d\n", write_ptr->length);
			print_hex_dump(KERN_DEBUG, "Written Packet Data to"
					   " USB: ", 16, 1, DUMP_PREFIX_ADDRESS,
					    buf, write_ptr->length, 1);
#endif 
			err = usb_diag_write(driver->legacy_ch, write_ptr);
		} else if (proc_num == LPASS_DATA) {
			write_ptr->buf = buf;
			err = usb_diag_write(driver->legacy_ch, write_ptr);
		} else if (proc_num == WCNSS_DATA) {
			write_ptr->buf = buf;
			err = usb_diag_write(driver->legacy_ch, write_ptr);
		}
#ifdef CONFIG_DIAG_SDIO_PIPE
		else if (proc_num == SDIO_DATA) {
			if (machine_is_msm8x60_fusion() ||
					 machine_is_msm8x60_fusn_ffa()) {
				write_ptr->buf = buf;
				err = usb_diag_write(driver->mdm_ch, write_ptr);
			} else
				pr_err("diag: Incorrect sdio data "
						"while USB write\n");
		}
#endif
#ifdef CONFIG_DIAGFWD_BRIDGE_CODE
		else if (proc_num == HSIC_DATA) {
			if (driver->hsic_device_enabled) {
				struct diag_request *write_ptr_mdm;
				write_ptr_mdm = (struct diag_request *)
						diagmem_alloc(driver,
						sizeof(struct diag_request),
							POOL_TYPE_HSIC_WRITE);
				if (write_ptr_mdm) {
					write_ptr_mdm->buf = buf;
					write_ptr_mdm->length =
					   diag_bridge[HSIC].write_len;
					write_ptr_mdm->context = (void *)HSIC;
					err = usb_diag_write(
					diag_bridge[HSIC].ch, write_ptr_mdm);
					
					if (err) {
						diagmem_free(driver,
							write_ptr_mdm,
							POOL_TYPE_HSIC_WRITE);
						pr_err_ratelimited("diag: HSIC write failure, err: %d\n",
							err);
					}
				} else {
					pr_err("diag: allocate write fail\n");
					err = -1;
				}
			} else {
				pr_err("diag: Incorrect HSIC data "
						"while USB write\n");
				err = -1;
			}
		} else if (proc_num == SMUX_DATA) {
				write_ptr->buf = buf;
				write_ptr->context = (void *)SMUX;
				pr_debug("diag: writing SMUX data\n");
				err = usb_diag_write(diag_bridge[SMUX].ch,
								 write_ptr);
		}
#endif
		APPEND_DEBUG('d');
	}
#endif 
    return err;
}

void __diag_smd_wcnss_send_req(void)
{
	void *buf = NULL, *temp_buf = NULL;
	int total_recd = 0, r = 0, pkt_len, *in_busy_wcnss_ptr = NULL;
	struct diag_request *write_ptr_wcnss = NULL;
	int loop_count = 0;

	if (!driver->in_busy_wcnss_1) {
		buf = driver->buf_in_wcnss_1;
		write_ptr_wcnss = driver->write_ptr_wcnss_1;
		in_busy_wcnss_ptr = &(driver->in_busy_wcnss_1);
	} else if (!driver->in_busy_wcnss_2) {
		buf = driver->buf_in_wcnss_2;
		write_ptr_wcnss = driver->write_ptr_wcnss_2;
		in_busy_wcnss_ptr = &(driver->in_busy_wcnss_2);
	}

	if (driver->ch_wcnss && buf) {
		temp_buf = buf;
		pkt_len = smd_cur_packet_size(driver->ch_wcnss);

		while (pkt_len && (pkt_len != total_recd)) {
			loop_count++;
			r = smd_read_avail(driver->ch_wcnss);
			pr_debug("diag: In %s, received pkt %d %d\n",
				__func__, r, total_recd);
			if (!r) {
				
				wait_event(driver->smd_wait_q,
					((driver->ch_wcnss == 0) ||
					smd_read_avail(driver->ch_wcnss)));
				
				if (driver->ch_wcnss) {
					pr_debug("diag: In %s, return from wait_event\n",
						__func__);
					continue;
				} else {
					pr_debug("diag: In %s, return from wait_event ch_wcnss closed\n",
						__func__);
					return;
				}
			}
			total_recd += r;
			if (total_recd > IN_BUF_SIZE) {
				if (total_recd < MAX_IN_BUF_SIZE) {
					pr_err("diag: In %s, SMD sending in packets up to %d bytes\n",
						__func__, total_recd);
					buf = krealloc(buf, total_recd,
								 GFP_KERNEL);
				} else {
					pr_err("diag: In %s, SMD sending in packets more than %d bytes\n",
						__func__, MAX_IN_BUF_SIZE);
					return;
				}
			}
			if (pkt_len < r) {
				pr_err("diag: In %s, SMD sending incorrect pkt\n",
					__func__);
				return;
			}
			if (pkt_len > r) {
				pr_debug("diag: In %s, SMD sending partial pkt %d %d %d %d\n",
					__func__, pkt_len, r, total_recd,
					loop_count);
			}
			
			smd_read(driver->ch_wcnss, temp_buf, r);
			temp_buf += r;
		}

		if (total_recd > 0) {
			if (!buf) {
				pr_err("diag: Out of diagmem for wcnss\n");
			} else {
				APPEND_DEBUG('j');
				write_ptr_wcnss->length = total_recd;
				*in_busy_wcnss_ptr = 1;
				diag_device_write(buf, WCNSS_DATA,
					 write_ptr_wcnss);
			}
		}
	} else if (driver->ch_wcnss && !buf &&
		(driver->logging_mode == MEMORY_DEVICE_MODE)) {
		chk_logging_wakeup();
	}
}

void __diag_smd_lpass_send_req(void)
{
	void *buf = NULL, *temp_buf = NULL;
	int total_recd = 0, r = 0, pkt_len, *in_busy_lpass_ptr = NULL;
	struct diag_request *write_ptr_lpass = NULL;
	int loop_count = 0;

	if (!driver->in_busy_lpass_1) {
		buf = driver->buf_in_lpass_1;
		write_ptr_lpass = driver->write_ptr_lpass_1;
		in_busy_lpass_ptr = &(driver->in_busy_lpass_1);
	} else if (!driver->in_busy_lpass_2) {
		buf = driver->buf_in_lpass_2;
		write_ptr_lpass = driver->write_ptr_lpass_2;
		in_busy_lpass_ptr = &(driver->in_busy_lpass_2);
	}

	if (driver->chlpass && buf) {
		temp_buf = buf;
		pkt_len = smd_cur_packet_size(driver->chlpass);

		while (pkt_len && (pkt_len != total_recd)) {
			loop_count++;
			r = smd_read_avail(driver->chlpass);
			pr_debug("diag: In %s, received pkt %d %d\n",
				__func__, r, total_recd);
			if (!r) {
				
				wait_event(driver->smd_wait_q,
					((driver->chlpass == 0) ||
					smd_read_avail(driver->chlpass)));
				
				if (driver->chlpass) {
					pr_debug("diag: In %s, return from wait_event\n",
						__func__);
					continue;
				} else {
					pr_debug("diag: In %s, return from wait_event chlpass closed\n",
						__func__);
					return;
				}
			}
			total_recd += r;
			if (total_recd > IN_BUF_SIZE) {
				if (total_recd < MAX_IN_BUF_SIZE) {
					pr_err("diag: In %s, SMD sending in packets up to %d bytes\n",
						__func__, total_recd);
					buf = krealloc(buf, total_recd,
								 GFP_KERNEL);
				} else {
					pr_err("diag: In %s, SMD sending in packets more than %d bytes\n",
						__func__, MAX_IN_BUF_SIZE);
					return;
				}
			}
			if (pkt_len < r) {
				pr_err("diag: In %s, SMD sending incorrect pkt\n",
					__func__);
				return;
			}
			if (pkt_len > r)
				pr_debug("diag: In %s, SMD sending partial pkt %d %d %d %d\n",
					__func__, pkt_len, r, total_recd,
					loop_count);
			
			smd_read(driver->chlpass, temp_buf, r);
			temp_buf += r;
		}

		if (total_recd > 0) {
			if (!buf)
				pr_err("diag: Out of diagmem for LPASS\n");
			else {
				APPEND_DEBUG('j');
				write_ptr_lpass->length = total_recd;
				*in_busy_lpass_ptr = 1;
				diag_device_write(buf, LPASS_DATA,
							 write_ptr_lpass);
			}
		}
	} else if (driver->chlpass && !buf &&
		(driver->logging_mode == MEMORY_DEVICE_MODE)) {
		chk_logging_wakeup();
	}
}

static void diag_update_pkt_buffer(unsigned char *buf)
{
	unsigned char *ptr = driver->pkt_buf;
	unsigned char *temp = buf;

	mutex_lock(&driver->diagchar_mutex);
	if (CHK_OVERFLOW(ptr, ptr, ptr + PKT_SIZE, driver->pkt_length))
		memcpy(ptr, temp , driver->pkt_length);
	else
		printk(KERN_CRIT " Not enough buffer space for PKT_RESP\n");
	mutex_unlock(&driver->diagchar_mutex);
}

void diag_update_userspace_clients(unsigned int type)
{
	int i;

	mutex_lock(&driver->diagchar_mutex);
	for (i = 0; i < driver->num_clients; i++)
		if (driver->client_map[i].pid != 0)
			driver->data_ready[i] |= type;
	wake_up_interruptible(&driver->wait_q);
	mutex_unlock(&driver->diagchar_mutex);
}

void diag_update_sleeping_process(int process_id, int data_type)
{
	int i;

	mutex_lock(&driver->diagchar_mutex);
	for (i = 0; i < driver->num_clients; i++)
		if (driver->client_map[i].pid == process_id) {
			driver->data_ready[i] |= data_type;
			break;
		}
	wake_up_interruptible(&driver->wait_q);
	mutex_unlock(&driver->diagchar_mutex);
}

void diag_send_data(struct diag_master_table entry, unsigned char *buf,
					 int len, int type)
{
	driver->pkt_length = len;
	if (entry.process_id != NON_APPS_PROC && type != MODEM_DATA) {
		diag_update_pkt_buffer(buf);
		diag_update_sleeping_process(entry.process_id, PKT_TYPE);
	} else {
		if (len > 0) {
			if (entry.client_id == MODEM_PROC && driver->ch) {
#if 0
				if (chk_apps_master() &&
					 (int)(*(char *)buf) == MODE_CMD)
					if ((int)(*(char *)(buf+1)) ==
						RESET_ID)
						return;
#endif
				smd_write(driver->ch, buf, len);
			} else if (entry.client_id == LPASS_PROC &&
							 driver->chlpass) {
				smd_write(driver->chlpass, buf, len);
			} else if (entry.client_id == WCNSS_PROC &&
							 driver->ch_wcnss) {
				smd_write(driver->ch_wcnss, buf, len);
			} else {
				pr_alert("diag: incorrect channel");
			}
		}
	}
}

static int diag_process_apps_pkt(unsigned char *buf, int len)
{
	uint16_t subsys_cmd_code;
	int subsys_id, ssid_first, ssid_last, ssid_range;
	int packet_type = 1, i, cmd_code;
	unsigned char *temp = buf;
	int data_type;
#if defined(CONFIG_DIAG_OVER_USB)
	unsigned char *ptr;
#endif

	
	if (diag_process_apps_masks(buf, len) == 0)
		return 0;

	
	cmd_code = (int)(*(char *)buf);
	temp++;
	subsys_id = (int)(*(char *)temp);
	temp++;
	subsys_cmd_code = *(uint16_t *)temp;
	temp += 2;
	data_type = APPS_DATA;
	
	if (chk_apps_master() && cmd_code == MODE_CMD) {
		if (subsys_id != RESET_ID)
			data_type = MODEM_DATA;
	}

	pr_debug("diag: %d %d %d", cmd_code, subsys_id, subsys_cmd_code);
	for (i = 0; i < diag_max_reg; i++) {
		entry = driver->table[i];
		if (entry.process_id != NO_PROCESS) {
			if (entry.cmd_code == cmd_code && entry.subsys_id ==
				 subsys_id && entry.cmd_code_lo <=
							 subsys_cmd_code &&
				  entry.cmd_code_hi >= subsys_cmd_code) {
				diag_send_data(entry, buf, len, data_type);
				packet_type = 0;
			} else if (entry.cmd_code == 255
				  && cmd_code == 75) {
				if (entry.subsys_id ==
					subsys_id &&
				   entry.cmd_code_lo <=
					subsys_cmd_code &&
					 entry.cmd_code_hi >=
					subsys_cmd_code) {
					diag_send_data(entry, buf, len,
								 data_type);
					packet_type = 0;
				}
			} else if (entry.cmd_code == 255 &&
				  entry.subsys_id == 255) {
				if (entry.cmd_code_lo <=
						 cmd_code &&
						 entry.
						cmd_code_hi >= cmd_code) {
					diag_send_data(entry, buf, len,
								 data_type);
					packet_type = 0;
				}
			}
		}
	}
#if defined(CONFIG_DIAG_OVER_USB)
	
	if ((*buf == 0x4b) && (*(buf+1) == 0x12) &&
		(*(uint16_t *)(buf+2) == 0x0055)) {
		for (i = 0; i < 4; i++)
			*(driver->apps_rsp_buf+i) = *(buf+i);
		*(uint32_t *)(driver->apps_rsp_buf+4) = PKT_SIZE;
		encode_rsp_and_send(7);
		return 0;
	}
	
	else if (!(driver->ch) && chk_apps_only() && *buf == 0x81) {
		driver->apps_rsp_buf[0] = 0x81;
		driver->apps_rsp_buf[1] = 0x0;
		*(uint16_t *)(driver->apps_rsp_buf + 2) = 0x0;
		*(uint16_t *)(driver->apps_rsp_buf + 4) = EVENT_LAST_ID + 1;
		for (i = 0; i < EVENT_LAST_ID/8 + 1; i++)
			*(unsigned char *)(driver->apps_rsp_buf + 6 + i) = 0x0;
		encode_rsp_and_send(6 + EVENT_LAST_ID/8);
		return 0;
	}
	
	else if (!(driver->ch) && chk_apps_only()
			  && (*buf == 0x73) && *(int *)(buf+4) == 1) {
		driver->apps_rsp_buf[0] = 0x73;
		*(int *)(driver->apps_rsp_buf + 4) = 0x1; 
		*(int *)(driver->apps_rsp_buf + 8) = 0x0; 
		*(int *)(driver->apps_rsp_buf + 12) = LOG_GET_ITEM_NUM(LOG_0);
		*(int *)(driver->apps_rsp_buf + 16) = LOG_GET_ITEM_NUM(LOG_1);
		*(int *)(driver->apps_rsp_buf + 20) = LOG_GET_ITEM_NUM(LOG_2);
		*(int *)(driver->apps_rsp_buf + 24) = LOG_GET_ITEM_NUM(LOG_3);
		*(int *)(driver->apps_rsp_buf + 28) = LOG_GET_ITEM_NUM(LOG_4);
		*(int *)(driver->apps_rsp_buf + 32) = LOG_GET_ITEM_NUM(LOG_5);
		*(int *)(driver->apps_rsp_buf + 36) = LOG_GET_ITEM_NUM(LOG_6);
		*(int *)(driver->apps_rsp_buf + 40) = LOG_GET_ITEM_NUM(LOG_7);
		*(int *)(driver->apps_rsp_buf + 44) = LOG_GET_ITEM_NUM(LOG_8);
		*(int *)(driver->apps_rsp_buf + 48) = LOG_GET_ITEM_NUM(LOG_9);
		*(int *)(driver->apps_rsp_buf + 52) = LOG_GET_ITEM_NUM(LOG_10);
		*(int *)(driver->apps_rsp_buf + 56) = LOG_GET_ITEM_NUM(LOG_11);
		*(int *)(driver->apps_rsp_buf + 60) = LOG_GET_ITEM_NUM(LOG_12);
		*(int *)(driver->apps_rsp_buf + 64) = LOG_GET_ITEM_NUM(LOG_13);
		*(int *)(driver->apps_rsp_buf + 68) = LOG_GET_ITEM_NUM(LOG_14);
		*(int *)(driver->apps_rsp_buf + 72) = LOG_GET_ITEM_NUM(LOG_15);
		encode_rsp_and_send(75);
		return 0;
	}
	
	else if (!(driver->ch) && chk_apps_only()
			 && (*buf == 0x7d) && (*(buf+1) == 0x1)) {
		driver->apps_rsp_buf[0] = 0x7d;
		driver->apps_rsp_buf[1] = 0x1;
		driver->apps_rsp_buf[2] = 0x1;
		driver->apps_rsp_buf[3] = 0x0;
		
		*(int *)(driver->apps_rsp_buf + 4) = MSG_MASK_TBL_CNT - 1;
		*(uint16_t *)(driver->apps_rsp_buf + 8) = MSG_SSID_0;
		*(uint16_t *)(driver->apps_rsp_buf + 10) = MSG_SSID_0_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 12) = MSG_SSID_1;
		*(uint16_t *)(driver->apps_rsp_buf + 14) = MSG_SSID_1_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 16) = MSG_SSID_2;
		*(uint16_t *)(driver->apps_rsp_buf + 18) = MSG_SSID_2_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 20) = MSG_SSID_3;
		*(uint16_t *)(driver->apps_rsp_buf + 22) = MSG_SSID_3_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 24) = MSG_SSID_4;
		*(uint16_t *)(driver->apps_rsp_buf + 26) = MSG_SSID_4_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 28) = MSG_SSID_5;
		*(uint16_t *)(driver->apps_rsp_buf + 30) = MSG_SSID_5_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 32) = MSG_SSID_6;
		*(uint16_t *)(driver->apps_rsp_buf + 34) = MSG_SSID_6_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 36) = MSG_SSID_7;
		*(uint16_t *)(driver->apps_rsp_buf + 38) = MSG_SSID_7_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 40) = MSG_SSID_8;
		*(uint16_t *)(driver->apps_rsp_buf + 42) = MSG_SSID_8_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 44) = MSG_SSID_9;
		*(uint16_t *)(driver->apps_rsp_buf + 46) = MSG_SSID_9_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 48) = MSG_SSID_10;
		*(uint16_t *)(driver->apps_rsp_buf + 50) = MSG_SSID_10_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 52) = MSG_SSID_11;
		*(uint16_t *)(driver->apps_rsp_buf + 54) = MSG_SSID_11_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 56) = MSG_SSID_12;
		*(uint16_t *)(driver->apps_rsp_buf + 58) = MSG_SSID_12_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 60) = MSG_SSID_13;
		*(uint16_t *)(driver->apps_rsp_buf + 62) = MSG_SSID_13_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 64) = MSG_SSID_14;
		*(uint16_t *)(driver->apps_rsp_buf + 66) = MSG_SSID_14_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 68) = MSG_SSID_15;
		*(uint16_t *)(driver->apps_rsp_buf + 70) = MSG_SSID_15_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 72) = MSG_SSID_16;
		*(uint16_t *)(driver->apps_rsp_buf + 74) = MSG_SSID_16_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 76) = MSG_SSID_17;
		*(uint16_t *)(driver->apps_rsp_buf + 78) = MSG_SSID_17_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 80) = MSG_SSID_18;
		*(uint16_t *)(driver->apps_rsp_buf + 82) = MSG_SSID_18_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 84) = MSG_SSID_19;
		*(uint16_t *)(driver->apps_rsp_buf + 86) = MSG_SSID_19_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 88) = MSG_SSID_20;
		*(uint16_t *)(driver->apps_rsp_buf + 90) = MSG_SSID_20_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 92) = MSG_SSID_21;
		*(uint16_t *)(driver->apps_rsp_buf + 94) = MSG_SSID_21_LAST;
		*(uint16_t *)(driver->apps_rsp_buf + 96) = MSG_SSID_22;
		*(uint16_t *)(driver->apps_rsp_buf + 98) = MSG_SSID_22_LAST;
		encode_rsp_and_send(99);
		return 0;
	}
	
	else if (!(driver->ch) && chk_apps_only()
			 && (*buf == 0x7d) && (*(buf+1) == 0x2)) {
		ssid_first = *(uint16_t *)(buf + 2);
		ssid_last = *(uint16_t *)(buf + 4);
		ssid_range = 4 * (ssid_last - ssid_first + 1);
		
		driver->apps_rsp_buf[0] = 0x7d;
		driver->apps_rsp_buf[1] = 0x2;
		*(uint16_t *)(driver->apps_rsp_buf + 2) = ssid_first;
		*(uint16_t *)(driver->apps_rsp_buf + 4) = ssid_last;
		driver->apps_rsp_buf[6] = 0x1;
		driver->apps_rsp_buf[7] = 0x0;
		ptr = driver->apps_rsp_buf + 8;
		
		switch (ssid_first) {
		case MSG_SSID_0:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_0[i/4];
			break;
		case MSG_SSID_1:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_1[i/4];
			break;
		case MSG_SSID_2:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_2[i/4];
			break;
		case MSG_SSID_3:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_3[i/4];
			break;
		case MSG_SSID_4:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_4[i/4];
			break;
		case MSG_SSID_5:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_5[i/4];
			break;
		case MSG_SSID_6:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_6[i/4];
			break;
		case MSG_SSID_7:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_7[i/4];
			break;
		case MSG_SSID_8:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_8[i/4];
			break;
		case MSG_SSID_9:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_9[i/4];
			break;
		case MSG_SSID_10:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_10[i/4];
			break;
		case MSG_SSID_11:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_11[i/4];
			break;
		case MSG_SSID_12:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_12[i/4];
			break;
		case MSG_SSID_13:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_13[i/4];
			break;
		case MSG_SSID_14:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_14[i/4];
			break;
		case MSG_SSID_15:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_15[i/4];
			break;
		case MSG_SSID_16:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_16[i/4];
			break;
		case MSG_SSID_17:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_17[i/4];
			break;
		case MSG_SSID_18:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_18[i/4];
			break;
		case MSG_SSID_19:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_19[i/4];
			break;
		case MSG_SSID_20:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_20[i/4];
			break;
		case MSG_SSID_21:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_21[i/4];
			break;
		case MSG_SSID_22:
			for (i = 0; i < ssid_range; i += 4)
				*(int *)(ptr + i) = msg_bld_masks_22[i/4];
			break;
		}
		encode_rsp_and_send(8 + ssid_range - 1);
		return 0;
	}
	
	else if ((cpu_is_msm8x60() || chk_apps_master()) && (*buf == 0x3A)) {
		
		driver->apps_rsp_buf[0] = *buf;
		encode_rsp_and_send(0);
		msleep(5000);
		
		msm_set_restart_mode(RESTART_DLOAD);
		printk(KERN_CRIT "diag: download mode set, Rebooting SoC..\n");
		kernel_restart(NULL);
		
		return 0;
	}
	
	else if ((*buf == 0x4b) && (*(buf+1) == 0x32) &&
		(*(buf+2) == 0x03)) {
		
		if (chk_polling_response()) {
			
			for (i = 0; i < 3; i++)
				driver->apps_rsp_buf[i] = *(buf+i);
			for (i = 0; i < 13; i++)
				driver->apps_rsp_buf[i+3] = 0;

			encode_rsp_and_send(15);
			return 0;
		}
	}
	 
	else if (chk_polling_response()) {
		
		if (*buf == 0x00) {
			for (i = 0; i < 55; i++)
				driver->apps_rsp_buf[i] = 0;

			encode_rsp_and_send(54);
			return 0;
		}
		
		else if (*buf == 0x7c) {
			driver->apps_rsp_buf[0] = 0x7c;
			for (i = 1; i < 8; i++)
				driver->apps_rsp_buf[i] = 0;
			
			*(int *)(driver->apps_rsp_buf + 8) =
							 chk_config_get_id();
			*(unsigned char *)(driver->apps_rsp_buf + 12) = '\0';
			*(unsigned char *)(driver->apps_rsp_buf + 13) = '\0';
			encode_rsp_and_send(13);
			return 0;
		}
	}
#endif
	return packet_type;
}

#ifdef CONFIG_DIAG_OVER_USB
void diag_send_error_rsp(int index)
{
	int i;

	if (index > 490) {
		pr_err("diag: error response too huge, aborting\n");
		return;
	}
	driver->apps_rsp_buf[0] = 0x13; 
	for (i = 0; i < index; i++)
		driver->apps_rsp_buf[i+1] = *(driver->hdlc_buf+i);
	encode_rsp_and_send(index - 3);
}
#else
static inline void diag_send_error_rsp(int index) {}
#endif

void diag_process_hdlc(void *data, unsigned len)
{
	struct diag_hdlc_decode_type hdlc;
	int ret, type = 0;
	pr_debug("diag: HDLC decode fn, len of data  %d\n", len);
	hdlc.dest_ptr = driver->hdlc_buf;
	hdlc.dest_size = USB_MAX_OUT_BUF;
	hdlc.src_ptr = data;
	hdlc.src_size = len;
	hdlc.src_idx = 0;
	hdlc.dest_idx = 0;
	hdlc.escaping = 0;

	ret = diag_hdlc_decode(&hdlc);

	if (ret)
		type = diag_process_apps_pkt(driver->hdlc_buf,
							  hdlc.dest_idx - 3);
	else if (driver->debug_flag) {
		printk(KERN_ERR "Packet dropped due to bad HDLC coding/CRC"
				" errors or partial packet received, packet"
				" length = %d\n", len);
		print_hex_dump(KERN_DEBUG, "Dropped Packet Data: ", 16, 1,
					   DUMP_PREFIX_ADDRESS, data, len, 1);
		driver->debug_flag = 0;
	}
	
	if (type == 1 && chk_apps_only()) {
		diag_send_error_rsp(hdlc.dest_idx);
		type = 0;
	}
	
	if (!(driver->ch) && type == 1) {
		if (chk_apps_only()) {
			diag_send_error_rsp(hdlc.dest_idx);
		} else { 
			if (driver->chlpass)
				smd_write(driver->chlpass, driver->hdlc_buf,
						  hdlc.dest_idx - 3);
		}
		type = 0;
	}

#ifdef DIAG_DEBUG
	pr_debug("diag: hdlc.dest_idx = %d", hdlc.dest_idx);
	for (i = 0; i < hdlc.dest_idx; i++)
		printk(KERN_DEBUG "\t%x", *(((unsigned char *)
							driver->hdlc_buf)+i));
#endif 
	
	if ((driver->ch) && (ret) && (type) && (hdlc.dest_idx > 3)) {
		APPEND_DEBUG('g');
#ifdef CONFIG_MODEM_DIAG_MASTER
		smd_write(driver->ch, data, len);
#else
		smd_write(driver->ch, driver->hdlc_buf, hdlc.dest_idx - 3);
#endif
		APPEND_DEBUG('h');
#ifdef DIAG_DEBUG
		printk(KERN_INFO "writing data to SMD, pkt length %d\n", len);
		print_hex_dump(KERN_DEBUG, "Written Packet Data to SMD: ", 16,
			       1, DUMP_PREFIX_ADDRESS, data, len, 1);
#endif 
	}
}

#ifdef CONFIG_DIAG_OVER_USB
#define N_LEGACY_WRITE	(driver->poolsize + 6)
#define N_LEGACY_READ	1

int diagfwd_connect(void)
{
	int err;

	printk(KERN_DEBUG "diag: USB connected\n");
	err = usb_diag_alloc_req(driver->legacy_ch, N_LEGACY_WRITE,
			N_LEGACY_READ);
	if (err)
		printk(KERN_ERR "diag: unable to alloc USB req on legacy ch");

	driver->usb_connected = 1;
	driver->qxdmusb_drop = 0;
	driver->in_busy_1 = 0;
	driver->in_busy_2 = 0;
	driver->in_busy_lpass_1 = 0;
	driver->in_busy_lpass_2 = 0;
	driver->in_busy_wcnss_1 = 0;
	driver->in_busy_wcnss_2 = 0;

	
	queue_work(driver->diag_wq, &(driver->diag_read_smd_work));
	queue_work(driver->diag_wq, &(driver->diag_read_smd_lpass_work));
	queue_work(driver->diag_wq, &(driver->diag_read_smd_wcnss_work));
	
	diag_smd_cntl_notify(NULL, SMD_EVENT_DATA);
	diag_smd_lpass_cntl_notify(NULL, SMD_EVENT_DATA);
	diag_smd_wcnss_cntl_notify(NULL, SMD_EVENT_DATA);
	
	queue_work(driver->diag_wq, &(driver->diag_read_work));
#ifdef CONFIG_DIAG_SDIO_PIPE
	if (machine_is_msm8x60_fusion() || machine_is_msm8x60_fusn_ffa()) {
		if (driver->mdm_ch && !IS_ERR(driver->mdm_ch))
			diagfwd_connect_sdio();
		else
			printk(KERN_INFO "diag: No USB MDM ch");
	}
#endif
	return 0;
}

int diagfwd_disconnect(void)
{
	printk(KERN_DEBUG "diag: USB disconnected\n");
	driver->usb_connected = 0;
	driver->debug_flag = 1;
	usb_diag_free_req(driver->legacy_ch);
	if (driver->logging_mode == USB_MODE) {
		driver->qxdmusb_drop = 1;
		driver->in_busy_1 = 1;
		driver->in_busy_2 = 1;
		driver->in_busy_lpass_1 = 1;
		driver->in_busy_lpass_2 = 1;
		driver->in_busy_wcnss_1 = 1;
		driver->in_busy_wcnss_2 = 1;
	}
#ifdef CONFIG_DIAG_SDIO_PIPE
	if (machine_is_msm8x60_fusion() || machine_is_msm8x60_fusn_ffa())
		if (driver->mdm_ch && !IS_ERR(driver->mdm_ch))
			diagfwd_disconnect_sdio();
#endif
	
	return 0;
}

int diagfwd_write_complete(struct diag_request *diag_write_ptr)
{
	unsigned char *buf = diag_write_ptr->buf;
	
	
	if (buf == (void *)driver->buf_in_1) {
		driver->in_busy_1 = 0;
		APPEND_DEBUG('o');
		queue_work(driver->diag_wq, &(driver->diag_read_smd_work));
	} else if (buf == (void *)driver->buf_in_2) {
		driver->in_busy_2 = 0;
		APPEND_DEBUG('O');
		queue_work(driver->diag_wq, &(driver->diag_read_smd_work));
	} else if (buf == (void *)driver->buf_in_lpass_1) {
		driver->in_busy_lpass_1 = 0;
		APPEND_DEBUG('p');
		queue_work(driver->diag_wq,
				&(driver->diag_read_smd_lpass_work));
	} else if (buf == (void *)driver->buf_in_lpass_2) {
		driver->in_busy_lpass_2 = 0;
		APPEND_DEBUG('P');
		queue_work(driver->diag_wq,
				&(driver->diag_read_smd_lpass_work));
	} else if (buf == driver->buf_in_wcnss_1) {
		driver->in_busy_wcnss_1 = 0;
		APPEND_DEBUG('r');
		queue_work(driver->diag_wq,
			 &(driver->diag_read_smd_wcnss_work));
	} else if (buf == driver->buf_in_wcnss_2) {
		driver->in_busy_wcnss_2 = 0;
		APPEND_DEBUG('R');
		queue_work(driver->diag_wq,
			 &(driver->diag_read_smd_wcnss_work));
	}
#ifdef CONFIG_DIAG_SDIO_PIPE
	else if (buf == (void *)driver->buf_in_sdio)
		if (machine_is_msm8x60_fusion() ||
			 machine_is_msm8x60_fusn_ffa())
			diagfwd_write_complete_sdio();
		else
			pr_err("diag: Incorrect buffer pointer while WRITE");
#endif
	else {
		diagmem_free(driver, (unsigned char *)buf, POOL_TYPE_HDLC);
		diagmem_free(driver, (unsigned char *)diag_write_ptr,
						 POOL_TYPE_WRITE_STRUCT);
		APPEND_DEBUG('q');
	}
	return 0;
}

int diagfwd_read_complete(struct diag_request *diag_read_ptr)
{
	int status = diag_read_ptr->status;
	unsigned char *buf = diag_read_ptr->buf;

	
	if (buf == (void *)driver->usb_buf_out) {
		driver->read_len_legacy = diag_read_ptr->actual;
		APPEND_DEBUG('s');
#ifdef DIAG_DEBUG
		printk(KERN_INFO "read data from USB, pkt length %d",
		    diag_read_ptr->actual);
		print_hex_dump(KERN_DEBUG, "Read Packet Data from USB: ", 16, 1,
		       DUMP_PREFIX_ADDRESS, diag_read_ptr->buf,
		       diag_read_ptr->actual, 1);
#endif 
#if DIAG_XPST && !defined(CONFIG_DIAGFWD_BRIDGE_CODE)
		if (driver->nohdlc) {
			driver->usb_read_ptr->buf = driver->usb_buf_out;
			driver->usb_read_ptr->length = USB_MAX_OUT_BUF;
			usb_diag_read(driver->legacy_ch, driver->usb_read_ptr);
			return 0;
		}
#endif
		if (driver->logging_mode == USB_MODE) {
			if (status != -ECONNRESET && status != -ESHUTDOWN)
				queue_work(driver->diag_wq,
					&(driver->diag_proc_hdlc_work));
			else
				queue_work(driver->diag_wq,
						 &(driver->diag_read_work));
		}
	}
#ifdef CONFIG_DIAG_SDIO_PIPE
	else if (buf == (void *)driver->usb_buf_mdm_out) {
		if (machine_is_msm8x60_fusion() ||
				 machine_is_msm8x60_fusn_ffa()) {
			driver->read_len_mdm = diag_read_ptr->actual;
			diagfwd_read_complete_sdio();
		} else
			pr_err("diag: Incorrect buffer pointer while READ");
	}
#endif
	else
		printk(KERN_ERR "diag: Unknown buffer ptr from USB");

	return 0;
}

void diag_read_work_fn(struct work_struct *work)
{
	APPEND_DEBUG('d');
	driver->usb_read_ptr->buf = driver->usb_buf_out;
	driver->usb_read_ptr->length = USB_MAX_OUT_BUF;
	usb_diag_read(driver->legacy_ch, driver->usb_read_ptr);
	APPEND_DEBUG('e');
}

void diag_process_hdlc_fn(struct work_struct *work)
{
	APPEND_DEBUG('D');
	diag_process_hdlc(driver->usb_buf_out, driver->read_len_legacy);
	diag_read_work_fn(work);
	APPEND_DEBUG('E');
}

void diag_usb_legacy_notifier(void *priv, unsigned event,
			struct diag_request *d_req)
{
	switch (event) {
	case USB_DIAG_CONNECT:
		diagfwd_connect();
		break;
	case USB_DIAG_DISCONNECT:
		diagfwd_disconnect();
		break;
	case USB_DIAG_READ_DONE:
		diagfwd_read_complete(d_req);
		break;
	case USB_DIAG_WRITE_DONE:
		diagfwd_write_complete(d_req);
		break;
	default:
		printk(KERN_ERR "Unknown event from USB diag\n");
		break;
	}
}

#endif 

static void diag_smd_notify(void *ctxt, unsigned event)
{
	if (event == SMD_EVENT_CLOSE) {
		driver->ch = 0;
		smd_diag_initialized = 0;
		wake_up(&driver->smd_wait_q);
		queue_work(driver->diag_cntl_wq,
			 &(driver->diag_clean_modem_reg_work));
		return;
	} else if (event == SMD_EVENT_OPEN) {
		if (ch_temp)
			driver->ch = ch_temp;
		DIAGFWD_INFO("%s: smd_open(%s):, ch_temp:%p, driver->ch:%p, &driver->ch:%p\n",
			__func__, SMDDIAG_NAME, ch_temp, driver->ch, &driver->ch);
		smd_diag_initialized = 1;
	}
	wake_up(&driver->smd_wait_q);
	queue_work(driver->diag_wq, &(driver->diag_read_smd_work));
}

#if defined(CONFIG_MSM_N_WAY_SMD)
static void diag_smd_lpass_notify(void *ctxt, unsigned event)
{
	if (event == SMD_EVENT_CLOSE) {
		driver->chlpass = 0;
		wake_up(&driver->smd_wait_q);
		queue_work(driver->diag_cntl_wq,
			 &(driver->diag_clean_lpass_reg_work));
		return;
	} else if (event == SMD_EVENT_OPEN) {
		if (chlpass_temp)
			driver->chlpass = chlpass_temp;
	}
	wake_up(&driver->smd_wait_q);
	queue_work(driver->diag_wq, &(driver->diag_read_smd_lpass_work));
}
#endif

static void diag_smd_wcnss_notify(void *ctxt, unsigned event)
{
	if (event == SMD_EVENT_CLOSE) {
		driver->ch_wcnss = 0;
		wake_up(&driver->smd_wait_q);
		queue_work(driver->diag_cntl_wq,
			 &(driver->diag_clean_wcnss_reg_work));
		return;
	} else if (event == SMD_EVENT_OPEN) {
		if (ch_wcnss_temp)
			driver->ch_wcnss = ch_wcnss_temp;
	}
	wake_up(&driver->smd_wait_q);
	queue_work(driver->diag_wq, &(driver->diag_read_smd_wcnss_work));
}

#if DIAG_XPST
void diag_smd_enable(smd_channel_t *ch, char *src, int mode)
{
	int r = 0;
	static smd_channel_t *_ch;
	DIAGFWD_INFO("smd_try_open(%s): mode=%d\n", src, mode);

	mutex_lock(&driver->smd_lock);
	diag_smd_function_mode = mode;
	if (mode) {
		if (!driver->ch) {
			r = smd_open(SMDDIAG_NAME, &driver->ch, driver, diag_smd_notify);
			if (!r)
				_ch = driver->ch;
		} else
			_ch = driver->ch;
	} else {
		if (driver->ch) {
			r = smd_close(driver->ch);
			driver->ch = NULL;
			if (!r)
				_ch = driver->ch;
		}
	}
	ch = _ch;
	mutex_unlock(&driver->smd_lock);
	DIAGFWD_INFO("smd_try_open(%s): r=%d _ch=%x\n", src, r, (unsigned int)ch);
}
#endif

static int diag_smd_probe(struct platform_device *pdev)
{
	int r = 0;

	if (pdev->id == SMD_APPS_MODEM) {
		r = smd_open(SMDDIAG_NAME, &driver->ch, driver, diag_smd_notify);
		ch_temp = driver->ch;
		DIAGFWD_INFO("%s: smd_open(%s):%d, ch_temp:%p, driver->ch:%p, &driver->ch:%p\n",
			__func__, SMDDIAG_NAME, r, ch_temp, driver->ch, &driver->ch);
	}
#if defined(CONFIG_MSM_N_WAY_SMD)
	if (pdev->id == SMD_APPS_QDSP) {
		r = smd_named_open_on_edge("DIAG", SMD_APPS_QDSP,
			&driver->chlpass, driver, diag_smd_lpass_notify);
		chlpass_temp = driver->chlpass;
	}
#endif
	if (pdev->id == SMD_APPS_WCNSS) {
		r = smd_named_open_on_edge("APPS_RIVA_DATA", SMD_APPS_WCNSS
			, &driver->ch_wcnss, driver, diag_smd_wcnss_notify);
		ch_wcnss_temp = driver->ch_wcnss;
	}
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pr_debug("diag: open SMD port, Id = %d, r = %d\n", pdev->id, r);

	return 0;
}

static int diagfwd_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: suspending...\n");
	return 0;
}

static int diagfwd_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: resuming...\n");
	return 0;
}

static const struct dev_pm_ops diagfwd_dev_pm_ops = {
	.runtime_suspend = diagfwd_runtime_suspend,
	.runtime_resume = diagfwd_runtime_resume,
};

static struct platform_driver msm_smd_ch1_driver = {

	.probe = diag_smd_probe,
	.driver = {
		   .name = SMDDIAG_NAME,
		   .owner = THIS_MODULE,
		   .pm   = &diagfwd_dev_pm_ops,
		   },
};

static struct platform_driver diag_smd_lite_driver = {

	.probe = diag_smd_probe,
	.driver = {
		   .name = "APPS_RIVA_DATA",
		   .owner = THIS_MODULE,
		   .pm   = &diagfwd_dev_pm_ops,
		   },
};

void diagfwd_init(void)
{
	pr_info("%s: start", __func__);
	diag_debug_buf_idx = 0;
	driver->read_len_legacy = 0;
	driver->use_device_tree = has_device_tree();
	mutex_init(&driver->diag_cntl_mutex);

	if (driver->buf_in_1 == NULL) {
		driver->buf_in_1 = kzalloc(IN_BUF_SIZE, GFP_KERNEL);
		if (driver->buf_in_1 == NULL)
			goto err;
		kmemleak_not_leak(driver->buf_in_1);
	}
	if (driver->buf_in_2 == NULL) {
		driver->buf_in_2 = kzalloc(IN_BUF_SIZE, GFP_KERNEL);
		if (driver->buf_in_2 == NULL)
			goto err;
		kmemleak_not_leak(driver->buf_in_2);
	}
	if (driver->buf_in_lpass_1 == NULL) {
		driver->buf_in_lpass_1 = kzalloc(IN_BUF_SIZE, GFP_KERNEL);
		if (driver->buf_in_lpass_1 == NULL)
			goto err;
		kmemleak_not_leak(driver->buf_in_lpass_1);
	}
	if (driver->buf_in_lpass_2 == NULL) {
		driver->buf_in_lpass_2 = kzalloc(IN_BUF_SIZE, GFP_KERNEL);
		if (driver->buf_in_lpass_2 == NULL)
			goto err;
		kmemleak_not_leak(driver->buf_in_lpass_2);
	}
	if (driver->buf_in_wcnss_1 == NULL) {
		driver->buf_in_wcnss_1 = kzalloc(IN_BUF_SIZE, GFP_KERNEL);
		if (driver->buf_in_wcnss_1 == NULL)
			goto err;
		kmemleak_not_leak(driver->buf_in_wcnss_1);
	}
	if (driver->buf_in_wcnss_2 == NULL) {
		driver->buf_in_wcnss_2 = kzalloc(IN_BUF_SIZE, GFP_KERNEL);
		if (driver->buf_in_wcnss_2 == NULL)
			goto err;
		kmemleak_not_leak(driver->buf_in_wcnss_2);
	}
	if (driver->usb_buf_out  == NULL &&
	     (driver->usb_buf_out = kzalloc(USB_MAX_OUT_BUF,
					 GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->usb_buf_out);
	if (driver->hdlc_buf == NULL
	    && (driver->hdlc_buf = kzalloc(HDLC_MAX, GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->hdlc_buf);
	if (driver->user_space_data == NULL)
		driver->user_space_data = kzalloc(USER_SPACE_DATA, GFP_KERNEL);
		if (driver->user_space_data == NULL)
			goto err;
	kmemleak_not_leak(driver->user_space_data);
#if defined(CONFIG_DIAG_SDIO_PIPE) || defined(CONFIG_DIAGFWD_BRIDGE_CODE)
	if (driver->user_space_mdm_data == NULL)
		driver->user_space_mdm_data = kzalloc(USER_SPACE_DATA, GFP_KERNEL);
		if (driver->user_space_mdm_data == NULL)
			goto err;
	kmemleak_not_leak(driver->user_space_mdm_data);
	if (driver->user_space_qsc_data == NULL)
		driver->user_space_qsc_data = kzalloc(USER_SPACE_DATA, GFP_KERNEL);
		if (driver->user_space_qsc_data == NULL)
			goto err;
	kmemleak_not_leak(driver->user_space_qsc_data);
#endif
	if (driver->client_map == NULL &&
	    (driver->client_map = kzalloc
	     ((driver->num_clients) * sizeof(struct diag_client_map),
		   GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->client_map);
#if defined(CONFIG_DIAG_SDIO_PIPE) || defined(CONFIG_DIAGFWD_BRIDGE_CODE)
	if (driver->mdmclient_map == NULL &&
	    (driver->mdmclient_map = kzalloc
	     ((driver->num_mdmclients) * sizeof(struct diag_client_map),
		   GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->mdmclient_map);
	if (driver->qscclient_map == NULL &&
	    (driver->qscclient_map = kzalloc
	     ((driver->num_qscclients) * sizeof(struct diag_client_map),
		   GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->qscclient_map);
#endif
	if (driver->buf_tbl == NULL)
			driver->buf_tbl = kzalloc(buf_tbl_size *
			  sizeof(struct diag_write_device), GFP_KERNEL);
	if (driver->buf_tbl == NULL)
		goto err;
	kmemleak_not_leak(driver->buf_tbl);
	if (driver->data_ready == NULL &&
	     (driver->data_ready = kzalloc(driver->num_clients * sizeof(int)
							, GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->data_ready);
#if defined(CONFIG_DIAG_SDIO_PIPE) || defined(CONFIG_DIAGFWD_BRIDGE_CODE)
	if (driver->mdmdata_ready == NULL &&
	     (driver->mdmdata_ready = kzalloc(driver->num_mdmclients * sizeof(struct
					 diag_client_map), GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->mdmdata_ready);
	if (driver->qscdata_ready == NULL &&
	     (driver->qscdata_ready = kzalloc(driver->num_qscclients * sizeof(struct
					 diag_client_map), GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->qscdata_ready);
#endif
	if (driver->table == NULL &&
	     (driver->table = kzalloc(diag_max_reg*
		      sizeof(struct diag_master_table),
		       GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->table);
	if (driver->write_ptr_1 == NULL) {
		driver->write_ptr_1 = kzalloc(
			sizeof(struct diag_request), GFP_KERNEL);
		if (driver->write_ptr_1 == NULL)
			goto err;
		kmemleak_not_leak(driver->write_ptr_1);
	}
	if (driver->write_ptr_2 == NULL) {
		driver->write_ptr_2 = kzalloc(
			sizeof(struct diag_request), GFP_KERNEL);
		if (driver->write_ptr_2 == NULL)
			goto err;
		kmemleak_not_leak(driver->write_ptr_2);
	}
	if (driver->write_ptr_lpass_1 == NULL) {
		driver->write_ptr_lpass_1 = kzalloc(
			sizeof(struct diag_request), GFP_KERNEL);
		if (driver->write_ptr_lpass_1 == NULL)
			goto err;
		kmemleak_not_leak(driver->write_ptr_lpass_1);
	}
	if (driver->write_ptr_lpass_2 == NULL) {
		driver->write_ptr_lpass_2 = kzalloc(
			sizeof(struct diag_request), GFP_KERNEL);
		if (driver->write_ptr_lpass_2 == NULL)
			goto err;
		kmemleak_not_leak(driver->write_ptr_lpass_2);
	}
	if (driver->write_ptr_wcnss_1 == NULL) {
		driver->write_ptr_wcnss_1 = kzalloc(
			sizeof(struct diag_request), GFP_KERNEL);
		if (driver->write_ptr_wcnss_1 == NULL)
			goto err;
		kmemleak_not_leak(driver->write_ptr_wcnss_1);
	}
	if (driver->write_ptr_wcnss_2 == NULL) {
		driver->write_ptr_wcnss_2 = kzalloc(
			sizeof(struct diag_request), GFP_KERNEL);
		if (driver->write_ptr_wcnss_2 == NULL)
			goto err;
		kmemleak_not_leak(driver->write_ptr_wcnss_2);
	}

	if (driver->usb_read_ptr == NULL) {
		driver->usb_read_ptr = kzalloc(
			sizeof(struct diag_request), GFP_KERNEL);
		if (driver->usb_read_ptr == NULL)
			goto err;
		kmemleak_not_leak(driver->usb_read_ptr);
	}
	if (driver->pkt_buf == NULL &&
	     (driver->pkt_buf = kzalloc(PKT_SIZE,
			 GFP_KERNEL)) == NULL)
		goto err;
	kmemleak_not_leak(driver->pkt_buf);
	if (driver->apps_rsp_buf == NULL) {
		driver->apps_rsp_buf = kzalloc(APPS_BUF_SIZE, GFP_KERNEL);
		if (driver->apps_rsp_buf == NULL)
			goto err;
		kmemleak_not_leak(driver->apps_rsp_buf);
	}
	driver->diag_wq = create_singlethread_workqueue("diag_wq");
#ifdef CONFIG_DIAG_OVER_USB
	INIT_WORK(&(driver->diag_proc_hdlc_work), diag_process_hdlc_fn);
	INIT_WORK(&(driver->diag_read_work), diag_read_work_fn);
	driver->legacy_ch = usb_diag_open(DIAG_LEGACY, driver,
			diag_usb_legacy_notifier);
	if (IS_ERR(driver->legacy_ch)) {
		printk(KERN_ERR "Unable to open USB diag legacy channel\n");
		goto err;
	}
#endif
#if DIAG_XPST
	mutex_init(&driver->smd_lock);
#endif
	platform_driver_register(&msm_smd_ch1_driver);
	platform_driver_register(&diag_smd_lite_driver);

	return;
err:
	pr_err("diag: Could not initialize diag buffers");
	kfree(driver->buf_in_1);
	kfree(driver->buf_in_2);
	kfree(driver->buf_in_lpass_1);
	kfree(driver->buf_in_lpass_2);
	kfree(driver->buf_in_wcnss_1);
	kfree(driver->buf_in_wcnss_2);
	kfree(driver->buf_msg_mask_update);
	kfree(driver->buf_log_mask_update);
	kfree(driver->buf_event_mask_update);
	kfree(driver->usb_buf_out);
	kfree(driver->hdlc_buf);
	kfree(driver->client_map);
	kfree(driver->buf_tbl);
	kfree(driver->data_ready);
	kfree(driver->table);
	kfree(driver->pkt_buf);
	kfree(driver->write_ptr_1);
	kfree(driver->write_ptr_2);
	kfree(driver->write_ptr_lpass_1);
	kfree(driver->write_ptr_lpass_2);
	kfree(driver->write_ptr_wcnss_1);
	kfree(driver->write_ptr_wcnss_2);
	kfree(driver->usb_read_ptr);
	kfree(driver->apps_rsp_buf);
	kfree(driver->user_space_data);
	if (driver->diag_wq)
		destroy_workqueue(driver->diag_wq);
}

void diagfwd_exit(void)
{
	smd_close(driver->ch);
	smd_close(driver->chlpass);
	smd_close(driver->ch_wcnss);
	driver->ch = 0;		
	driver->chlpass = 0;
	driver->ch_wcnss = 0;
#ifdef CONFIG_DIAG_OVER_USB
	if (driver->usb_connected)
		usb_diag_free_req(driver->legacy_ch);
	usb_diag_close(driver->legacy_ch);
#endif
	platform_driver_unregister(&msm_smd_ch1_driver);
	platform_driver_unregister(&msm_diag_dci_driver);
	platform_driver_unregister(&diag_smd_lite_driver);
	kfree(driver->buf_in_1);
	kfree(driver->buf_in_2);
	kfree(driver->buf_in_lpass_1);
	kfree(driver->buf_in_lpass_2);
	kfree(driver->buf_in_wcnss_1);
	kfree(driver->buf_in_wcnss_2);
	kfree(driver->buf_msg_mask_update);
	kfree(driver->buf_log_mask_update);
	kfree(driver->buf_event_mask_update);
	kfree(driver->usb_buf_out);
	kfree(driver->hdlc_buf);
	kfree(driver->client_map);
	kfree(driver->buf_tbl);
	kfree(driver->data_ready);
	kfree(driver->table);
	kfree(driver->pkt_buf);
	kfree(driver->write_ptr_1);
	kfree(driver->write_ptr_2);
	kfree(driver->write_ptr_lpass_1);
	kfree(driver->write_ptr_lpass_2);
	kfree(driver->write_ptr_wcnss_1);
	kfree(driver->write_ptr_wcnss_2);
	kfree(driver->usb_read_ptr);
	kfree(driver->apps_rsp_buf);
	kfree(driver->user_space_data);
	destroy_workqueue(driver->diag_wq);
}
