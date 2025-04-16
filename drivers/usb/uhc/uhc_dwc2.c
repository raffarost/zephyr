/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/logging/log.h>

#include <usb_dwc2_hw.h>
#include <usb_dwc2.h>
#include "uhc_common.h"

LOG_MODULE_REGISTER(uhc_dwc2, CONFIG_UHC_DRIVER_LOG_LEVEL);
#include "uhc_dwc2_vendor_quirks.h"

/* Recommended RxFIFO size for Host mode
 * Formula: ((MPS / 4) * 2) + 2
 *
 * - mps: largest packet size expected in the USB transfer. Must be a multiple of 4.
 * - *2: Two slots per packet are typically required to account for back-to-back packets.
 * - +2: Additional slots required to store status information for each received packet.
  */
#define UDH_DWC2_HOST_GRXFSIZ(mps) (((mps / 4U) * 2U) + 2U)
#define UDH_DWC2_HOST_GNPTXFSIZ(mps) (((mps) / 4U) * 2U)
#define UDH_DWC2_HOST_GPTXFSIZ(mps, mc) (((mps) * (mc)) / 4U)

/* TX FIFO0 depth in 32-bit words (used by control IN endpoint)
 * Try 2 * bMaxPacketSize0 to allow simultaneous operation with a fallback to
 * whatever is available when 2 * bMaxPacketSize0 is not possible.
 */
#define UDH_DWC2_FIFO0_DEPTH		(2 * 16U)

#define USB_DWC2_HAINT_CHAN_MASK    ((1UL << OTG_NUM_HOST_CHAN) - 1)

#define UHC_DWC2_IRQ_TIMEOUT_MS 200

enum dwc2_suspend_type {
	DWC2_SUSPEND_NO_POWER_SAVING,
	DWC2_SUSPEND_HIBERNATION,
};

/* Registers that have to be stored before Partial Power Down or Hibernation */
struct dwc2_reg_backup {
	uint32_t gotgctl;
	uint32_t gahbcfg;
	uint32_t gusbcfg;
	uint32_t gintmsk;
	uint32_t grxfsiz;
	uint32_t gnptxfsiz;
	uint32_t gi2cctl;
	uint32_t glpmcfg;
	uint32_t gdfifocfg;
};

struct uhc_dwc2_data {
	struct k_sem irq_sem;
	struct dwc2_reg_backup backup;
	uint32_t ghwcfg1;
	uint32_t max_xfersize;
	uint32_t max_pktcnt;
	uint16_t dfifodepth;
	uint16_t rxfifo_depth;
	uint16_t max_txfifo_depth[16];
	/* Configuration flags */
	unsigned int dynfifosizing : 1;
	unsigned int bufferdma : 1;
	unsigned int syncrst : 1;
	/* Defect workarounds */
	unsigned int wa_essregrestored : 1;
	/* Runtime state flags */
	unsigned int hibernated : 1;
	unsigned int hfir_set : 1;
	enum dwc2_suspend_type suspend_type;
};

struct usb_dwc2_reg *dwc2_get_base(const struct device *dev)
{
	const struct uhc_dwc2_config *const config = dev->config;

	return config->base;
}

static int uhc_dwc2_lock(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	return k_mutex_lock(&data->mutex, K_FOREVER);
}

static int uhc_dwc2_unlock(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	return k_mutex_unlock(&data->mutex);
}

static int uhc_dwc2_sof_enable(const struct device *dev)
{
	return 0;
}

static int uhc_dwc2_bus_suspend(const struct device *dev)
{
	return 0;
}

static int uhc_dwc2_bus_reset(const struct device *dev)
{
	return 0;
}

static int uhc_dwc2_bus_resume(const struct device *dev)
{
	return 0;
}

static int uhc_dwc2_enqueue(const struct device *dev,
			    struct uhc_transfer *const xfer)
{
	return 0;
}

static int uhc_dwc2_dequeue(const struct device *dev,
			    struct uhc_transfer *const xfer)
{
	return 0;
}

