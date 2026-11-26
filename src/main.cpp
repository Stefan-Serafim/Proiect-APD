#include <cstdio>

#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"

extern "C" void app_main() {
    ESP_LOGI("apd", "Node %c", APD_NODE_ID == 1 ? 'A' : 'B');

    esp_vfs_littlefs_conf_t config = {};
    config.base_path = "/littlefs";
    config.partition_label = "littlefs";

    const esp_err_t result = esp_vfs_littlefs_register(&config);
    if (result != ESP_OK) {
        ESP_LOGE("apd", "LittleFS mount failed: %s", esp_err_to_name(result));
        return;
    }

    std::FILE *status = std::fopen("/littlefs/www/status.txt", "r");
    if (!status) {
        ESP_LOGE("apd", "Could not open /littlefs/www/status.txt");
        return;
    }

    char line[80];
    if (std::fgets(line, sizeof(line), status)) {
        std::printf("%s", line);
    } else {
        ESP_LOGE("apd", "Could not read /littlefs/www/status.txt");
    }
    std::fclose(status);
}
