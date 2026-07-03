/*
 * wifi_config.h -- WiFi credentials for the Pico 2 W (BOX_NET_LWIP) build.
 * Override at build time:  -DWIFI_SSID="myssid" -DWIFI_PASSWORD="mypass"
 * (do NOT commit real credentials).
 */
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#ifndef WIFI_SSID
#define WIFI_SSID     "change-me"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "change-me"
#endif
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 30000
#endif

#endif /* WIFI_CONFIG_H */
