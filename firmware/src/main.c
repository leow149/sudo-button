#include "esp_log.h"
#include "hw.h"
#include "nvs_flash.h"
#include "proto.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    ESP_ERROR_CHECK(hw_init());
    ui_start(proto_result, proto_forget);
    proto_init();
    ESP_LOGI(TAG, "sudo-button up");
}
