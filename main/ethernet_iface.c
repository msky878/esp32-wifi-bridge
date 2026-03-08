/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */



/*
**This file is based on "ethernet_iface.c" from the "sta2eth" example project found in the esp-idf:
https://github.com/espressif/esp-idf/blob/release/v5.5/examples/network/sta2eth/main/ethernet_iface.c

**All changes are marked by comments, everything else is left the same as the original project.
**To differentiate my comments from the originals, my comments always start with "**" (2 asterisks)

**Miroslav Michalsky 2026
**ESP32 Wi-Fi Bridge
**sta2eth
*/



#include <string.h>
#include "cc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "wired_iface.h"
#include "dhcpserver/dhcpserver_options.h"
#include "esp_mac.h"
#include "ethernet_init.h"
#include "esp_eth_netif_glue.h"



/**
 *  Disable promiscuous mode on Ethernet interface by setting this macro to 0
 *  if disabled, we'd have to rewrite MAC addressed in frames with the actual Eth interface MAC address
 *  - this results in better throughput
 *  - might cause ARP conflicts if the PC is also connected to the same AP with another NIC
 */
// ** - must be 1 - allows the Ethernet chip to accept packets addressed to the 
// **   Wi-Fi router instead of dropping them at the hardware MAC filter
#define ETH_BRIDGE_PROMISCUOUS 1    // **changed from CONFIG_EXAMPLE_ETHERNET_USE_PROMISCUOUS to 1

/**
 * Set this to 1 to runtime update HW addresses in DHCP messages
 * (this is needed if the client uses 61 option and the DHCP server applies strict rules on assigning addresses)
 */
// ** - must be 1 - modifies the DHCP payload 'chaddr' to match our spoofed Wi-Fi MAC
// ** - without this, strict modern routers will reject the DHCP request and the PC won't get an IP
#define MODIFY_DHCP_MSGS 1  // **changed from CONFIG_EXAMPLE_MODIFY_DHCP_MESSAGES to 1

static const char *TAG = "sta2eth-eth"; // **renamed from example_wired_ethernet
static esp_eth_handle_t s_eth_handle = NULL;
static bool s_ethernet_is_connected = false;
static uint8_t s_eth_mac[6];
static wired_rx_cb_t s_rx_cb = NULL;
static wired_free_cb_t s_free_cb = NULL;



#define L2_CONFIG_ETHERTYPE 0x8901  // **added confguration EtherType for the wired_recv and wired_send_credentials functions

volatile bool s_config_ack_received = false; // **added as a flag for the handshake



// **Unmodified function from the example
/**
 * @brief Event handler for Ethernet events
 */
void eth_event_handler(void *arg, esp_event_base_t event_base,
                       int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    esp_netif_t *netif = (esp_netif_t *)arg;

    switch (event_id)
    {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        if (netif)
        {
            // Start DHCP server only if we "have" the actual netif (provisioning mode)
            // (if netif==NULL we are only forwarding frames, no lwip involved)
            esp_netif_dhcps_start(netif);
        }
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        s_ethernet_is_connected = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        if (netif)
        {
            esp_netif_dhcps_stop(netif);
        }
        s_ethernet_is_connected = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        ESP_LOGI(TAG, "Default Event");
        break;
    }
}



#define IP_V4 0x40
#define IP_PROTO_UDP 0x11
#define DHCP_PORT_IN 0x43
#define DHCP_PORT_OUT 0x44
#define DHCP_MACIG_COOKIE_OFFSET (8 + 236)
#define DHCP_HW_ADDRESS_OFFSET (36)
#define MIN_DHCP_PACKET_SIZE (285)
#define IP_HEADER_SIZE (20)
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_COOKIE_WITH_PKT_TYPE(type) {0x63, 0x82, 0x53, 0x63, 0x35, 1, type};



// **added source code start:

#define MAX_CLIENTS 10            // **maximum number of clients to track for IP/MAC mapping
#define MAC_TABLE_TIMEOUT_SEC 300 // **forget a client after 5 minutes of inactivity
#define MAX_DHCP_PENDING 5        // **max number of concurrent DHCP transactions to track

#define DHCP_XID_OFFSET (12)    // **transaction ID is at offset 4 in DHCP payload, 12 bytes into UDP payload
#define DHCP_PKT_TYPE_OFFSET (DHCP_MACIG_COOKIE_OFFSET + 5) // **offset for DHCP Message Type option
#define DHCP_ACK 5



