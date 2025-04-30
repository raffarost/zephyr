/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <usb_dwc2_hw.h>
#include <usb_dwc2.h>

#if defined(CONFIG_UDC_DWC2)
LOG_MODULE_DECLARE(udc_dwc2, CONFIG_UDC_DRIVER_LOG_LEVEL);
#elif defined(CONFIG_UHC_DWC2)
LOG_MODULE_DECLARE(uhc_dwc2, CONFIG_UHC_DRIVER_LOG_LEVEL);
#endif

/*
 * Common functions for both DWC2 device and host driver
 */

void dwc2_flush_rx_fifo(const struct device *dev)
{
	struct usb_dwc2_reg *const base = dwc2_get_base(dev);
	mem_addr_t grstctl_reg = (mem_addr_t)&base->grstctl;

	sys_write32(USB_DWC2_GRSTCTL_RXFFLSH, grstctl_reg);
	while (sys_read32(grstctl_reg) & USB_DWC2_GRSTCTL_RXFFLSH) {
	}
}

void dwc2_flush_tx_fifo(const struct device *dev, const uint8_t fnum)
{
	struct usb_dwc2_reg *const base = dwc2_get_base(dev);
	mem_addr_t grstctl_reg = (mem_addr_t)&base->grstctl;
	uint32_t grstctl;

	grstctl = usb_dwc2_set_grstctl_txfnum(fnum) | USB_DWC2_GRSTCTL_TXFFLSH;

	sys_write32(grstctl, grstctl_reg);
	while (sys_read32(grstctl_reg) & USB_DWC2_GRSTCTL_TXFFLSH) {
	}
}

int uhc_dwc2_core_soft_reset(const struct device *dev)
{
	struct usb_dwc2_reg *const base = dwc2_get_base(dev);
	mem_addr_t grstctl_reg = (mem_addr_t)&base->grstctl;
	const unsigned int csr_timeout_us = 10000UL;
	uint32_t cnt = 0UL;

	/* Check AHB master idle state */
	while (!(sys_read32(grstctl_reg) & USB_DWC2_GRSTCTL_AHBIDLE)) {
		k_busy_wait(1);

		if (++cnt > csr_timeout_us) {
			LOG_ERR("Wait for AHB idle timeout, GRSTCTL 0x%08x",
				sys_read32(grstctl_reg));
			return -EIO;
		}
	}

	/* Apply Core Soft Reset */
	sys_write32(USB_DWC2_GRSTCTL_CSFTRST, grstctl_reg);

	cnt = 0UL;
	do {
		if (++cnt > csr_timeout_us) {
			LOG_ERR("Wait for CSR done timeout, GRSTCTL 0x%08x",
				sys_read32(grstctl_reg));
			return -EIO;
		}

		k_busy_wait(1);
/* quirk commonality needs to be analyzed for this */
#if 0
		if (dwc2_quirk_is_phy_clk_off(dev)) {
			/* Software reset won't finish without PHY clock */
			return -EIO;
		}
#endif
	} while (sys_read32(grstctl_reg) & USB_DWC2_GRSTCTL_CSFTRST &&
		 !(sys_read32(grstctl_reg) & USB_DWC2_GRSTCTL_CSFTRSTDONE));

	/* CSFTRSTDONE is W1C so the write must have the bit set to clear it */
	sys_clear_bits(grstctl_reg, USB_DWC2_GRSTCTL_CSFTRST);

	LOG_DBG("DWC2 core reset done");

	return 0;
}
