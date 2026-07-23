/*
 * box_usbd.c -- device_next USB context, adapted from Zephyr's
 * samples/subsys/usb/common/sample_usbd_init.c (the pattern Zephyr intends apps
 * to copy). Self-contained: VID/PID/strings are hard-set here rather than pulled
 * from the SAMPLE_USBD_* Kconfig. Registers every CDC-ACM class instance from the
 * board overlay (the data pipe + the console), with both HS and FS configurations.
 */
#include "box_usbd.h"

#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(box_usbd, LOG_LEVEL_INF);

/* Placeholder VID/PID: Zephyr's test VID. A shipping box needs a real allocation. */
#define BOX_USB_VID 0x2fe3
#define BOX_USB_PID 0x0002

USBD_DEVICE_DEFINE(box_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   BOX_USB_VID, BOX_USB_PID);

USBD_DESC_LANG_DEFINE(box_lang);
USBD_DESC_MANUFACTURER_DEFINE(box_mfr, "dserv");
USBD_DESC_PRODUCT_DEFINE(box_product, "extio box");
USBD_DESC_SERIAL_NUMBER_DEFINE(box_sn);        /* from HWINFO device id */

USBD_DESC_CONFIG_DEFINE(box_fs_desc, "extio FS");
USBD_DESC_CONFIG_DEFINE(box_hs_desc, "extio HS");

static const uint8_t box_attr = 0;             /* bus-powered, no remote wakeup */
USBD_CONFIGURATION_DEFINE(box_fs_config, box_attr, 250, &box_fs_desc);
USBD_CONFIGURATION_DEFINE(box_hs_config, box_attr, 250, &box_hs_desc);

/* CDC-ACM is a multi-interface class -> advertise the IAD code triple. */
static void fix_triple(struct usbd_context *ctx, enum usbd_speed speed)
{
	usbd_device_set_code_triple(ctx, speed, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
}

int box_usbd_start(usbd_msg_cb_t msg_cb)
{
	int err;

	if ((err = usbd_add_descriptor(&box_ctx, &box_lang)) ||
	    (err = usbd_add_descriptor(&box_ctx, &box_mfr)) ||
	    (err = usbd_add_descriptor(&box_ctx, &box_product)) ||
	    (err = usbd_add_descriptor(&box_ctx, &box_sn))) {
		LOG_ERR("descriptor add failed (%d)", err);
		return err;
	}

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&box_ctx) == USBD_SPEED_HS) {
		if ((err = usbd_add_configuration(&box_ctx, USBD_SPEED_HS, &box_hs_config)) ||
		    (err = usbd_register_all_classes(&box_ctx, USBD_SPEED_HS, 1, NULL))) {
			LOG_ERR("HS config failed (%d)", err);
			return err;
		}
		fix_triple(&box_ctx, USBD_SPEED_HS);
	}

	if ((err = usbd_add_configuration(&box_ctx, USBD_SPEED_FS, &box_fs_config)) ||
	    (err = usbd_register_all_classes(&box_ctx, USBD_SPEED_FS, 1, NULL))) {
		LOG_ERR("FS config failed (%d)", err);
		return err;
	}
	fix_triple(&box_ctx, USBD_SPEED_FS);

	if (msg_cb && (err = usbd_msg_register_cb(&box_ctx, msg_cb))) {
		return err;
	}
	if ((err = usbd_init(&box_ctx))) {
		LOG_ERR("usbd_init failed (%d)", err);
		return err;
	}
	if ((err = usbd_enable(&box_ctx))) {
		LOG_ERR("usbd_enable failed (%d)", err);
		return err;
	}
	return 0;
}