static int uhc_dwc2_init_controller(const struct device *dev)
{
	const struct uhc_dwc2_config *const config = dev->config;
	struct uhc_dwc2_data *priv = uhc_get_private(dev);
	struct usb_dwc2_reg *const base = config->base;
	mem_addr_t gintsts_reg = (mem_addr_t)&base->gintsts;
	mem_addr_t gahbcfg_reg = (mem_addr_t)&base->gahbcfg;
	mem_addr_t gusbcfg_reg = (mem_addr_t)&base->gusbcfg;
	uint32_t gsnpsid;
	uint32_t gusbcfg;
	uint32_t gahbcfg;
	uint32_t ghwcfg1;
	uint32_t ghwcfg2;
	uint32_t ghwcfg3;
	uint32_t ghwcfg4;
	int ret;

	ret = uhc_dwc2_core_soft_reset(dev);
	if (ret) {
		return ret;
	}

	/* Enable RTL workarounds based on controller revision */
	gsnpsid = sys_read32((mem_addr_t)&base->gsnpsid);
	priv->wa_essregrestored = gsnpsid < USB_DWC2_GSNPSID_REV_5_00A;

	ghwcfg1 = sys_read32((mem_addr_t)&base->ghwcfg1);
	ghwcfg2 = sys_read32((mem_addr_t)&base->ghwcfg2);
	ghwcfg3 = sys_read32((mem_addr_t)&base->ghwcfg3);
	ghwcfg4 = sys_read32((mem_addr_t)&base->ghwcfg4);

	if (!(ghwcfg4 & USB_DWC2_GHWCFG4_DEDFIFOMODE)) {
		LOG_ERR("Only dedicated TX FIFO mode is supported");
		return -ENOTSUP;
	}

	/*
	 * Force host mode as we do no support role changes.
	 * Wait 25ms for the change to take effect.
	 */
	gusbcfg = USB_DWC2_GUSBCFG_FORCEHSTMODE;
	sys_write32(gusbcfg, gusbcfg_reg);
	k_msleep(25);

	LOG_DBG("Operation mode: %s", (sys_read32(gintsts_reg) &
		USB_DWC2_GINTSTS_CURMOD) ? "host" : "device");

	/* Buffer DMA is always supported in Internal DMA mode.
	 * TODO: check and support descriptor DMA if available
	 */
	priv->bufferdma = (usb_dwc2_get_ghwcfg2_otgarch(ghwcfg2) ==
			   USB_DWC2_GHWCFG2_OTGARCH_INTERNALDMA);

	if (!IS_ENABLED(CONFIG_UHC_DWC2_DMA)) {
		priv->bufferdma = 0;
	} else if (priv->bufferdma) {
		LOG_WRN("Buffer DMA mode enabled");
	}

	if (ghwcfg2 & USB_DWC2_GHWCFG2_DYNFIFOSIZING) {
		LOG_DBG("Dynamic FIFO Sizing enabled");
		priv->dynfifosizing = true;
	}

	if (IS_ENABLED(CONFIG_UDC_DWC2_HIBERNATION) &&
	    ghwcfg4 & USB_DWC2_GHWCFG4_HIBERNATION) {
		LOG_INF("Hibernation enabled");
		priv->suspend_type = DWC2_SUSPEND_HIBERNATION;
	} else {
		priv->suspend_type = DWC2_SUSPEND_NO_POWER_SAVING;
	}

	LOG_DBG("OTG architecture (OTGARCH) %u, mode (OTGMODE) %u",
		usb_dwc2_get_ghwcfg2_otgarch(ghwcfg2),
		usb_dwc2_get_ghwcfg2_otgmode(ghwcfg2));

	priv->dfifodepth = usb_dwc2_get_ghwcfg3_dfifodepth(ghwcfg3);
	LOG_DBG("DFIFO depth (DFIFODEPTH) %u bytes", priv->dfifodepth * 4);

	priv->max_pktcnt = GHWCFG3_PKTCOUNT(usb_dwc2_get_ghwcfg3_pktsizewidth(ghwcfg3));
	priv->max_xfersize = GHWCFG3_XFERSIZE(usb_dwc2_get_ghwcfg3_xfersizewidth(ghwcfg3));
	LOG_DBG("Max packet count %u, Max transfer size %u",
		priv->max_pktcnt, priv->max_xfersize);

	LOG_DBG("Vendor Control interface support enabled: %s",
		(ghwcfg3 & USB_DWC2_GHWCFG3_VNDCTLSUPT) ? "true" : "false");

	LOG_DBG("PHY interface type: FSPHYTYPE %u, HSPHYTYPE %u, DATAWIDTH %u",
		usb_dwc2_get_ghwcfg2_fsphytype(ghwcfg2),
		usb_dwc2_get_ghwcfg2_hsphytype(ghwcfg2),
		usb_dwc2_get_ghwcfg4_phydatawidth(ghwcfg4));

	LOG_DBG("LPM mode is %s",
		(ghwcfg3 & USB_DWC2_GHWCFG3_LPMMODE) ? "enabled" : "disabled");

	if (ghwcfg3 & USB_DWC2_GHWCFG3_RSTTYPE) {
		priv->syncrst = 1;
	}

	/* Configure AHB, select Completer or DMA mode */
	gahbcfg = sys_read32(gahbcfg_reg);

	if (priv->bufferdma) {
		gahbcfg |= USB_DWC2_GAHBCFG_DMAEN;
	} else {
		gahbcfg &= ~USB_DWC2_GAHBCFG_DMAEN;
	}

	sys_write32(gahbcfg, gahbcfg_reg);

	if (usb_dwc2_get_ghwcfg4_phydatawidth(ghwcfg4)) {
		gusbcfg |= USB_DWC2_GUSBCFG_PHYIF_16_BIT;
	}

	/* Update PHY configuration */
	sys_write32(gusbcfg, gusbcfg_reg);

	return 0;
}

