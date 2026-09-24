/* SPDX-License-Identifier: MIT */

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(astrial_bulk_echo, LOG_LEVEL_INF);

#define BULK_OUT 0x01
#define BULK_IN  0x81
#define CHUNK_SIZE 4096

struct echo_descriptors {
	struct usb_if_descriptor iface;
	struct usb_ep_descriptor out_fs;
	struct usb_ep_descriptor in_fs;
	struct usb_ep_descriptor out_hs;
	struct usb_ep_descriptor in_hs;
	struct usb_desc_header end;
};

static struct echo_descriptors descriptors = {
	.iface = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = USB_BCC_VENDOR,
	},
	.out_fs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = BULK_OUT,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64),
	},
	.in_fs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = BULK_IN,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64),
	},
	.out_hs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = BULK_OUT,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(512),
	},
	.in_hs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = BULK_IN,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(512),
	},
	.end = { .bLength = 0 },
};

static const struct usb_desc_header *const fs_desc[] = {
	(struct usb_desc_header *)&descriptors.iface,
	(struct usb_desc_header *)&descriptors.out_fs,
	(struct usb_desc_header *)&descriptors.in_fs,
	(struct usb_desc_header *)&descriptors.end,
};
static const struct usb_desc_header *const hs_desc[] = {
	(struct usb_desc_header *)&descriptors.iface,
	(struct usb_desc_header *)&descriptors.out_hs,
	(struct usb_desc_header *)&descriptors.in_hs,
	(struct usb_desc_header *)&descriptors.end,
};

static atomic_t enabled;

static int queue_out(struct usbd_class_data *c_data, struct net_buf *buf)
{
	int rc;
	if (!atomic_get(&enabled)) {
		net_buf_unref(buf);
		return 0;
	}
	net_buf_reset(buf);
	udc_get_buf_info(buf)->ep = BULK_OUT;
	rc = usbd_ep_enqueue(c_data, buf);
	if (rc != 0) {
		LOG_ERR("queue OUT failed: %d", rc);
		net_buf_unref(buf);
	}
	return rc;
}

static int new_out(struct usbd_class_data *c_data)
{
	struct net_buf *buf = usbd_ep_buf_alloc(c_data, BULK_OUT, CHUNK_SIZE);
	if (buf == NULL) {
		LOG_ERR("OUT buffer allocation failed");
		return -ENOMEM;
	}
	return queue_out(c_data, buf);
}

static int request_done(struct usbd_class_data *c_data,
			struct net_buf *buf, int err)
{
	struct udc_buf_info *info = udc_get_buf_info(buf);
	uint8_t ep = info->ep;
	int rc;

	if (err != 0) {
		net_buf_unref(buf);
		if (atomic_get(&enabled)) {
			LOG_ERR("transfer 0x%02x failed: %d", ep, err);
			return new_out(c_data);
		}
		return 0;
	}

	if (ep == BULK_OUT && buf->len != 0) {
		/* Return exactly the bytes received; the host checks every byte. */
		info->ep = BULK_IN;
		rc = usbd_ep_enqueue(c_data, buf);
		if (rc == 0) {
			return 0;
		}
		LOG_ERR("queue IN failed: %d", rc);
		net_buf_unref(buf);
		return new_out(c_data);
	}

	return queue_out(c_data, buf);
}

static void enable(struct usbd_class_data *c_data)
{
	atomic_set(&enabled, 1);
	(void)new_out(c_data);
}

static void disable(struct usbd_class_data *c_data)
{
	ARG_UNUSED(c_data);
	atomic_clear(&enabled);
}

static const void *get_desc(struct usbd_class_data *c_data,
			    enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
	return speed == USBD_SPEED_HS ? hs_desc : fs_desc;
}

static int init(struct usbd_class_data *c_data)
{
	ARG_UNUSED(c_data);
	return 0;
}

static const struct usbd_class_api echo_api = {
	.request = request_done,
	.enable = enable,
	.disable = disable,
	.init = init,
	.get_desc = get_desc,
};

USBD_DEFINE_CLASS(astrial_echo_0, &echo_api, NULL, NULL);
