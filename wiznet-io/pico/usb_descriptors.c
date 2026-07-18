/*
 * usb_descriptors.c -- TinyUSB descriptors for the USB-CDC box (BOX_NET_USB only).
 *
 * Composite CDC device: CDC0 = console/CLI, CDC1 = binary dserv data channel.
 * Compiled for the usb AND dual targets (pico/CMakeLists.txt target_sources);
 * the w6300/pico2w builds don't own TinyUSB so they don't include it.
 *
 * The manufacturer/product/serial strings below are the box's stable USB
 * IDENTITY: on Linux udev exposes them as ID_VENDOR=dserv / ID_MODEL=
 * extio_USB_box / ID_SERIAL_SHORT=<chip id> and a /dev/serial/by-id/ symlink,
 * so the host (config/extioconf.tcl) selects the box by identity rather than by
 * enumeration order -- it can't grab a co-resident CDC device (e.g. a juicer).
 *
 * The BLE HANDHELD (BOX_NET_BLE) presents a DIFFERENT identity: its data rides
 * the radio, so its (console-only-in-practice) CDCs must NEVER match the host's
 * box discovery -- 2026-07-17 bench: the handheld's dead data CDC outsorted the
 * receiver's on macOS and extioconf read silence. Product deliberately contains
 * no "extio" substring (the Linux by-id glob is *extio*if02*), and it gets its
 * own PID. See BLE.md.
 *
 * Standard boilerplate modelled on TinyUSB's cdc_dual_ports example. Scales to a
 * single data-only CDC when built with -DCFG_TUD_CDC=1.
 */
#include "tusb.h"
#include "pico/unique_id.h"

#ifdef BOX_NET_BLE
#define EXTIO_USB_PID     0x100C            /* dev PID for the BLE handheld */
#define EXTIO_USB_PRODUCT "dserv handheld"  /* NO "extio": host box-globs must not match */
#else
#define EXTIO_USB_PID     0x100B            /* dev PID for the extio USB box */
#define EXTIO_USB_PRODUCT "extio USB box"
#endif

/* ---------------- Device descriptor ---------------- */
/* Composite (>1 interface assoc) -> use IAD device class. */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,      /* Raspberry Pi VID -- DEV USE. Assign your own PID for production. */
    .idProduct          = EXTIO_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *) &desc_device; }

/* ---------------- Configuration descriptor ---------------- */
enum {
    ITF_NUM_CDC_0 = 0,
    ITF_NUM_CDC_0_DATA,
#if CFG_TUD_CDC > 1
    ITF_NUM_CDC_1,
    ITF_NUM_CDC_1_DATA,
#endif
    ITF_NUM_TOTAL
};

#define EPNUM_CDC_0_NOTIF   0x81
#define EPNUM_CDC_0_OUT     0x02
#define EPNUM_CDC_0_IN      0x82
#define EPNUM_CDC_1_NOTIF   0x83
#define EPNUM_CDC_1_OUT     0x04
#define EPNUM_CDC_1_IN      0x84

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    /* CDC 0: console / CLI (string index 4) */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_0, 4, EPNUM_CDC_0_NOTIF, 8, EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),
#if CFG_TUD_CDC > 1
    /* CDC 1: binary dserv data (string index 5) */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_1, 5, EPNUM_CDC_1_NOTIF, 8, EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return desc_fs_configuration;
}

/* ---------------- String descriptors ---------------- */
enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL, STRID_CDC0, STRID_CDC1 };

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},   /* 0: supported language = English (0x0409) */
    "dserv",                      /* 1: Manufacturer */
    EXTIO_USB_PRODUCT,            /* 2: Product (the identity the host globs match) */
    NULL,                         /* 3: Serial -> filled from the board unique id */
    "extio CLI",                  /* 4: CDC0 interface */
    "extio data",                 /* 5: CDC1 interface */
};

static uint16_t _desc_str[32 + 1];

/* board unique id -> hex serial string (once) */
static const char *serial_string(void)
{
    static char s[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static int done = 0;
    if (!done) { pico_get_unique_board_id_string(s, sizeof s); done = 1; }
    return s;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;
    size_t chr_count;

    if (index == STRID_LANGID) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = (index == STRID_SERIAL) ? serial_string() : string_desc_arr[index];
        if (!str) return NULL;
        chr_count = strlen(str);
        size_t max = (sizeof(_desc_str) / 2) - 1;
        if (chr_count > max) chr_count = max;
        for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