static void uhc_dwc2_fifo_size(const struct device *dev)
{
	const struct uhc_dwc2_config *const config = dev->config;
	struct uhc_dwc2_data *priv = uhc_get_private(dev);
	struct usb_dwc2_reg *const base = config->base;
	uint32_t val;

	priv->rxfifo_depth = usb_dwc2_get_grxfsiz(sys_read32((mem_addr_t)&base->grxfsiz));

	if (priv->dynfifosizing) {
		uint32_t gnptxfsiz;

		priv->rxfifo_depth = MIN(priv->rxfifo_depth, UDH_DWC2_HOST_GRXFSIZ(64));
		sys_write32(usb_dwc2_set_grxfsiz(priv->rxfifo_depth), (mem_addr_t)&base->grxfsiz);

		/* Set TxFIFO 0 depth */
		val = sys_read32((mem_addr_t)&base->gnptxfsiz);
		val = MIN(UDH_DWC2_FIFO0_DEPTH, usb_dwc2_get_gnptxfsiz_nptxfdep(val));
		gnptxfsiz = usb_dwc2_set_gnptxfsiz_nptxfdep(val) |
			    usb_dwc2_set_gnptxfsiz_nptxfstaddr(priv->rxfifo_depth);

		sys_write32(gnptxfsiz, (mem_addr_t)&base->gnptxfsiz);
	}

	LOG_DBG("RX FIFO size %u bytes", priv->rxfifo_depth * 4);
}

static int uhc_dwc2_host_connect(const struct device *dev)
{
	const struct uhc_dwc2_config *const config = dev->config;
	struct uhc_data *data = dev->data;
	struct uhc_dwc2_data *priv = uhc_get_private(dev);
	struct usb_dwc2_reg *const base = config->base;

	/* Trigger port reset process */
	sys_set_bits((mem_addr_t)&base->hprt, USB_DWC2_HPRT_PRTRST);
	k_msleep(20);
	sys_clear_bits((mem_addr_t)&base->hprt, USB_DWC2_HPRT_PRTRST);

	uint32_t hprt = sys_read32((mem_addr_t)&base->hprt);

	switch (usb_dwc2_get_hprt_prtspd(hprt))
	{
		case USB_DWC2_DCFG_DEVSPD_USBHS20:
			LOG_DBG("High speed enumerated");
		break;

		case USB_DWC2_DCFG_DEVSPD_USBFS20:
			LOG_DBG("Full speed enumerated");
		break;

		case USB_DWC2_DCFG_DEVSPD_USBLS116:
			LOG_DBG("Low speed enumerated");
		break;
	}

	/* Set frame interval according to speed and PHY clock. Can be done
	 * only once and only after HPRT.PrtEnaPort has been set
	 */
	if ((hprt & USB_DWC2_HPRT_PRTENA) && !priv->hfir_set) {
		uint32_t phy_clk = dwc2_quirk_get_phy_clk(dev);
		uint32_t divisor = data->caps.hs ? 8000U : 1000U;
		uint32_t hfir = usb_dwc2_set_hfir_frint(phy_clk / divisor);
		sys_write32(hfir, (mem_addr_t)&base->hfir);
		priv->hfir_set = true;
	} else {
		LOG_WRN("Error writing hfir: port still not enabled");
	}

	return 0;
}

