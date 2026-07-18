/*
 * btstack_config.h -- BTstack configuration for the BOX_BLE builds (BLE
 * central receiver; see box_ble_central.h + wiznet-io/BLE.md).
 *
 * btstack includes this by NAME ("btstack_config.h"), found via the flattened
 * example dir on the include path. It is inert for targets that don't link
 * pico_btstack_* (nothing else includes it), same trick as box_tusb_config.h.
 *
 * Shape follows pico-examples' pico_w/bt config (the known-good set for the
 * CYW43 shared-bus HCI transport -- the controller-to-host flow control and
 * ACL buffer caps below are load-bearing there, not tuning), trimmed to
 * LE-only central + a bond table sized for a rig's worth of handhelds.
 */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// BTstack features that can be enabled
// (ENABLE_BLE itself comes from the SDK's pico_btstack_ble interface define --
//  defining it here too is a redefinition warning across every btstack TU)
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL              /* costs little; the bench TX personality shares this config */
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP             /* hci_dump_embedded_stdout.c (SDK compiles it) #errors without this */
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS   /* LE Secure Connections (Just Works) pairing */

// BTstack configuration. buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)    /* fits an MTU-247 notification = one 128B dserv frame */
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_GATT_CLIENTS 2
#define MAX_NR_HCI_CONNECTIONS 2          /* handheld + one spare (bench second box) */
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_L2CAP_SERVICES 2
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 16
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

// Limit number of ACL/SCO Buffer to use by stack to avoid cyw43 shared bus overrun
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN (255 + 4)
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Link Key DB and LE Device DB using TLV on top of Flash Sector interface
// (bank placement: see the flash note in box_ble_central.h -- RP2350 default
// offset lands the banks at top-of-flash -12K/-8K, clear of the persist sector)
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

// We don't give btstack a malloc; GATT server unused so far, so a small ATT DB
#define MAX_ATT_DB_SIZE 512

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS
// map btstack_assert onto Pico SDK assert()
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#endif /* BTSTACK_CONFIG_H */
