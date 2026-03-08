/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
**This file is based on "manual_config.c" from the "sta2eth" example project found in the esp-idf:
https://github.com/espressif/esp-idf/blob/release/v5.5/examples/network/sta2eth/main/manual_config.c

**All changes are marked by comments, everything else is left the same as the original project.
**To differentiate my comments from the originals, my comments always start with "**" (2 asterisks)

**Miroslav Michalsky 2026
**ESP32 Wi-Fi Bridge
**sta2eth
*/

#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include "wired_iface.h"

static const char *TAG = "sta2eth-conf"; // **renamed from AP_Config
static httpd_handle_t s_web_server = NULL;

extern volatile bool s_config_ack_received; // **flag for the handshake

// **Unmodified function from the example
bool is_provisioned(void)
{
    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK)
    {
        return false;
    }
    if (strlen((const char *)wifi_cfg.sta.ssid))
    {
        return true;
    }
    return false;
}

// **Modified function from the example
static esp_err_t http_get_handler(httpd_req_t *req)
{
    // **modified the HTML/JS code
    const char page[] =
        "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head>"
        "<body>"
        "<h2>ESP32 Bridge Setup</h2>"
        "<form action=\"/\" method=\"get\">"
        "<h3>1. Connect Bridge to Home WiFi</h3>"
        "Home SSID: <input type=\"text\" name=\"ssid\"><br><br>"
        "Home Pass: <input type=\"text\" name=\"password\"><br><hr>"

        "<h3>2. Secondary Device Configuration</h3>"
        "<input type=\"checkbox\" id=\"cust\" name=\"custom\" value=\"1\" onclick=\"toggle()\">"
        "<label for=\"cust\"> Use different WiFi for Secondary AP</label><br><br>"

        "<div id=\"extra\" style=\"display:none\">"
        "Target AP SSID: <input type=\"text\" name=\"ap_ssid\"><br><br>"
        "Target AP Pass: <input type=\"text\" name=\"ap_pass\"><br><br>"
        "</div>"

        "<script>"
        "function toggle() {"
        "  var x = document.getElementById('extra');"
        "  if (document.getElementById('cust').checked) { x.style.display = 'block'; }"
        "  else { x.style.display = 'none'; }"
        "}"
        "</script>"

        "<hr><input type=\"submit\" value=\"Save & Sync Devices\">"
        "</form></body></html>";

    char *buf = NULL;
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;

    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            // **added more fields
            char sta_ssid[33] = {0};
            char sta_pass[65] = {0};
            char eth_ssid[33] = {0};
            char eth_pass[65] = {0};
            char is_custom[4] = {0};

            // **home (source) Wi-Fi credentials
            if (httpd_query_key_value(buf, "ssid", sta_ssid, sizeof(sta_ssid)) == ESP_OK &&
                httpd_query_key_value(buf, "password", sta_pass, sizeof(sta_pass)) == ESP_OK)
            {
                // **custom AP credentials or mirroring
                if (httpd_query_key_value(buf, "custom", is_custom, sizeof(is_custom)) == ESP_OK &&
                    strcmp(is_custom, "1") == 0)
                {
                    ESP_LOGI(TAG, "User selected custom AP credentials");
                    httpd_query_key_value(buf, "ap_ssid", eth_ssid, sizeof(eth_ssid));
                    httpd_query_key_value(buf, "ap_pass", eth_pass, sizeof(eth_pass));
                }
                else
                {
                    ESP_LOGI(TAG, "User selected mirror credentials");
                    strcpy(eth_ssid, sta_ssid);
                    strcpy(eth_pass, sta_pass);
                }

                // **modified response/after submit + added syncing with the secondary module
                if (strlen(sta_ssid) > 0)
                {
                    const char resp[] = "<h1>Settings Saved. Syncing & Rebooting...</h1>"
                                        "<p>Note: If the secondary module is unreachable, "
                                        "only the primary bridge connection will be updated.</p>";
                    httpd_resp_send(req, resp, strlen(resp));

                    // **give the network stack time to send the cofirmation page
                    ESP_LOGI(TAG, "Page sent - waiting for TCP flush before killing Wi-Fi");
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    ESP_LOGI(TAG, "Stopping STA to apply new config");
                    esp_wifi_stop(); // **kills both AP and STA, but ensures the state is 'INIT'
                    vTaskDelay(pdMS_TO_TICKS(500));

                    wifi_config_t wifi_cfg = {0};
                    // **since we stopped wifi, we manually fill the struct
                    strncpy((char *)wifi_cfg.sta.ssid, sta_ssid, sizeof(wifi_cfg.sta.ssid));
                    strncpy((char *)wifi_cfg.sta.password, sta_pass, sizeof(wifi_cfg.sta.password));

                    ESP_LOGI(TAG, "Saving Local Config: SSID=%s", wifi_cfg.sta.ssid);

                    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

                    if (err == ESP_OK) // **should always return ESP_OK because Wi-Fi is stopped
                    {
                        ESP_LOGI(TAG, "Syncing to secondary module: SSID=%s", eth_ssid);
                        s_config_ack_received = false;
                        int max_retries = 5;
                        bool sync_success = false; // **for ACK retry logic

                        while (max_retries > 0 && !sync_success)
                        {
                            wired_send_credentials(eth_ssid, eth_pass);

                            // Wait up to 500ms for the ACK, checking every 50ms
                            int wait_ms = 0;
                            while (wait_ms < 500)
                            {
                                if (s_config_ack_received)
                                {
                                    sync_success = true;
                                    break;
                                }
                                vTaskDelay(pdMS_TO_TICKS(50));
                                wait_ms += 50;
                            }

                            if (!sync_success)
                            {
                                ESP_LOGW(TAG, "No ACK received - retrying... (%d attempts left)", max_retries - 1);
                                max_retries--;
                            }
                        }

                        if (sync_success)
                        {
                            ESP_LOGI(TAG, "Secondary module successfully acknowledged config!");
                        }
                        else
                        {
                            ESP_LOGE(TAG, "Failed to get ACK from secondary module - rebooting anyway...");
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to set config: %s", esp_err_to_name(err));
                    }

                    free(buf);
                    ESP_LOGI(TAG, "Rebooting...");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();

                    return ESP_OK;
                }
            }
        }
        free(buf);
    }

    httpd_resp_send(req, page, strlen(page));
    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = http_get_handler,
};