typedef struct
{
    uint32_t xid;       // **the DHCP Transaction ID
    uint8_t mac[6];     // **the client's real MAC address
    uint32_t last_seen; // **timestamp for cleanup
} dhcp_pending_t;

static dhcp_pending_t s_dhcp_pending_table[MAX_DHCP_PENDING] = {0}; // **table to track ongoing DHCP transactions

// **the client table
typedef struct
{
    uint8_t mac[6];
    uint32_t ip;
    uint32_t last_seen;
} client_entry_t;

static client_entry_t s_client_table[MAX_CLIENTS] = {0};

typedef struct __attribute__((packed))
{
    uint8_t magic[4]; // **"CONF"
    uint8_t ssid_len;
    char ssid[32];
    uint8_t pass_len;
    char password[64];
} config_payload_t;

// **added source code end



#if MODIFY_DHCP_MSGS
// **Unmodified function from the example
static void update_udp_checksum(uint16_t *udp_header, uint16_t *ip_header)
{
    uint32_t sum = 0;
    uint16_t *ptr = udp_header;
    ptr[3] = 0; // clear the current checksum
    int payload_len = htons(ip_header[1]) - IP_HEADER_SIZE;
    // add UDP payload
    for (int i = 0; i < payload_len / 2; i++)
    {
        sum += htons(*ptr++);
    }
    // add the padding if the packet length is odd
    if (payload_len & 1)
    {
        sum += (*((uint8_t *)ptr) << 8);
    }
    // add some IP header data
    ptr = ip_header + 6;
    for (int i = 0; i < 4; i++)
    { // IP addresses
        sum += htons(*ptr++);
    }
    sum += IP_PROTO_UDP + payload_len; // protocol + size
    do
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    } while (sum & 0xFFFF0000); //  process the carry
    ptr = udp_header;
    ptr[3] = htons(~sum); // update the UDP header with the new checksum
}
#endif // MODIFY_DHCP_MSGS



