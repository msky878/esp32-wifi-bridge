/*
**Miroslav Michalsky 2026
**ESP32 Wi-Fi Bridge
*/

#include "sdkconfig.h"
#include "esp_log.h"

extern void sta2eth_app_main(void);
extern void eth2ap_app_main(void);

/*
**Use the idf.py menuconfig to select which software to use during the build
*/
void app_main(void)
{
#ifdef CONFIG_BUILD_STA2ETH
    ESP_LOGI("BOOT", "Starting Primary Module (STA2ETH)");
    sta2eth_app_main();
#elif defined(CONFIG_BUILD_ETH2AP)
    ESP_LOGI("BOOT", "Starting Secondary Module (ETH2AP)");
    eth2ap_app_main();
#else
    #error "No device role selected - run idf.py menuconfig"
#endif
}
