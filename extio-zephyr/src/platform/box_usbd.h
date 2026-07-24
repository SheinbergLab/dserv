/*
 * box_usbd.h -- USB device (device_next) context bring-up for the extio box.
 * Instantiates the usbd context, registers the CDC-ACM class instances declared
 * in the board overlay, and enables the controller (High-Speed when available).
 */
#ifndef BOX_USBD_H
#define BOX_USBD_H

#include <zephyr/usb/usbd.h>

/* Set up descriptors + configurations, register the CDC-ACM classes, init and
 * enable the USB device. Optional msg_cb receives usbd lifecycle events.
 * 0 on success.
 *
 * with_data_pipe = 0 registers ONLY the console CDC, so the box enumerates with
 * no binary dserv frame pipe at all. That is what a DECLARED-Ethernet box wants:
 * its data path is the network, and a pipe it will never read only invites a
 * host to open it and believe it has a USB box (dserv's extio subprocess does
 * exactly that -- it claims the port, lists the box in extio/boxes, and
 * hot-swap-polls it forever). The console stays up either way: a deployed box
 * must remain diagnosable over its own cable.
 *
 * This mirrors the Pico's single `w6300` build, which never inits TinyUSB and so
 * offers a console only; the `dual` build enumerates both unconditionally and
 * has the same wart this flag fixes. */
int box_usbd_start(usbd_msg_cb_t msg_cb, int with_data_pipe);

#endif /* BOX_USBD_H */
