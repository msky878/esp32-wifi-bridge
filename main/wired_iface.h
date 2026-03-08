/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */



/*
**This file is based on "wired_iface.h" from the "sta2eth" example project found in the esp-idf:
https://github.com/espressif/esp-idf/blob/release/v5.5/examples/network/sta2eth/main/wired_iface.h

**All changes are marked by comments, everything else is left the same as the original project.

**Miroslav Michalsky 2026
**ESP32 Wi-Fi Bridge
**sta2eth
*/



#pragma once

typedef esp_err_t (*wired_rx_cb_t)(void *buffer, uint16_t len, void *ctx);

typedef void (*wired_free_cb_t)(void *buffer, void *ctx);

typedef enum
{
    FROM_WIRED,
    TO_WIRED
} mac_spoof_direction_t;

void mac_spoof(mac_spoof_direction_t direction, uint8_t *buffer, uint16_t len, uint8_t own_mac[6]);

esp_err_t wired_bridge_init(wired_rx_cb_t rx_cb, wired_free_cb_t free_cb);

esp_err_t wired_send(void *buffer, uint16_t len, void *buff_free_arg);

esp_err_t wired_netif_init(void);



// **Added declaration of custom function
esp_err_t wired_send_credentials(const char *ssid, const char *password);