static void uhc_dwc2_host_disconnect(const struct device *dev)
{
	/* Power VBUS off */
//	sys_clear_bits((mem_addr_t)&base->hprt, USB_DWC2_HPRT_PRTPWR);
}

static int uhc_dwc2_host_init(const struct device *dev)
{
	const struct uhc_dwc2_config *const config = dev->config;
	struct uhc_data *data = dev->data;
	struct usb_dwc2_reg *const base = config->base;

	/* Unmask port interrupts */
	sys_write32(USB_DWC2_GINTSTS_PRTINT, (mem_addr_t)&base->gintmsk);

	/* Set maximum speed support */
	if (data->caps.hs) {
		sys_clear_bits((mem_addr_t)&base->hcfg, USB_DWC2_HCFG_FSLSSUPP);
	} else {
		sys_set_bits((mem_addr_t)&base->hcfg, USB_DWC2_HCFG_FSLSSUPP);
	}

	/* Dynamic FIFO sizing */
	uhc_dwc2_fifo_size(dev);

	/* Flush FIFOs */
	dwc2_flush_rx_fifo(dev);
	dwc2_flush_tx_fifo(dev, 0);

	/* Unmask additional host-mode interrupts */
	sys_write32(USB_DWC2_GINTSTS_HCHINT  |  /* Host Channel Interrupt */
		USB_DWC2_GINTSTS_PRTINT          |  /* Port Interrupt (connect/disconnect/reset) */
		USB_DWC2_GINTSTS_DISCONNINT      |  /* Disconnect detected */
		USB_DWC2_GINTSTS_CONIDSTSCHNG    |  /* Connector ID Status Change (OTG, optional) */
		USB_DWC2_GINTSTS_SESSREQINT      |  /* Session Request Interrupt (OTG, optional) */
		USB_DWC2_GINTSTS_SOF             |  /* Start of Frame (1ms tick, optional for testing) */
		USB_DWC2_GINTSTS_RXFLVL,            /* Rx FIFO Non-Empty Interrupt */
		(mem_addr_t)&base->gintmsk);

	/* Power VBUS */
	sys_set_bits((mem_addr_t)&base->hprt, USB_DWC2_HPRT_PRTPWR);

	LOG_DBG("USB host initialized");

	return 0;
}

static int uhc_dwc2_channel_init(const struct device *dev, int channel)
{
	const struct uhc_dwc2_config *const config = dev->config;
	struct usb_dwc2_reg *const base = config->base;

	if (channel >= OTG_NUM_HOST_CHAN) {
		LOG_ERR("Invalid channel number");
		return -EINVAL;
	}
#if 0	/* causing a freeze, probably stuck interrupt */
	/* Unmask TxFIFO empty (or half) interrupts  */
	sys_set_bits((mem_addr_t)&base->gintmsk, USB_DWC2_GINTSTS_PTXFEMP);
#endif
	/* Unmask interrupt for specific channel */
	sys_set_bits((mem_addr_t)&base->haintmsk, (1 << channel));

	/* Unmask transaction-related interrupts for the channel */
	sys_write32(USB_DWC2_HCINT_XFERCOMPL    |  /* Transfer Complete */
		USB_DWC2_HCINT_CHHLTD               |  /* Channel Halted */
		USB_DWC2_HCINT_AHBERR               |  /* AHB Error */
		USB_DWC2_HCINT_STALL                |  /* Stall Condition */
		USB_DWC2_HCINT_NAK                  |  /* NAK Response */
		USB_DWC2_HCINT_XACTERR              |  /* Transaction Error */
		USB_DWC2_HCINT_BNA,                    /* Buffer Not Available */
		(mem_addr_t)&base->host_chans[channel].hcintmsk);

	return 0;
}

