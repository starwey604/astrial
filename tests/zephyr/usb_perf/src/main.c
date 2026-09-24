/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(astrial_usb_perf, LOG_LEVEL_INF);

USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfg, CONFIG_ASTRIAL_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(product, CONFIG_ASTRIAL_USB_PRODUCT);
USBD_CONFIGURATION_DEFINE(fs_config, USB_SCD_SELF_POWERED, 100, NULL);
USBD_CONFIGURATION_DEFINE(hs_config, USB_SCD_SELF_POWERED, 100, NULL);
USBD_DEVICE_DEFINE(astrial_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_ASTRIAL_USB_VID, CONFIG_ASTRIAL_USB_PID);

#define TRY(expr) do { int rc = (expr); if (rc != 0) { \
	LOG_ERR("%s failed: %d", #expr, rc); return rc; } } while (0)

static void usb_message(struct usbd_context *const ctx,
			const struct usbd_msg *const msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));
	if (msg->type == USBD_MSG_VBUS_READY) {
		int rc = usbd_enable(ctx);
		if (rc != 0 && rc != -EALREADY) {
			LOG_ERR("usbd_enable failed: %d", rc);
		}
	} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
		(void)usbd_disable(ctx);
	}
}

int main(void)
{
	TRY(usbd_add_descriptor(&astrial_usbd, &lang));
	TRY(usbd_add_descriptor(&astrial_usbd, &mfg));
	TRY(usbd_add_descriptor(&astrial_usbd, &product));

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&astrial_usbd) == USBD_SPEED_HS) {
		TRY(usbd_add_configuration(&astrial_usbd, USBD_SPEED_HS,
					   &hs_config));
		TRY(usbd_register_class(&astrial_usbd, "astrial_echo_0",
					USBD_SPEED_HS, 1));
		TRY(usbd_device_set_code_triple(&astrial_usbd, USBD_SPEED_HS,
						0, 0, 0));
	} else {
		LOG_ERR("high-speed USB controller required");
		return -ENOTSUP;
	}

	TRY(usbd_add_configuration(&astrial_usbd, USBD_SPEED_FS, &fs_config));
	TRY(usbd_register_class(&astrial_usbd, "astrial_echo_0",
				USBD_SPEED_FS, 1));
	TRY(usbd_device_set_code_triple(&astrial_usbd, USBD_SPEED_FS, 0, 0, 0));
	TRY(usbd_msg_register_cb(&astrial_usbd, usb_message));
	TRY(usbd_init(&astrial_usbd));

	/* MCX EHCI may not report VBUS_READY when J11 was connected at boot. */
	int rc = usbd_enable(&astrial_usbd);
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("usbd_enable failed: %d", rc);
		return rc;
	}
	LOG_INF("bulk echo ready: %04x:%04x", CONFIG_ASTRIAL_USB_VID,
		CONFIG_ASTRIAL_USB_PID);
	return 0;
}