// **Heavily modified function from the example
// **for bi-directional MAC address spoofing and packet inspection
// ** - translates L2 addresses between the wired network and the strict Wi-Fi STA interface
// ** - tracks client MAC and IP mappings using ARP and DHCP inspection
// ** - intercepts DHCP packets to spoof the inner client hardware address (chaddr)
// ** - masks client hostnames in DHCP requests to hide device identities from the upstream router
// ** - uses DHCP Transaction IDs (XID) to route DHCP responses back to the correct client
// ** - restores original destination MACs for incoming packets based on the learned IP table
void mac_spoof(mac_spoof_direction_t direction, uint8_t *buffer, uint16_t len, uint8_t own_mac[6])
{
    uint8_t *dest_mac = buffer;
    uint8_t *src_mac = buffer + 6;
    uint8_t *eth_type = buffer + 12;

    // **direction: Ethernet => Wi-Fi
    if (direction == FROM_WIRED)
    {
        // **learn incoming wired client MACs and manage table timeouts
        int client_idx = -1;
        uint32_t current_time = esp_log_timestamp() / 1000;
        int first_empty_slot = -1;

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (memcmp(s_client_table[i].mac, src_mac, 6) == 0)
            {
                client_idx = i;
                s_client_table[i].last_seen = current_time;
                break;
            }
            if (s_client_table[i].last_seen == 0 && first_empty_slot == -1)
            {
                first_empty_slot = i;
            }
            else if (current_time - s_client_table[i].last_seen > MAC_TABLE_TIMEOUT_SEC)
            {
                memset(&s_client_table[i], 0, sizeof(client_entry_t));
                if (first_empty_slot == -1)
                    first_empty_slot = i;
            }
        }
        if (client_idx == -1 && first_empty_slot != -1)
        {
            ESP_LOGI(TAG, "New client learned: %02x:%02x:%02x:%02x:%02x:%02x",
                     src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
            memcpy(s_client_table[first_empty_slot].mac, src_mac, 6);
            s_client_table[first_empty_slot].last_seen = current_time;
            s_client_table[first_empty_slot].ip = 0;
            client_idx = first_empty_slot;
        }

        // **packet-specific spoofing
        if (eth_type[0] == 0x08)
        { // **is it IP packet?
            if (eth_type[1] == 0x00 && len > 42)
            { // **IPv4
                uint8_t *ip_header = eth_type + 2;
                if ((ip_header[0] & 0xF0) == IP_V4 && ip_header[9] == IP_PROTO_UDP)
                {
                    uint8_t *udp_header = ip_header + IP_HEADER_SIZE;
                    const uint8_t dhcp_ports[] = {0, DHCP_PORT_OUT, 0, DHCP_PORT_IN};
                    if (memcmp(udp_header, dhcp_ports, sizeof(dhcp_ports)) == 0 && len > MIN_DHCP_PACKET_SIZE)
                    {
                        // **intercept DHCP requests to track XID and spoof inner chaddr
                        uint32_t xid;
                        memcpy(&xid, udp_header + DHCP_XID_OFFSET, sizeof(xid));

                        bool found = false;
                        int empty_slot = -1;
                        for (int i = 0; i < MAX_DHCP_PENDING; i++)
                        {
                            if (s_dhcp_pending_table[i].xid == xid)
                            {
                                found = true;
                                break;
                            }
                            if (s_dhcp_pending_table[i].xid == 0 && empty_slot == -1)
                            {
                                empty_slot = i;
                            }
                            if (s_dhcp_pending_table[i].xid != 0 && (current_time - s_dhcp_pending_table[i].last_seen > 60))
                            {
                                s_dhcp_pending_table[i].xid = 0; // Timeout old entries
                                if (empty_slot == -1)
                                    empty_slot = i;
                            }
                        }
                        if (!found && empty_slot != -1)
                        {
                            s_dhcp_pending_table[empty_slot].xid = xid;
                            memcpy(s_dhcp_pending_table[empty_slot].mac, src_mac, 6);
                            s_dhcp_pending_table[empty_slot].last_seen = current_time;
                            ESP_LOGI(TAG, "Tracking DHCP XID: 0x%08lX for MAC %02x:%02x:%02x:%02x:%02x:%02x",
                                     xid, src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
                        }

                        uint8_t *dhcp_client_hw_addr = udp_header + DHCP_HW_ADDRESS_OFFSET;
                        memcpy(dhcp_client_hw_addr, own_mac, 6); // Spoof chaddr

                        // **mask client hostnames (option 12/81) by changing them to ignored private options
                        uint16_t udp_len = ntohs(((uint16_t *)udp_header)[2]);  // **UDP length is at offset 4 in UDP header
                        uint16_t dhcp_len = udp_len - 8; // **subtract UDP header

                        // **check if packet is large enough to contain options
                        if (dhcp_len > DHCP_MACIG_COOKIE_OFFSET + 4)
                        {
                            uint8_t *options = udp_header + DHCP_MACIG_COOKIE_OFFSET + 4;
                            int options_len = dhcp_len - (DHCP_MACIG_COOKIE_OFFSET + 4);
                            int opt_idx = 0;

                            while (opt_idx < options_len && options[opt_idx] != 255)
                            { // **255 is end option
                                if (options[opt_idx] == 0)
                                {
                                    opt_idx++;
                                    continue;
                                }

                                if (opt_idx + 1 < options_len)
                                {
                                    uint8_t opt_len = options[opt_idx + 1];
                                    // **12 = hostname, 81 = client FQDN (fully qualified domain name)
                                    if (options[opt_idx] == 12 || options[opt_idx] == 81)
                                    {
                                        options[opt_idx] = 254; // **change to ignored Private Option
                                        ESP_LOGI(TAG, "Masked client hostname to hide it from router");
                                    }
                                    opt_idx += 2 + opt_len;
                                }
                                else
                                {
                                    break;
                                }
                            }
                        }

                        update_udp_checksum((uint16_t *)udp_header, (uint16_t *)ip_header);
                    }
                }
            }
            else if (eth_type[1] == 0x06 && len >= 42)
            { // **ARP
                // **intercept ARP packets to learn IP-to-MAC mappings and track IP changes
                uint8_t *arp_payload = eth_type + 2;
                uint8_t *arp_sender_mac = arp_payload + 8;
                uint8_t *arp_sender_ip = arp_payload + 14;

                if (client_idx != -1)
                {
                    uint32_t incoming_ip;
                    memcpy(&incoming_ip, arp_sender_ip, 4);

                    // **only log if the IP address is actually new or has changed
                    if (s_client_table[client_idx].ip != incoming_ip)
                    {
                        s_client_table[client_idx].ip = incoming_ip;
                        ESP_LOGI(TAG, "Learned IP %d.%d.%d.%d for client %d",
                                 arp_sender_ip[0], arp_sender_ip[1], arp_sender_ip[2], arp_sender_ip[3], client_idx);
                    }
                }

                memcpy(arp_sender_mac, own_mac, 6); // **spoof sender MAC in ARP payload
            }
        }

        // **rewrite the source MAC in the Ethernet header to match the Wi-Fi interface
        memcpy(src_mac, own_mac, 6);
    }
    // **direction: Wi-Fi => Ethernet
    else if (direction == TO_WIRED)
    {
        if (eth_type[0] == 0x08 && eth_type[1] == 0x00 && len > MIN_DHCP_PACKET_SIZE)
        {
            uint8_t *ip_header = eth_type + 2;
            if ((ip_header[0] & 0xF0) == IP_V4 && ip_header[9] == IP_PROTO_UDP)
            {
                uint8_t *udp_header = ip_header + IP_HEADER_SIZE;
                const uint8_t dhcp_ports[] = {0, DHCP_PORT_IN, 0, DHCP_PORT_OUT};
                if (memcmp(udp_header, dhcp_ports, sizeof(dhcp_ports)) == 0)
                {
                    uint32_t xid;
                    memcpy(&xid, udp_header + DHCP_XID_OFFSET, sizeof(xid));

                    for (int i = 0; i < MAX_DHCP_PENDING; i++)
                    {
                        if (s_dhcp_pending_table[i].xid == xid)
                        {
                            uint8_t *client_mac = s_dhcp_pending_table[i].mac;
                            ESP_LOGI(TAG, "Found matching DHCP XID: 0x%08lX - forwarding to client", xid);

                            memcpy(dest_mac, client_mac, 6);
                            uint8_t *dhcp_client_hw_addr = udp_header + DHCP_HW_ADDRESS_OFFSET;
                            memcpy(dhcp_client_hw_addr, client_mac, 6);
                            update_udp_checksum((uint16_t *)udp_header, (uint16_t *)ip_header);

                            // **if it's the final ACK, the transaction is done
                            uint8_t *pkt_type = udp_header + DHCP_PKT_TYPE_OFFSET;
                            if (*pkt_type == DHCP_ACK)
                            {
                                ESP_LOGI(TAG, "DHCP ACK received - transaction complete");
                                s_dhcp_pending_table[i].xid = 0;
                            }
                            return;
                        }
                    }
                }
            }
        }

        // **restore the destination MAC for general unicast traffic using the learned IP table
        if (memcmp(dest_mac, own_mac, 6) == 0)
        {
            bool forwarded = false;
            if (eth_type[0] == 0x08 && eth_type[1] == 0x00 && len > 42)
            { // IPv4
                uint8_t *ip_header = eth_type + 2;
                uint32_t dest_ip;
                memcpy(&dest_ip, ip_header + 16, 4); // **get destination IP from IP header

                for (int i = 0; i < MAX_CLIENTS; i++)
                {
                    if (s_client_table[i].ip == dest_ip)
                    {
                        memcpy(dest_mac, s_client_table[i].mac, 6);
                        forwarded = true;
                        break;
                    }
                }
            }

            // **fallback to broadcast if the destination IP is unknown
            if (!forwarded)
            {
                ESP_LOGD(TAG, "Unknown destination, broadcasting packet.");
                memset(dest_mac, 0xFF, 6);
            }
        }
    }
}



// **Modified function from the example
// ** - added handler for ACK handshake
static esp_err_t wired_recv(esp_eth_handle_t eth_handle, uint8_t *buffer, uint32_t len, void *priv)
{
    // **added source code start:
    if (len >= 18)  // **check if packet is large enough for header (14) + "ACK_" (4)
    {
        uint16_t eth_type = (buffer[12] << 8) | buffer[13];
        if (eth_type == L2_CONFIG_ETHERTYPE)
        {
            if (buffer[14] == 'A' && buffer[15] == 'C' && buffer[16] == 'K' && buffer[17] == '_')
            {
                s_config_ack_received = true;
                ESP_LOGI(TAG, "Received configuration ACK from secondary module");
                free(buffer);
                return ESP_OK; // **drop the packet - don't pass it to the bridge
            }
        }
    }
    // **added source code end

    esp_err_t ret = s_rx_cb(buffer, len, buffer);
    free(buffer);
    return ret;
}



// **Unmodified function from the example
esp_err_t wired_bridge_init(wired_rx_cb_t rx_cb, wired_free_cb_t free_cb)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));

    // Check for multiple Ethernet interfaces
    if (1 < eth_port_cnt)
    {
        ESP_LOGW(TAG, "Multiple Ethernet Interface detected: Only the first initialized interface is going to be used.");
    }
    s_eth_handle = eth_handles[0];
    free(eth_handles);
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth_handle, wired_recv, NULL));
#if ETH_BRIDGE_PROMISCUOUS
    bool eth_promiscuous = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &eth_promiscuous));
