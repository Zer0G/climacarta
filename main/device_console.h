#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t device_console_start(void);
bool device_console_take_provision_request(void);