static int uhc_dwc2_enable(const struct device *dev)
{
	struct usb_dwc2_reg *const base = dwc2_get_base(dev);
	int err;

	err = dwc2_quirk_pre_enable(dev);
	if (err) {
		LOG_ERR("Quirk pre enable failed %d", err);
		return err;
	}

	/* Enable global interrupt */
	sys_set_bits((mem_addr_t)&base->gahbcfg, USB_DWC2_GAHBCFG_GLBINTRMASK);
	dwc2_quirk_irq_enable_func(dev);

	err = uhc_dwc2_host_init(dev);
	if (err) {
		return err;
	}

	/* test only */
	err = uhc_dwc2_channel_init(dev, 0);
	if (err) {
		return err;
	}

	err = dwc2_quirk_post_enable(dev);
	if (err) {
		LOG_ERR("Quirk post enable failed %d", err);
		return err;
	}

	return 0;
}

static int uhc_dwc2_disable(const struct device *dev)
{
	return 0;
}

static int uhc_dwc2_shutdown(const struct device *dev)
{
	return 0;
}

static int uhc_dwc2_preinit(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	(void)dwc2_quirk_caps(dev);

	k_mutex_init(&data->mutex);

	return 0;
}

static int uhc_dwc2_init(const struct device *dev)
{
	int ret;

	ret = dwc2_quirk_init(dev);

	if (ret) {
		LOG_ERR("Quirk init failed %d", ret);
		return ret;
	}

	ret = uhc_dwc2_init_controller(dev);
	if (ret) {
		return ret;
	}

	return 0;
}

static void uhc_dwc2_isr_handler(const struct device *dev)
{
	struct uhc_data *data = dev->data;
	struct uhc_dwc2_data *priv = data->priv;
	struct usb_dwc2_reg *const base = dwc2_get_base(dev);
	mem_addr_t gintsts_reg = (mem_addr_t)&base->gintsts;
	uint32_t int_status;
	uint32_t gintmsk;

	gintmsk = sys_read32((mem_addr_t)&base->gintmsk);

	/*  Read and handle interrupt status register */
	while ((int_status = sys_read32(gintsts_reg) & gintmsk)) {

		LOG_DBG("GINTSTS 0x%x", int_status);

		if (int_status & USB_DWC2_GINTSTS_PRTINT) {
			/* Clear port connected interrupt */
			sys_write32(USB_DWC2_GINTSTS_PRTINT, gintsts_reg);

			uint32_t hprt = sys_read32((mem_addr_t)&base->hprt);

			if (hprt & USB_DWC2_HPRT_PRTCONNDET) {
				sys_set_bits((mem_addr_t)&base->hprt, USB_DWC2_HPRT_PRTCONNDET);
				LOG_DBG("Port connect detected");
				uhc_dwc2_host_connect(dev);
			} else if (hprt & USB_DWC2_HPRT_PRTENCHNG) {
				sys_set_bits((mem_addr_t)&base->hprt, USB_DWC2_HPRT_PRTENCHNG);
				LOG_DBG("Port %s, device %s",
					(hprt & USB_DWC2_HPRT_PRTENA) ? "enabled" : "disabled",
					(hprt & USB_DWC2_HPRT_PRTCONNSTS) ? "connected" : "disconnected");
			} else if (hprt & USB_DWC2_HPRT_PRTOVRCURRCHNG) {
				sys_set_bits((mem_addr_t)&base->hprt, USB_DWC2_HPRT_PRTOVRCURRCHNG);
				LOG_DBG("Overcurrent state %s", (hprt & USB_DWC2_HPRT_PRTOVRCURRACT) ? "active" : "inactive");
			}
		}

		if (int_status & USB_DWC2_GINTSTS_DISCONNINT) {
			/* Clear device disconnect interrupt */
			sys_write32(USB_DWC2_GINTSTS_DISCONNINT, gintsts_reg);
			uhc_dwc2_host_disconnect(dev);
			LOG_DBG("Device disconnect detected");
		}

		if (int_status & USB_DWC2_GINTSTS_CONIDSTSCHNG) {
			sys_write32(USB_DWC2_GINTSTS_CONIDSTSCHNG, gintsts_reg);
			LOG_DBG("Connector ID status change (OTG)");
		}

		if (int_status & USB_DWC2_GINTSTS_SESSREQINT) {
			sys_write32(USB_DWC2_GINTSTS_SESSREQINT, gintsts_reg);
			LOG_DBG("Session request interrupt (OTG)");
		}

		if (int_status & USB_DWC2_GINTSTS_HCHINT) {
			sys_write32(USB_DWC2_GINTSTS_HCHINT, gintsts_reg);
			LOG_DBG("Host channel interrupt");
			// Handle specific host channel interrupts here
		}

		if (int_status & USB_DWC2_GINTSTS_RXFLVL) {
			// RXFLVL is *not* cleared by writing to GINTSTS!
			// Instead, it is cleared by reading the GRXSTSP register.
			LOG_DBG("RxFIFO non-empty interrupt");
	
			// e.g., handle_rxfifo(dev);
		}
	
		if (int_status & USB_DWC2_GINTSTS_SOF) {
			sys_write32(USB_DWC2_GINTSTS_SOF, gintsts_reg);
			LOG_DBG("Start of Frame (SOF)");
		}
	
		if (int_status & USB_DWC2_GINTSTS_PTXFEMP) {
			sys_write32(USB_DWC2_GINTSTS_PTXFEMP, gintsts_reg);
			LOG_DBG("Periodic TxFIFO Empty");
		}

		if (int_status & USB_DWC2_GINTSTS_HCHINT) {
			sys_write32(USB_DWC2_GINTSTS_HCHINT, gintsts_reg);

			uint32_t haint = sys_read32((mem_addr_t)&base->haint) & USB_DWC2_HAINT_CHAN_MASK;

			for (int chan = 0; chan < OTG_NUM_HOST_CHAN; chan++) {
				if (haint & (1 << chan)) {
					uint32_t hcint = sys_read32((mem_addr_t)&base->host_chans[chan].hcint);
					/* Clear all interrupt flags for this channel */
					sys_write32(hcint, (mem_addr_t)&base->host_chans[chan].hcint);

					LOG_DBG("Channel %d interrupt: 0x%08x", chan, hcint);

					/* test: Decode common error bits */
					if (hcint & USB_DWC2_HCINT_BBLERR)
						LOG_WRN("Channel %d: Babble error", chan);
					if (hcint & USB_DWC2_HCINT_XACTERR)
						LOG_WRN("Channel %d: Transaction error", chan);
					if (hcint & USB_DWC2_HCINT_STALL)
						LOG_WRN("Channel %d: STALL", chan);
					if (hcint & USB_DWC2_HCINT_NAK)
						LOG_INF("Channel %d: NAK", chan);
				}
			}
		}
	}

	(void)dwc2_quirk_irq_clear(dev);
}

