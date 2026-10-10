#pragma once

#include "sdkconfig.h"

/* A build target maps to one physical Waveshare product. Display technology,
 * GPIO routing and OTA compatibility are therefore compile-time properties. */
#if CONFIG_IDF_TARGET_ESP32C6

#define BOARD_MODEL_NAME "Waveshare ESP32-C6-ePaper-1.54"
#define BOARD_OTA_ID "waveshare-esp32-c6-epaper-1.54"
#define BOARD_DISPLAY_NAME "white/black"
#define BOARD_DISPLAY_IS_FOUR_COLOR 0
#define BOARD_USES_IO_EXPANDER 1

#define BOARD_BUTTON_BOOT_GPIO  9
#define BOARD_BUTTON_POWER_GPIO 2
#define BOARD_BOOT_CAN_WAKE     0
#define BOARD_I2C_SDA_GPIO      18
#define BOARD_I2C_SCL_GPIO      8
#define BOARD_BATTERY_ADC_CHAN  ADC_CHANNEL_0

#define BOARD_EPD_MOSI_GPIO       5
#define BOARD_EPD_SCLK_GPIO       6
#define BOARD_EPD_CS_GPIO         7
#define BOARD_EPD_BUSY_GPIO       10
#define BOARD_EPD_RESET_GPIO      11
#define BOARD_EPD_DC_GPIO         15
#define BOARD_EPD_BUSY_READY_LEVEL 0

#elif CONFIG_IDF_TARGET_ESP32S3

#define BOARD_MODEL_NAME "Waveshare ESP32-S3-ePaper-1.54G"
#define BOARD_OTA_ID "waveshare-esp32-s3-epaper-1.54g"
#define BOARD_DISPLAY_NAME "white/red/yellow/black"
#define BOARD_DISPLAY_IS_FOUR_COLOR 1
#define BOARD_USES_IO_EXPANDER 0

#define BOARD_BUTTON_BOOT_GPIO    0
#define BOARD_BUTTON_POWER_GPIO   18
#define BOARD_BOOT_CAN_WAKE       1
#define BOARD_GREEN_LED_GPIO      3
#define BOARD_GREEN_LED_ON_LEVEL  0
#define BOARD_BATTERY_HOLD_GPIO   17
#define BOARD_EPD_POWER_GPIO      6
#define BOARD_AUDIO_POWER_GPIO    42
#define BOARD_I2C_SDA_GPIO        47
#define BOARD_I2C_SCL_GPIO        48
#define BOARD_BATTERY_ADC_CHAN    ADC_CHANNEL_3

#define BOARD_EPD_MOSI_GPIO       13
#define BOARD_EPD_SCLK_GPIO       12
#define BOARD_EPD_CS_GPIO         11
#define BOARD_EPD_BUSY_GPIO       8
#define BOARD_EPD_RESET_GPIO      9
#define BOARD_EPD_DC_GPIO         10
#define BOARD_EPD_BUSY_READY_LEVEL 1
#define BOARD_AUDIO_POWER_ON_LEVEL 0

#else
#error "ClimaCarta supports only esp32c6 and esp32s3 targets"
#endif