// **Modified function from the example
// **that starts the webserver, accesible on http://192.168.4.1/
// ** - removed and added some info messages
static void start_webserver(void)
{
    if (s_web_server != NULL) // **added check
        return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.stack_size = 8192; // **added stack size

    // Start the httpd server
    if (httpd_start(&s_web_server, &config) == ESP_OK)
    {
        // Set URI handlers
        httpd_register_uri_handler(s_web_server, &root);
        ESP_LOGI(TAG, "Web Server Started");
    }
}

// **Newly added function
// **configures the configuration softAP using values from menuconfig
// ** - automatically switches to an open network if the password is left blank
static void configure_softap(void)
{
    wifi_config_t wifi_ap_config = {
        .ap = {
            .max_connection = 4,
            .channel = 1,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK // Default to secure
        },
    };

    strncpy((char *)wifi_ap_config.ap.ssid, CONFIG_PRIMARY_AP_SSID, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(CONFIG_PRIMARY_AP_SSID);

    if (strlen(CONFIG_PRIMARY_AP_PASSWORD) == 0)
    {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        strncpy((char *)wifi_ap_config.ap.password, CONFIG_PRIMARY_AP_PASSWORD, sizeof(wifi_ap_config.ap.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI(TAG, "SoftAP configured - SSID: %s PASS: %s",
             CONFIG_PRIMARY_AP_SSID,
             strlen(CONFIG_PRIMARY_AP_PASSWORD) > 0 ? CONFIG_PRIMARY_AP_PASSWORD : "(Open Network)");
}

// **Newly added function
void start_config_services(void)
{
    configure_softap();
    start_webserver();
}