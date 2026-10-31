#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
struct esp_partition_t { int x; }; struct esp_app_desc_t { char version[32]; };
const esp_partition_t* esp_ota_get_running_partition(); const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
esp_err_t esp_ota_get_partition_description(const esp_partition_t*, esp_app_desc_t*); esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
