/*
 * usb_descriptors.c -- TinyUSB descriptors for the USB-CDC box (BOX_NET_USB only).
 *
 * Composite CDC device: CDC0 = console/CLI, CDC1 = binary dserv data channel.
 * Compiled into the firmware only for the usb target (added to sources in
 * pico/CMakeLists.txt under BOX_TARGET=usb); inert dead file for other targets.
 *
 * Standard boilerplate modelled on TinyUSB's cdc_dual_ports example. Scales to a
 * single data-only CDC when built with -DCFG_TUD_CDC=1.
 */
#include "tusb.h"
#include "pico/unique_id.h"

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
    .idProduct          = 0x100B,      /* arbitrary dev PID for the extio USB box */
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
    "extio USB box",              /* 2: Product */
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
