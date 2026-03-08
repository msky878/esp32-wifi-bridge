/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */



/*
**This file is based on "ethernet_example_main.c" from the "eth2ap" example project found in the esp-idf:
https://github.com/espressif/esp-idf/blob/release/v5.5/examples/network/eth2ap/main/ethernet_example_main.c

**All changes are marked by comments, everything else is left the same as the original project.
**To differentiate my comments from the originals, my comments always start with "**" (2 asterisks)

**Miroslav Michalsky 2026
**ESP32 Wi-Fi Bridge
**eth2ap
*/



#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth_driver.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_private/wifi.h"
#include "ethernet_init.h"
#include "nvs.h"    // **added NVS library

static const char *TAG = "eth2ap";  // **renamed from eth2ap_example
static esp_eth_handle_t s_eth_handle = NULL;
static QueueHandle_t flow_control_queue = NULL;
static bool s_sta_is_connected = false;
static bool s_ethernet_is_connected = false;
static uint8_t s_eth_mac[6];

#define FLOW_CONTROL_QUEUE_TIMEOUT_MS (100)
#define FLOW_CONTROL_QUEUE_LENGTH (40)
#define FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS (100)

typedef struct
{
    void *packet;
    uint16_t length;
} flow_control_msg_t;



#define L2_CONFIG_ETHERTYPE 0x8901  // **added confguration EtherType for the pkt_eth2wifi function

// **Added structure for payload for the pkt_eth2wifi function
// **for receiving the configuration frames
typedef struct __attribute__((packed))
{
    uint8_t magic[4]; // **"CONF"
    uint8_t ssid_len;
    char ssid[32];
    uint8_t pass_len;
    char password[64];
} config_payload_t;



// **Newly added NVS helper function
// **saves Wi-Fi AP credentials to NVS
// ** 1. opens the NVS "storage" namespace with R/W permissions
// ** 2. stages the new SSID and Password strings using unique keys
// ** 3. atomically commits the changes to flash memory to ensure data integrity
static void save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Saving new credentials to NVS");
        nvs_set_str(nvs_handle, "ap_ssid", ssid);
        nvs_set_str(nvs_handle, "ap_pass", password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}



// **Newly added NVS helper function
// **loads Wi-Fi AP credentials from NVS
// ** 1. first initializes the configuration with factory-default values from Kconfig
// ** 2. attempts to open the "storage" NVS partition in read-only mode
// ** 3. only overwrites the defaults if BOTH a valid SSID and Password are recovered from flash
// ** - this ensures the device remains reachable even if the NVS is empty or data is corrupted
static void load_wifi_credentials(wifi_config_t *config)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);

    // **default wifi configuartion from Kconfig in case the NVS fails
    strncpy((char *)config->ap.ssid, CONFIG_BRIDGE_WIFI_SSID, 32);
    strncpy((char *)config->ap.password, CONFIG_BRIDGE_WIFI_PASSWORD, 64);

    if (err == ESP_OK)
    {
        size_t ssid_len = 32;
        size_t pass_len = 64;
        char nvs_ssid[32] = {0};
        char nvs_pass[64] = {0};

        // **load BOTH data from NVS or nothing
        if (nvs_get_str(nvs_handle, "ap_ssid", nvs_ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(nvs_handle, "ap_pass", nvs_pass, &pass_len) == ESP_OK)
        {

            ESP_LOGI(TAG, "Loaded AP credentials from NVS - AP SSID: %s", nvs_ssid);

            memset(config->ap.ssid, 0, 32);
            memset(config->ap.password, 0, 64);
            strncpy((char *)config->ap.ssid, nvs_ssid, 32);
            strncpy((char *)config->ap.password, nvs_pass, 64);

            config->ap.ssid_len = strlen((char *)config->ap.ssid);
        }
        nvs_close(nvs_handle);
    }
}



