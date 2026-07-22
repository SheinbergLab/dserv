/*
 * box_usbd.h -- USB device (device_next) context bring-up for the extio box.
 * Instantiates the usbd context, registers the CDC-ACM class instances declared
 * in the board overlay, and enables the controller (High-Speed when available).
 */
#ifndef BOX_USBD_H
#define BOX_USBD_H

#include <zephyr/usb/usbd.h>

/* Set up descriptors + configurations, register all classes, init and enable the
 * USB device. Optional msg_cb receives usbd lifecycle events. 0 on success. */
int box_usbd_start(usbd_msg_cb_t msg_cb);

#endif /* BOX_USBD_H */
