/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */



/*
**This file is based on "sta2eth_main.c" from the "sta2eth" example project found in the esp-idf:
https://github.com/espressif/esp-idf/blob/release/v5.5/examples/network/sta2eth/main/sta2eth_main.c

**All changes are marked by comments, everything else is left the same as the original project.
**To differentiate my comments from the originals, my comments always start with "**" (2 asterisks)

**Miroslav Michalsky 2026
**ESP32 Wi-Fi Bridge
**sta2eth
*/



#include <string.h>
#include <esp_timer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_private/wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "wired_iface.h"

static const char *TAG = "sta2eth-main"; // **renamed from example_sta2wired

static EventGroupHandle_t s_event_flags;
static bool s_wifi_is_connected = false;
static uint8_t s_sta_mac[6];

const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;
const int RECONFIGURE_BIT = BIT2;



// **added function prototypes from manual_config.c
bool is_provisioned(void);
void start_config_services(void);



// **Unmodified function from the example
/**
 * WiFi -- Wired packet path
 */
static esp_err_t wired_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (s_wifi_is_connected)
    {
        mac_spoof(FROM_WIRED, buffer, len, s_sta_mac);
        if (esp_wifi_internal_tx(ESP_IF_WIFI_STA, buffer, len) != ESP_OK)
        {
            ESP_LOGD(TAG, "Failed to send packet to WiFi!");
        }
    }
    return ESP_OK;
}



// **Unmodified function from the example
static void wifi_buff_free(void *buffer, void *ctx)
{
    esp_wifi_internal_free_rx_buffer(buffer);
}



// **Modified function from the example
// ** - changed debug message text
static esp_err_t wifi_recv_callback(void *buffer, uint16_t len, void *eb)
{
    mac_spoof(TO_WIRED, buffer, len, s_sta_mac);
    if (wired_send(buffer, len, eb) != ESP_OK)
    {
        esp_wifi_internal_free_rx_buffer(eb);
        ESP_LOGD(TAG, "Failed to send packet to Ethernet!"); // **changed from "Failed to send packet to USB!"
    }
    return ESP_OK;
}



// **Unmodified function from the example
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "Wi-Fi STA disconnected");
        s_wifi_is_connected = false;
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, NULL);
        esp_wifi_connect();

        xEventGroupClearBits(s_event_flags, CONNECTED_BIT);
        xEventGroupSetBits(s_event_flags, DISCONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "Wi-Fi STA connected");
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, wifi_recv_callback);
        s_wifi_is_connected = true;
        xEventGroupClearBits(s_event_flags, DISCONNECTED_BIT);
        xEventGroupSetBits(s_event_flags, CONNECTED_BIT);
    }
}



// **Modified function from the example
// ** - removed certain error checks and status verification
// ** - added information message
static esp_err_t connect_wifi(void)
{
    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK)
    {
        // configuration not available, report error to restart provisioning
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_cfg.sta.ssid);
    esp_wifi_connect();

    // **non-blocking return - events handle the rest
    return ESP_OK;
}



/**
 * GPIO button functionality
 */
// **reset button support not tested
#define GPIO_INPUT CONFIG_BRIDGE_RECONFIGURE_BUTTON
#define GPIO_LONG_PUSH_US   2000000  /* push for 2 seconds to reconfigure */



// **Unmodified function from the example
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    static int64_t last_pushed = -1;
    if (gpio_get_level(GPIO_INPUT) == 0)
    {
        last_pushed = esp_timer_get_time();
    }
    else
    {
        uint64_t now = esp_timer_get_time();
        if (last_pushed != -1 && now - last_pushed > GPIO_LONG_PUSH_US)
        {
            BaseType_t high_task_wakeup;
            xEventGroupSetBitsFromISR(s_event_flags, RECONFIGURE_BIT, &high_task_wakeup);
            if (high_task_wakeup)
                portYIELD_FROM_ISR();
        }
        last_pushed = -1;
    }
}



// **Modified function from the example
// ** - added pin support check
static void gpio_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .pin_bit_mask = (1ULL << GPIO_INPUT),
        .mode = GPIO_MODE_INPUT,
        // **only enable internal pull-up if the pin supports it (GPIO < 34)
        .pull_up_en = (GPIO_INPUT < 34) ? 1 : 0
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT, gpio_isr_handler, NULL);
}



// **Heavily modified function from the example
/**
 * Application
 */
void sta2eth_app_main(void)
{
    /* Initialize NVS and WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_netif_init();
    s_event_flags = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();    // **create STA interface for the bridge
    esp_netif_create_default_wifi_ap();     // **create AP interface for config

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    gpio_init();

    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));

    // **this allows simultaneous AP (config) and STA (bridge)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    start_config_services();

    ESP_ERROR_CHECK(esp_wifi_start());

    // **start bridge if provisioned
    if (is_provisioned())
    {
        ESP_LOGI(TAG, "Credentials found - starting bridge!");

        // **initialize Ethernet (Wired) Interface
        ESP_ERROR_CHECK(wired_bridge_init(wired_recv_callback, wifi_buff_free));

        // **connect to home (source) Wi-Fi
        connect_wifi();
    }
    else
    {
        ESP_LOGW(TAG, "No credentials found - connect to 'espmoduleconfig' to setup!");
    }

    // **reset button support not tested but I kept the code in case I add button later?
    while (1)
    {
        EventBits_t bits = xEventGroupWaitBits(s_event_flags, RECONFIGURE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & RECONFIGURE_BIT)
        {
            ESP_LOGW(TAG, "Button Pressed - factory resetting...");
            esp_wifi_restore();
            esp_restart();
        }
    }
}
