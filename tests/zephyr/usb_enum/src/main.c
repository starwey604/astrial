/*
 * SPDX-License-Identifier: MIT
 *
 * Astrial USB enumeration-check firmware for FRDM-MCXN947.
 *
 * Enumerates the on-chip HS USB controller as a vendor device using the
 * device_next stack and the built-in loopback class so that:
 *
 *   - the host-side enumeration check can confirm VID/PID, bus speed and
 *     endpoint layout (lsusb / dmesg), and
 *   - astrial::usb finds the interface it expects by default
 *     (interface 0, vendor class, bulk OUT 0x01 / bulk IN 0x81).
 *
 * Deliberately self-contained: this file does not use the Zephyr tree's
 * samples/subsys/usb/common helpers.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(astrial_usb_enum, LOG_LEVEL_INF);

USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfg, CONFIG_ASTRIAL_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(product, CONFIG_ASTRIAL_USB_PRODUCT);

USBD_CONFIGURATION_DEFINE(fs_config, USB_SCD_SELF_POWERED, 100, NULL);
USBD_CONFIGURATION_DEFINE(hs_config, USB_SCD_SELF_POWERED, 100, NULL);

USBD_DEVICE_DEFINE(astrial_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_ASTRIAL_USB_VID,
		   CONFIG_ASTRIAL_USB_PID);

#define ASTRIAL_TRY(expr)                                                       \
	do {                                                                    \
		int _err = (expr);                                              \
		if (_err != 0) {                                                \
			LOG_ERR("%s failed: %d", #expr, _err);                  \
			return _err;                                            \
		}                                                               \
	} while (0)

static void enum_msg_cb(struct usbd_context *const ctx,
			const struct usbd_msg *const msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

	if (!usbd_can_detect_vbus(ctx)) {
		return;
	}

	/* Controller reports VBUS state: enable/disable with the cable. */
	if (msg->type == USBD_MSG_VBUS_READY) {
		int err = usbd_enable(ctx);

		if (err != 0) {
			LOG_ERR("usbd_enable failed: %d", err);
		}
	}

	if (msg->type == USBD_MSG_VBUS_REMOVED) {
		(void)usbd_disable(ctx);
	}
}

int main(void)
{
	ASTRIAL_TRY(usbd_add_descriptor(&astrial_usbd, &lang));
	ASTRIAL_TRY(usbd_add_descriptor(&astrial_usbd, &mfg));
	ASTRIAL_TRY(usbd_add_descriptor(&astrial_usbd, &product));

	/*
	 * Loopback is a single vendor interface without an IAD, so the
	 * device descriptor must defer the class code to the interface
	 * descriptors (0/0/0 instead of the Miscellaneous triple that
	 * USBD_DEVICE_DEFINE preloads).
	 */
	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&astrial_usbd) == USBD_SPEED_HS) {
		LOG_INF("UDC capability: high-speed");
		ASTRIAL_TRY(usbd_add_configuration(&astrial_usbd, USBD_SPEED_HS,
						   &hs_config));
		ASTRIAL_TRY(usbd_register_class(&astrial_usbd, "loopback_0",
						USBD_SPEED_HS, 1));
		ASTRIAL_TRY(usbd_device_set_code_triple(&astrial_usbd,
							USBD_SPEED_HS, 0, 0, 0));
	} else {
		LOG_INF("UDC capability: full-speed only");
	}

	ASTRIAL_TRY(usbd_add_configuration(&astrial_usbd, USBD_SPEED_FS,
					   &fs_config));
	ASTRIAL_TRY(usbd_register_class(&astrial_usbd, "loopback_0",
					USBD_SPEED_FS, 1));
	ASTRIAL_TRY(usbd_device_set_code_triple(&astrial_usbd, USBD_SPEED_FS,
						0, 0, 0));

	ASTRIAL_TRY(usbd_msg_register_cb(&astrial_usbd, enum_msg_cb));
	ASTRIAL_TRY(usbd_init(&astrial_usbd));

	/*
	 * When the controller cannot sense VBUS there is no event to wait
	 * for, enable right away; otherwise enum_msg_cb does it on
	 * USBD_MSG_VBUS_READY.
	 */
	if (!usbd_can_detect_vbus(&astrial_usbd)) {
		ASTRIAL_TRY(usbd_enable(&astrial_usbd));
		LOG_INF("USB device support enabled (no VBUS detection)");
	}

	LOG_INF("Astrial USB enum check ready: %04x:%04x",
		CONFIG_ASTRIAL_USB_VID, CONFIG_ASTRIAL_USB_PID);
	return 0;
}
