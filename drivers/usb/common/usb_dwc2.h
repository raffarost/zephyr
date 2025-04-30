/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_USB_COMMON_USB_DWC2
#define ZEPHYR_DRIVERS_USB_COMMON_USB_DWC2

struct usb_dwc2_reg *dwc2_get_base(const struct device *dev);
void dwc2_flush_rx_fifo(const struct device *dev);
void dwc2_flush_tx_fifo(const struct device *dev, const uint8_t fnum);
uint32_t dwc2_get_txfdep(const struct device *dev, const uint32_t f_idx);
int uhc_dwc2_core_soft_reset(const struct device *dev);

#endif /* ZEPHYR_DRIVERS_USB_COMMON_USB_DWC2 */