static const struct uhc_api uhc_dwc2_api = {
	.lock = uhc_dwc2_lock,
	.unlock = uhc_dwc2_unlock,
	.init = uhc_dwc2_init,
	.enable = uhc_dwc2_enable,
	.disable = uhc_dwc2_disable,
	.shutdown = uhc_dwc2_shutdown,

	.bus_reset = uhc_dwc2_bus_reset,
	.sof_enable  = uhc_dwc2_sof_enable,
	.bus_suspend = uhc_dwc2_bus_suspend,
	.bus_resume = uhc_dwc2_bus_resume,

	.ep_enqueue = uhc_dwc2_enqueue,
	.ep_dequeue = uhc_dwc2_dequeue,
};

#define DT_DRV_COMPAT snps_dwc2

#define UHC_DWC2_VENDOR_QUIRK_GET(n)						\
	COND_CODE_1(DT_NODE_VENDOR_HAS_IDX(DT_DRV_INST(n), 1),			\
		    (&dwc2_vendor_quirks_##n),					\
		    (NULL))

#define UHC_DWC2_DT_INST_REG_ADDR(n)						\
			COND_CODE_1(DT_NUM_REGS(DT_DRV_INST(n)), (DT_INST_REG_ADDR(n)),		\
					(DT_INST_REG_ADDR_BY_NAME(n, core)))

static struct uhc_dwc2_data uhc_dwc2_data_host = {
	.irq_sem = Z_SEM_INITIALIZER(uhc_dwc2_data_host.irq_sem, 0, 1),
};

static const struct uhc_dwc2_config uhc_dwc2_config_host = {
	.base = (struct usb_dwc2_reg *)UHC_DWC2_DT_INST_REG_ADDR(0),
	.quirks = UHC_DWC2_VENDOR_QUIRK_GET(0),
};

static struct uhc_data uhc_dwc2_priv_data = {
	.priv = &uhc_dwc2_data_host,
};

DEVICE_DT_INST_DEFINE(0, uhc_dwc2_preinit, NULL,
		      &uhc_dwc2_priv_data, &uhc_dwc2_config_host,
		      POST_KERNEL, 99,
		      &uhc_dwc2_api);
