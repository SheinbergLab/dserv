/*
 * box_tusb_config.h -- TinyUSB device config for the USB-CDC box (BOX_NET_USB).
 *
 * Selected ONLY by the usb target, via  -DCFG_TUSB_CONFIG_FILE="box_tusb_config.h".
 * Named box_* (not tusb_config.h) on purpose: build.sh flattens pico/ + net/ into
 * one shared example dir, so a file literally called tusb_config.h would shadow the
 * pico-sdk's own config and break pico_stdio_usb on the W6300 / pico2w images. With
 * the override macro, this file is inert for every target except usb.
 *
 * Two CDCs: CDC0 = console/CLI (reserved; currently the CLI/printf goes to UART on
 * the usb build), CDC1 = the binary dserv data channel (BOX_USB_CDC_DATA=1). For a
 * single data-only first-light image, build with -DCFG_TUD_CDC=1 -DBOX_USB_CDC_DATA=0.
 */
#ifndef BOX_TUSB_CONFIG_H
#define BOX_TUSB_CONFIG_H

/* CFG_TUSB_MCU / CFG_TUSB_OS are supplied by the pico-sdk tinyusb_device target as
 * -D defines; guard just in case, never override what the SDK set. */
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU        OPT_MCU_RP2040   /* rp2350 uses the rp2040 USB port in pico-sdk */
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS         OPT_OS_PICO
#endif

#define CFG_TUD_ENABLED     1
#ifndef CFG_TUSB_RHPORT0_MODE          /* required by newer TinyUSB tusb_init() */
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#endif
#ifndef CFG_TUD_MAX_SPEED
#define CFG_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED
#endif
#define CFG_TUD_ENDPOINT0_SIZE 64

/* Class counts. 2 CDC = CLI(0) + data(1). Override to 1 for a data-only build. */
#ifndef CFG_TUD_CDC
#define CFG_TUD_CDC         2
#endif
#define CFG_TUD_MSC         0
#define CFG_TUD_HID         0
#define CFG_TUD_MIDI        0
#define CFG_TUD_VENDOR      0

/* FIFOs >= 128 so a whole 128-byte dserv frame writes/reads without splitting
 * (the host-side framer resyncs anyway, but atomic frames keep it clean).
 * TX = 1024 so a full publish burst (heartbeat 4 frames, sync 3) fits without
 * entering client_send's bounded drain-guard -- that loop now runs on the RT
 * core, so it should only ever trigger when the host truly stalls. */
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 1024
#define CFG_TUD_CDC_EP_BUFSIZE 64      /* full-speed bulk max packet */

#endif /* BOX_TUSB_CONFIG_H */