#endif
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, &s_eth_mac));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
    s_rx_cb = rx_cb;
    s_free_cb = free_cb;
    return ESP_OK;
}



// **Modified function from the example
// ** - changed error message to debug message
esp_err_t wired_send(void *buffer, uint16_t len, void *buff_free_arg)
{
    if (s_ethernet_is_connected)
    {
        if (esp_eth_transmit(s_eth_handle, buffer, len) != ESP_OK)
        {
            ESP_LOGD(TAG, "Ethernet send packet failed (Buffer full)");
            return ESP_FAIL;
        }
        if (s_free_cb)
        {
            s_free_cb(buff_free_arg, NULL);
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}



// **Newly added function
// **transmits Wi-Fi credentials to the secondary module via a raw Ethernet packet
// ** 1. allocates DMA-capable memory for the custom Layer 2 frame (header + payload)
// ** 2. constructs the Ethernet header using a broadcast destination MAC and a custom EtherType
// ** 3. builds the payload by injecting a "CONF" magic identifier along with the SSID and Password
// ** 4. transmits the frame directly over the wire and frees the allocated buffer
// ** - this allows the primary module to securely pass configuration data even if neither device has an IP address yet
esp_err_t wired_send_credentials(const char *ssid, const char *password)
{
    if (!s_eth_handle)
    {
        ESP_LOGE(TAG, "Ethernet not initialized, cannot send L2 config");
        return ESP_FAIL;
    }

    // **prepare buffer
    // **size = Eth header (14) + payload
    uint16_t payload_len = sizeof(config_payload_t);
    uint16_t total_len = ETH_HEADER_LEN + payload_len;

    uint8_t *buffer = heap_caps_malloc(total_len, MALLOC_CAP_DMA);
    if (!buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for L2 config packet");
        return ESP_ERR_NO_MEM;
    }
    memset(buffer, 0, total_len);

    // **construct Ethernet Header
    uint8_t *dest_mac = buffer;
    uint8_t *src_mac = buffer + 6;
    uint8_t *eth_type = buffer + 12;

    // ** - for the destination we use broadcast (FF:FF:FF:FF:FF:FF)
    // ** - this way if we have multiple secondary modules, they all get the configuration (not tested)
    memset(dest_mac, 0xFF, 6);

    memcpy(src_mac, s_eth_mac, 6);

    // **set Custom EtherType (0x8901)
    eth_type[0] = (L2_CONFIG_ETHERTYPE >> 8) & 0xFF;
    eth_type[1] = (L2_CONFIG_ETHERTYPE & 0xFF);

    config_payload_t *payload = (config_payload_t *)(buffer + ETH_HEADER_LEN);

    // **set "CONF" keyword payload
    payload->magic[0] = 'C';
    payload->magic[1] = 'O';
    payload->magic[2] = 'N';
    payload->magic[3] = 'F';

    payload->ssid_len = (uint8_t)strlen(ssid);
    if (payload->ssid_len > 32)
        payload->ssid_len = 32;
    memcpy(payload->ssid, ssid, payload->ssid_len);

    payload->pass_len = (uint8_t)strlen(password);
    if (payload->pass_len > 64)
        payload->pass_len = 64;
    memcpy(payload->password, password, payload->pass_len);

    ESP_LOGI(TAG, "Sending L2 Config Frame - SSID: %s", ssid);

    esp_err_t ret = esp_eth_transmit(s_eth_handle, buffer, total_len);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send L2 config packet");
    }

    free(buffer);
    return ret;
}
