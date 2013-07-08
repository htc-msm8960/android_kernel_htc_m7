/*
 * Copyright (C) 2008 Google, Inc.
 * Author: Nick Pelly <npelly@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __ASM_ARCH_MSM_SERIAL_HS_H
#define __ASM_ARCH_MSM_SERIAL_HS_H

#include<linux/serial_core.h>

struct msm_serial_hs_platform_data {
	int wakeup_irq;  
	
	unsigned char inject_rx_on_wakeup;
	char rx_to_inject;
	unsigned config_gpio;
	int uart_tx_gpio;
	int uart_rx_gpio;
	int uart_cts_gpio;
	int uart_rfr_gpio;
	int userid;

#ifdef CONFIG_MSM_SERIAL_HS_BRCM
	unsigned char bt_wakeup_pin;
	unsigned char host_wakeup_pin;
#endif
};

#ifdef CONFIG_MSM_SERIAL_HS_BRCM
extern void imc_msm_hs_request_clock_on(struct uart_port *uport);
#endif
unsigned int msm_hs_tx_empty(struct uart_port *uport);
void msm_hs_request_clock_off(struct uart_port *uport);
void msm_hs_request_clock_on(struct uart_port *uport);
void msm_hs_set_mctrl(struct uart_port *uport,
				    unsigned int mctrl);
#endif
