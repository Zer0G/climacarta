#pragma once

#include <stddef.h>

#include "esp_err.h"

#define OTA_VERSION_MAX_SIZE 64

typedef void (*ota_progress_cb_t)(int downloaded, int total);

esp_err_t ota_confirm_running_image(void);
esp_err_t ota_get_latest_version(char *version, size_t version_size);
esp_err_t ota_install_from_github(ota_progress_cb_t progress_callback);