// **Modified function from the example
// ** - changed error message to debug message
// Forward packets from Wi-Fi to Ethernet
static esp_err_t pkt_wifi2eth(void *buffer, uint16_t len, void *eb)
{
    if (s_ethernet_is_connected)
    {
        if (esp_eth_transmit(s_eth_handle, buffer, len) != ESP_OK)
        {
            ESP_LOGD(TAG, "Ethernet send packet failed");
        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}



// **Modified function from the example
// ** - implemented L2 configuration protocol (EtherType 0x8901)
// ** - added a handler to extract SSID and password from custom frames
// ** - new credentials saved to NVS
// ** - generates and sends Ethernet ACK frame back to the sender
// Forward packets from Ethernet to Wi-Fi
// Note that, Ethernet works faster than Wi-Fi on ESP32,
// so we need to add an extra queue to balance their speed difference.
static esp_err_t pkt_eth2wifi(esp_eth_handle_t eth_handle, uint8_t *buffer, uint32_t len, void *priv)
{
    // **added source code start:
    if (len >= (ETH_HEADER_LEN + sizeof(config_payload_t)))
    {
        uint8_t *eth_type_ptr = buffer + 12;
        uint16_t eth_type = (eth_type_ptr[0] << 8) | eth_type_ptr[1];

        if (eth_type == L2_CONFIG_ETHERTYPE)
        {
            config_payload_t *payload = (config_payload_t *)(buffer + ETH_HEADER_LEN);

            // **verify keyword bytes "CONF"
            if (payload->magic[0] == 'C' && payload->magic[1] == 'O' &&
                payload->magic[2] == 'N' && payload->magic[3] == 'F')
            {
                char new_ssid[33] = {0};
                char new_pass[65] = {0};

                memcpy(new_ssid, payload->ssid, payload->ssid_len > 32 ? 32 : payload->ssid_len);
                memcpy(new_pass, payload->password, payload->pass_len > 64 ? 64 : payload->pass_len);

                ESP_LOGW(TAG, "Received config frame - new AP SSID: %s", new_ssid);

                save_wifi_credentials(new_ssid, new_pass);

                ESP_LOGI(TAG, "Sending ACK frame to primary module");
                uint8_t ack_frame[18] = {0};                                    // 14 bytes MAC header + 4 bytes "ACK_"
                memset(ack_frame, 0xFF, 6);                                     // Broadcast Dest
                esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, ack_frame + 6);   

                // **set Custom EtherType (0x8901)
                ack_frame[12] = (L2_CONFIG_ETHERTYPE >> 8) & 0xFF;
                ack_frame[13] = (L2_CONFIG_ETHERTYPE & 0xFF);

                // **set "ACK_" keyword payload
                ack_frame[14] = 'A';
                ack_frame[15] = 'C';
                ack_frame[16] = 'K';
                ack_frame[17] = '_';

                esp_eth_transmit(eth_handle, ack_frame, sizeof(ack_frame));

                free(buffer);

                ESP_LOGW(TAG, "Configuration saved - rebooting in 1 second");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();

                return ESP_OK;
            }
        }
    }
    // **added source code end

    esp_err_t ret = ESP_OK;
    flow_control_msg_t msg = {
        .packet = buffer,
        .length = len};
    if (xQueueSend(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGD(TAG, "send flow control message failed or timeout");   // **changed error message to debug message
        free(buffer);
        ret = ESP_FAIL;
    }
    return ret;
}



// **Modified function from the example
// ** - added an early-exit condition for the Wi-Fi transmission retry loop
// ** - if the Wi-Fi stack returns ESP_ERR_WIFI_NOT_ASSOC (12309), it indicates the
// **   destination station has disconnected - by breaking the loop immediately 
// **   instead of waiting for the FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS, we prevent 
// **   stale packets from head-of-line blocking the queue for other connected clients
// This task will fetch the packet from the queue, and then send out through Wi-Fi.
// Wi-Fi handles packets slower than Ethernet, we might add some delay between each transmitting.
static void eth2wifi_flow_control_task(void *args)
{
    flow_control_msg_t msg;
    int res = 0;
    uint32_t timeout = 0;
    while (1)
    {
        if (xQueueReceive(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) == pdTRUE)
        {
            timeout = 0;
            if (s_sta_is_connected && msg.length)
            {
                do
                {
                    vTaskDelay(pdMS_TO_TICKS(timeout));
                    timeout += 2;
                    res = esp_wifi_internal_tx(WIFI_IF_AP, msg.packet, msg.length);

                    // **added source code start:
                    // **12309 = ESP_ERR_WIFI_NOT_ASSOC = client is gone
                    // **break immediately so we don't block the queue for healthy devices
                    if (res == 12309)
                    {
                        break;
                    }
                    // **added source code end
                } while (res && timeout < FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS);
                if (res != ESP_OK)
                {
                    ESP_LOGD(TAG, "WiFi send packet failed: %d", res); // **changed error message to debug message
                }
            }
            free(msg.packet);
        }
    }
    vTaskDelete(NULL);
}



// **Unmodified function from the example
// Event handler for Ethernet
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        s_ethernet_is_connected = true;
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, s_eth_mac);
        esp_wifi_set_mac(WIFI_IF_AP, s_eth_mac);
        ESP_ERROR_CHECK(esp_wifi_start());
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        s_ethernet_is_connected = false;
        ESP_ERROR_CHECK(esp_wifi_stop());
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}



