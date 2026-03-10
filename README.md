# ESP32 Wi-Fi Bridge

This project provides a complete, dual-module Wi-Fi bridging solution using two ESP32 devices connected via Ethernet. It allows you to transparently bridge a source Wi-Fi network (Station) to a new broadcasted Wi-Fi network (Access Point) over a high-speed wired Ethernet backbone.

This project was developed as part of a Bachelor's Thesis by Miroslav Michalsky at VSB-TUO (2026).

## Features

* Layer 2 MAC Spoofing: Seamlessly bridges networks by spoofing MAC addresses, bypassing strict Wi-Fi Station (STA) limitations.
* Deep Packet Inspection (DHCP): Modifies internal DHCP payloads (`chaddr`) and masks client hostnames (Options 12 & 81) to ensure compatibility with strict modern routers.
* Unified Codebase: A single ESP-IDF project that can compile into either the Primary or Secondary module firmware via `menuconfig`.
* Web-Based Configuration: The Primary module hosts a captive SoftAP and a web portal for easy network setup.
* Automated L2 Syncing: The Primary module synchronizes Wi-Fi credentials to the Secondary module over the Ethernet wire using a custom Layer 2 protocol (EtherType `0x8901`).
* Factory Reset Button: Configurable GPIO support for physical device resets *(not tested).*

## Architecture

The system requires at least two ESP32 modules with Ethernet capabilities (tested on the **WT32-ETH01**).

1. Primary Module (`sta2eth`): Connects wirelessly to your home/source Wi-Fi router. It passes this traffic down the physical Ethernet cable.
2. Secondary Module (`eth2ap`): Receives the internet traffic via Ethernet and broadcasts it as a new Wi-Fi Access Point for local devices to connect to.

*(Note: The system architecture theoretically supports multiple secondary modules connected via an Ethernet switch, though it has only been explicitly tested with a 1-to-1 connection).*

## Performance
Tested on two WT32-ETH01 modules with 1 connected device:
* **Download:** ~17 MB/s
* **Upload:** ~3 MB/s

## Hardware & Setup Prerequisites

* **Framework:** ESP-IDF v5.5
* **Hardware:** 2x WT32-ETH01 (or similar ESP32 boards with Ethernet PHY)
* **Connection:** A standard Ethernet cable linking the two boards.

## Installation & Flashing

1. Source your ESP-IDF environment:
    ```bash
    /esp-idf/export.sh

2. Navigate to the project folder and open the configuration menu:
    ```bash
    idf.py menuconfig

3. Configure the Hardware Role:

    Navigate to ESP32 WiFi Bridge Configuration -> Select Device-Role to Build.
    Choose either Primary Module or Secondary Module.

4. Configure Ethernet (For WT32-ETH01):

    Go to Component config -> Ethernet -> Support ESP32 internal EMAC controller.

    Set the following parameters for the LAN8720 PHY:

    * Ethernet PHY Device: LAN87xx

    * SMI MDC GPIO: 23

    * SMI MDIO GPIO: 18

    * PHY Reset GPIO: 16

    * PHY Address: 1

5. Build and flash the modules:
    ```bash
    idf.py build flash monitor

## Usage / Configuration

Power on both modules connected by an Ethernet cable.

The Primary Module will broadcast a setup Wi-Fi network (default SSID: espmoduleconfig, Password: 12345678).

Connect your phone or PC to this setup network.

Open a web browser and navigate to http://192.168.4.1.

Enter your home Wi-Fi credentials (to give the bridge internet access) and configure the target AP credentials (the network the Secondary module will broadcast).

Click **Save & Sync**. The Primary module will automatically pass the new credentials to the Secondary module over the Ethernet cable. Both devices will reboot and the bridge will go live.

## Acknowledgements & Legal

Parts of this codebase are heavily modified versions of the sta2eth and eth2ap examples provided in the official Espressif ESP-IDF repository.

All custom modifications, L2 tracking logic, and deep-packet inspection additions are explicitly marked in the source code with comments starting with ** (two asterisks) to differentiate them from the original Espressif foundation code.

License: This project is provided under the Unlicense / CC0-1.0 (inheriting the original Espressif example licensing).
