# **Base source files that BOTH modules need
set(srcs "main.c")

# **Add files based on the menuconfig selection
if(CONFIG_BUILD_STA2ETH)
    list(APPEND srcs 
        "sta2eth_main.c" 
        "manual_config.c" 
        "ethernet_iface.c"
    )
elseif(CONFIG_BUILD_ETH2AP)
    list(APPEND srcs 
        "eth2ap_main.c"
    )
endif()

idf_component_register(SRCS "${srcs}"
                       PRIV_REQUIRES esp_wifi esp_eth nvs_flash ethernet_init esp_http_server esp_timer esp_driver_gpio lwip
                       INCLUDE_DIRS ".")