// **Unmodified function from the example
// Event handler for Wi-Fi
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static uint8_t s_con_cnt = 0;
    switch (event_id)
    {
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Wi-Fi AP got a station connected");
        if (!s_con_cnt)
        {
            s_sta_is_connected = true;
            esp_wifi_internal_reg_rxcb(WIFI_IF_AP, pkt_wifi2eth);
        }
        s_con_cnt++;
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "Wi-Fi AP got a station disconnected");
        s_con_cnt--;
        if (!s_con_cnt)
        {
            s_sta_is_connected = false;
            esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
        }
        break;
    default:
        break;
    }
}



// **Unmodified function from the example
static void initialize_ethernet(void)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));
    if (eth_port_cnt > 1)
    {
        ESP_LOGW(TAG, "multiple Ethernet devices detected, the first initialized is to be used!");
    }
    s_eth_handle = eth_handles[0];
    free(eth_handles);
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth_handle, pkt_eth2wifi, NULL));
    bool eth_promiscuous = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &eth_promiscuous));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
}



// **Modified function from the example
// ** - removed hardcoded SSID and Password
// ** - AP credentials are retrieved from NVS
static void initialize_wifi(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .ap = {
            // **removed hardcoded .ssid, .ssid_len, .password
            .max_connection = CONFIG_BRIDGE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .channel = CONFIG_BRIDGE_WIFI_CHANNEL},
    };

    load_wifi_credentials(&wifi_config); // **added call to the NVS function 

    if (strlen((char *)wifi_config.ap.password) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
}



// **Unmodified function from the example
static esp_err_t initialize_flow_control(void)
{
    flow_control_queue = xQueueCreate(FLOW_CONTROL_QUEUE_LENGTH, sizeof(flow_control_msg_t));
    if (!flow_control_queue)
    {
        ESP_LOGE(TAG, "create flow control queue failed");
        return ESP_FAIL;
    }
    BaseType_t ret = xTaskCreate(eth2wifi_flow_control_task, "flow_ctl", 2048, NULL, (tskIDLE_PRIORITY + 2), NULL);
    if (ret != pdTRUE)
    {
        ESP_LOGE(TAG, "create flow control task failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}



// **Modified function from the example
// ** - only renamed the function to make it work with the main.c merger
// ** - body function is left unchanged
void eth2ap_app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(initialize_flow_control());
    initialize_wifi();
    initialize_ethernet();
}
