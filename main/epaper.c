#include "epaper.h"
#include "font_share_tech_mono.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qrcode.h"

#define EPD_WIDTH             200
#define EPD_HEIGHT            200
#define EPD_MONO_BUFFER_SIZE  (EPD_WIDTH * EPD_HEIGHT / 8)
#define EPD_COLOR_BUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 4)
#define EPD_SPI_HOST    SPI2_HOST
#define EPD_BUSY_POLL_MS 10
#define EPD_MONO_BUSY_TIMEOUT_MS 30000
#define EPD_COLOR_BUSY_TIMEOUT_MS 60000
#define EPD_COLOR_BUSY_ASSERT_TIMEOUT_MS 1000

static const char *TAG = "epaper";
static spi_device_handle_t s_spi;
static uint8_t s_framebuffer[EPD_COLOR_BUFFER_SIZE];
static bool s_ota_progress_started;

typedef enum {
    EPD_COLOR_BLACK = 0,
    EPD_COLOR_WHITE = 1,
    EPD_COLOR_YELLOW = 2,
    EPD_COLOR_RED = 3,
} epd_color_t;

/* Waveform supplied for the 200x200 panel used by the Waveshare board. */
static const uint8_t s_full_lut[159] = {
    0x80,0x48,0x40,0,0,0,0,0,0,0,0,0, 0x40,0x48,0x80,0,0,0,0,0,0,0,0,0,
    0x80,0x48,0x40,0,0,0,0,0,0,0,0,0, 0x40,0x48,0x80,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0x0a, 0,0,0,0,0,0,0x08,0x01,0,0x08,0x01,0,0x02,
    0x0a,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0x22,0x22,0x22,0x22,0x22,0x22,0,
    0,0,0x22,0x17,0x41,0,0x32,0x20,
};

static esp_err_t spi_write(bool command, const void *data, size_t length)
{
    gpio_set_level(BOARD_EPD_DC_GPIO, command ? 0 : 1);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t send_command(uint8_t command)
{
    return spi_write(true, &command, 1);
}

static esp_err_t send_data(const void *data, size_t length)
{
    return spi_write(false, data, length);
}

static esp_err_t send_byte(uint8_t value)
{
    return send_data(&value, 1);
}

static esp_err_t wait_ready(void)
{
    const int timeout_ms = BOARD_DISPLAY_IS_FOUR_COLOR ?
        EPD_COLOR_BUSY_TIMEOUT_MS : EPD_MONO_BUSY_TIMEOUT_MS;
    for (int elapsed_ms = 0; elapsed_ms < timeout_ms;
         elapsed_ms += EPD_BUSY_POLL_MS) {
        if (gpio_get_level(BOARD_EPD_BUSY_GPIO) == BOARD_EPD_BUSY_READY_LEVEL) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(EPD_BUSY_POLL_MS));
    }
    ESP_LOGE(TAG, "display busy timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_four_color_busy_cycle(const char *operation)
{
    /* JD79660AA BUSY_N transitions LOW after power/refresh commands. Wait for
     * that assertion before waiting for idle HIGH, so the old idle level cannot
     * be mistaken for completion and no arbitrary fixed delay is required. */
    for (int elapsed_ms = 0; elapsed_ms < EPD_COLOR_BUSY_ASSERT_TIMEOUT_MS;
         elapsed_ms += EPD_BUSY_POLL_MS) {
        if (gpio_get_level(BOARD_EPD_BUSY_GPIO) == 0) {
            ESP_LOGI(TAG, "%s: BUSY asserted after %d ms", operation, elapsed_ms);
            const esp_err_t result = wait_ready();
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "%s: display ready", operation);
            }
            return result;
        }
        vTaskDelay(pdMS_TO_TICKS(EPD_BUSY_POLL_MS));
    }
    ESP_LOGE(TAG, "%s did not assert display BUSY", operation);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t set_window_and_cursor(void)
{
    const uint8_t x_range[] = {0, (EPD_WIDTH - 1) >> 3};
    const uint8_t y_range[] = {EPD_HEIGHT - 1, 0, 0, 0};
    const uint8_t cursor_x = 0;
    const uint8_t cursor_y[] = {EPD_HEIGHT - 1, 0};
    ESP_RETURN_ON_ERROR(send_command(0x44), TAG, "x window cmd");
    ESP_RETURN_ON_ERROR(send_data(x_range, sizeof(x_range)), TAG, "x window");
    ESP_RETURN_ON_ERROR(send_command(0x45), TAG, "y window cmd");
    ESP_RETURN_ON_ERROR(send_data(y_range, sizeof(y_range)), TAG, "y window");
    ESP_RETURN_ON_ERROR(send_command(0x4e), TAG, "x cursor cmd");
    ESP_RETURN_ON_ERROR(send_byte(cursor_x), TAG, "x cursor");
    ESP_RETURN_ON_ERROR(send_command(0x4f), TAG, "y cursor cmd");
    return send_data(cursor_y, sizeof(cursor_y));
}

static esp_err_t load_lut(void)
{
    ESP_RETURN_ON_ERROR(send_command(0x32), TAG, "lut cmd");
    ESP_RETURN_ON_ERROR(send_data(s_full_lut, 153), TAG, "lut");
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "lut busy");
    ESP_RETURN_ON_ERROR(send_command(0x3f), TAG, "gate cmd");
    ESP_RETURN_ON_ERROR(send_byte(s_full_lut[153]), TAG, "gate");
    ESP_RETURN_ON_ERROR(send_command(0x03), TAG, "voltage cmd");
    ESP_RETURN_ON_ERROR(send_byte(s_full_lut[154]), TAG, "voltage");
    ESP_RETURN_ON_ERROR(send_command(0x04), TAG, "source voltage cmd");
    ESP_RETURN_ON_ERROR(send_data(&s_full_lut[155], 3), TAG, "source voltage");
    ESP_RETURN_ON_ERROR(send_command(0x2c), TAG, "vcom cmd");
    return send_byte(s_full_lut[158]);
}

static esp_err_t activate_full_refresh(void)
{
    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "update cmd");
    ESP_RETURN_ON_ERROR(send_byte(0xc7), TAG, "update mode");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "display update");
    return wait_ready();
}

static esp_err_t activate_partial_refresh(void)
{
    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "partial update cmd");
    ESP_RETURN_ON_ERROR(send_byte(0xff), TAG, "partial update mode");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "partial display update");
    return wait_ready();
}

static esp_err_t clear_panel_to_white(void)
{
    memset(s_framebuffer, 0xff, EPD_MONO_BUFFER_SIZE);
    ESP_RETURN_ON_ERROR(set_window_and_cursor(), TAG, "clear cursor");

    /* Initialise both controller RAM planes as done by Waveshare's
     * DisplayPartBaseImage sequence. After EPD power cycling the old-image
     * plane is otherwise undefined and can leave visible remnants. */
    ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "clear new RAM cmd");
    ESP_RETURN_ON_ERROR(send_data(s_framebuffer, EPD_MONO_BUFFER_SIZE), TAG,
                        "clear new RAM");
    ESP_RETURN_ON_ERROR(send_command(0x26), TAG, "clear old RAM cmd");
    ESP_RETURN_ON_ERROR(send_data(s_framebuffer, EPD_MONO_BUFFER_SIZE), TAG,
                        "clear old RAM");
    return activate_full_refresh();
}

static esp_err_t mono_controller_init(void)
{
    gpio_set_level(BOARD_EPD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BOARD_EPD_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_EPD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "reset busy");
    ESP_RETURN_ON_ERROR(send_command(0x12), TAG, "soft reset");
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "soft reset busy");

    const uint8_t driver[] = {0xc7, 0x00, 0x01};
    ESP_RETURN_ON_ERROR(send_command(0x01), TAG, "driver cmd");
    ESP_RETURN_ON_ERROR(send_data(driver, sizeof(driver)), TAG, "driver");
    ESP_RETURN_ON_ERROR(send_command(0x11), TAG, "entry mode cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x01), TAG, "entry mode");
    ESP_RETURN_ON_ERROR(set_window_and_cursor(), TAG, "RAM geometry");
    ESP_RETURN_ON_ERROR(send_command(0x3c), TAG, "border cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x01), TAG, "border");
    ESP_RETURN_ON_ERROR(send_command(0x18), TAG, "temp sensor cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x80), TAG, "temp sensor");
    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "waveform cmd");
    ESP_RETURN_ON_ERROR(send_byte(0xb1), TAG, "waveform");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "activate waveform");
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "waveform busy");
    return load_lut();
}

/* JD79660 initialization for the 1.54-inch 200x200 four-color panel. */
static esp_err_t four_color_controller_init(void)
{
    gpio_set_level(BOARD_EPD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(BOARD_EPD_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_EPD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "four-color reset busy");

    ESP_RETURN_ON_ERROR(send_command(0x4d), TAG, "four-color config cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x78), TAG, "four-color config");
    const uint8_t panel_setting[] = {0x0f, 0x29};
    ESP_RETURN_ON_ERROR(send_command(0x00), TAG, "four-color panel cmd");
    ESP_RETURN_ON_ERROR(send_data(panel_setting, sizeof(panel_setting)), TAG,
                        "four-color panel");
    const uint8_t booster[] = {0x0d, 0x12, 0x30, 0x20, 0x19, 0x2a, 0x22};
    ESP_RETURN_ON_ERROR(send_command(0x06), TAG, "four-color booster cmd");
    ESP_RETURN_ON_ERROR(send_data(booster, sizeof(booster)), TAG,
                        "four-color booster");
    ESP_RETURN_ON_ERROR(send_command(0x50), TAG, "four-color interval cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x37), TAG, "four-color interval");
    const uint8_t resolution[] = {0x00, EPD_WIDTH, 0x00, EPD_HEIGHT};
    ESP_RETURN_ON_ERROR(send_command(0x61), TAG, "four-color resolution cmd");
    ESP_RETURN_ON_ERROR(send_data(resolution, sizeof(resolution)), TAG,
                        "four-color resolution");
    ESP_RETURN_ON_ERROR(send_command(0xe9), TAG, "four-color cascade cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x01), TAG, "four-color cascade");
    ESP_RETURN_ON_ERROR(send_command(0x30), TAG, "four-color PLL cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x08), TAG, "four-color PLL");
    ESP_RETURN_ON_ERROR(send_command(0x04), TAG, "four-color power on");
    return wait_four_color_busy_cycle("power on");
}

static void clear_framebuffer(void)
{
    memset(s_framebuffer, BOARD_DISPLAY_IS_FOUR_COLOR ? 0x55 : 0xff,
           BOARD_DISPLAY_IS_FOUR_COLOR ? EPD_COLOR_BUFFER_SIZE :
                                         EPD_MONO_BUFFER_SIZE);
}

static void pixel_color(int x, int y, epd_color_t color)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return;
    }
    if (BOARD_DISPLAY_IS_FOUR_COLOR) {
        const size_t index = (size_t)y * (EPD_WIDTH / 4) + (size_t)(x >> 2);
        const unsigned shift = 6U - 2U * (unsigned)(x & 3);
        const uint8_t mask = (uint8_t)(0x03U << shift);
        s_framebuffer[index] = (uint8_t)((s_framebuffer[index] & ~mask) |
                                         ((uint8_t)color << shift));
        return;
    }
    const size_t index = (size_t)y * (EPD_WIDTH / 8) + (size_t)(x >> 3);
    const uint8_t mask = (uint8_t)(0x80U >> (x & 7));
    if (color == EPD_COLOR_BLACK) {
        s_framebuffer[index] &= (uint8_t)~mask;
    } else {
        s_framebuffer[index] |= mask;
    }
}

static void pixel(int x, int y, bool black)
{
    pixel_color(x, y, black ? EPD_COLOR_BLACK : EPD_COLOR_WHITE);
}

static void text_with_font_color_tracking(int x, int y, const char *value,
                                          const bitmap_font_t *font,
                                          epd_color_t color, int tracking)
{
    if (!value || !font) {
        return;
    }
    const size_t glyph_size = (size_t)font->stride * font->height;

    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor) {
        size_t glyph_index = 0;
        if (cursor[0] == 0xc2U && cursor[1] == 0xb0U) {
            glyph_index = SHARE_TECH_MONO_DEGREE_INDEX;
            cursor += 2;
        } else {
            /* Meedori Sans is unicase: lower-case codepoints select its
             * alternate glyph style. Preserve the string exactly as given. */
            unsigned char code = *cursor++;
            if (code < SHARE_TECH_MONO_FIRST_CHAR || code > SHARE_TECH_MONO_LAST_CHAR) {
                code = '?';
            }
            glyph_index = (size_t)(code - SHARE_TECH_MONO_FIRST_CHAR);
        }
        const uint8_t *glyph = font->data + glyph_index * glyph_size;
        for (int cy = 0; cy < font->height; ++cy) {
            for (int cx = 0; cx < font->width; ++cx) {
                const uint8_t bits = glyph[(size_t)cy * font->stride + (cx >> 3)];
                if (bits & (0x80U >> (cx & 7))) {
                    pixel_color(x + cx, y + cy, color);
                }
            }
        }
        x += font->width + tracking;
    }
}

static void text_with_font_color_tracking_scaled_x(
    int x, int y, const char *value, const bitmap_font_t *font,
    epd_color_t color, int tracking, unsigned scale_percent)
{
    if (!value || !font || !scale_percent) return;
    const size_t glyph_size = (size_t)font->stride * font->height;
    const int scaled_width =
        ((int)font->width * (int)scale_percent + 50) / 100;

    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor) {
        size_t glyph_index = 0;
        if (cursor[0] == 0xc2U && cursor[1] == 0xb0U) {
            glyph_index = SHARE_TECH_MONO_DEGREE_INDEX;
            cursor += 2;
        } else {
            unsigned char code = *cursor++;
            if (code < SHARE_TECH_MONO_FIRST_CHAR ||
                code > SHARE_TECH_MONO_LAST_CHAR) {
                code = '?';
            }
            glyph_index = (size_t)(code - SHARE_TECH_MONO_FIRST_CHAR);
        }
        const uint8_t *glyph = font->data + glyph_index * glyph_size;
        for (int cy = 0; cy < font->height; ++cy) {
            for (int destination_x = 0; destination_x < scaled_width;
                 ++destination_x) {
                const int source_x = destination_x * font->width /
                                     scaled_width;
                const uint8_t bits = glyph[(size_t)cy * font->stride +
                                           (source_x >> 3)];
                if (bits & (0x80U >> (source_x & 7))) {
                    pixel_color(x + destination_x, y + cy, color);
                }
            }
        }
        x += scaled_width + tracking;
    }
}

static void text_with_font_color(int x, int y, const char *value,
                                 const bitmap_font_t *font, epd_color_t color)
{
    text_with_font_color_tracking(x, y, value, font, color, 0);
}

static void text_with_font(int x, int y, const char *value,
                           const bitmap_font_t *font)
{
    text_with_font_color(x, y, value, font, EPD_COLOR_BLACK);
}

static void text_with_font_half_size(int x, int y, const char *value,
                                     const bitmap_font_t *font)
{
    if (!value || !font) return;
    const size_t glyph_size = (size_t)font->stride * font->height;
    const int cell_width = (font->width + 1) / 2;

    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; ++cursor) {
        unsigned char code = *cursor;
        if (code < SHARE_TECH_MONO_FIRST_CHAR ||
            code > SHARE_TECH_MONO_LAST_CHAR) {
            code = '?';
        }
        const uint8_t *glyph = font->data +
            (size_t)(code - SHARE_TECH_MONO_FIRST_CHAR) * glyph_size;
        for (int destination_y = 0;
             destination_y < (font->height + 1) / 2; ++destination_y) {
            for (int destination_x = 0; destination_x < cell_width;
                 ++destination_x) {
                bool set = false;
                for (int source_dy = 0; source_dy < 2 && !set; ++source_dy) {
                    const int source_y = destination_y * 2 + source_dy;
                    if (source_y >= font->height) continue;
                    for (int source_dx = 0; source_dx < 2; ++source_dx) {
                        const int source_x = destination_x * 2 + source_dx;
                        if (source_x >= font->width) continue;
                        const uint8_t bits = glyph[(size_t)source_y *
                                                   font->stride +
                                                   (source_x >> 3)];
                        if (bits & (0x80U >> (source_x & 7))) {
                            set = true;
                            break;
                        }
                    }
                }
                if (set) pixel(x + destination_x, y + destination_y, true);
            }
        }
        x += cell_width;
    }
}

static size_t text_cell_count(const char *value)
{
    size_t count = 0;
    const unsigned char *cursor = (const unsigned char *)value;
    while (cursor && *cursor) {
        if (cursor[0] == 0xc2U && cursor[1] == 0xb0U) {
            cursor += 2;
        } else {
            ++cursor;
        }
        ++count;
    }
    return count;
}

static int text_width_with_tracking(const char *value,
                                    const bitmap_font_t *font, int tracking)
{
    const size_t cells = text_cell_count(value);
    if (!cells) return 0;
    return (int)cells * font->width + ((int)cells - 1) * tracking;
}

static void text_status_centered(int y, const char *value)
{
    const int width = (int)text_cell_count(value) * s_share_tech_mono_status.width;
    text_with_font((EPD_WIDTH - width) / 2, y, value,
                   &s_share_tech_mono_status);
}

static void text_status_header(int y, const char *value)
{
    const int width = (int)text_cell_count(value) * s_share_tech_mono_status.width;
    const int header_right = 140;
    text_with_font((header_right - width) / 2, y, value,
                   &s_share_tech_mono_status);
}

static void text_centered(int y, const char *value, int scale)
{
    if (scale < 1) {
        scale = 1;
    } else if (scale > 4) {
        scale = 4;
    }
    const bitmap_font_t *font = s_share_tech_mono_fonts[scale - 1];
    const int width = (int)text_cell_count(value) * font->width;
    text_with_font((EPD_WIDTH - width) / 2, y, value, font);
}

static void text_bold_centered_color(int y, const char *value, int scale,
                                     epd_color_t color)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    const bitmap_font_t *font = s_share_tech_mono_fonts[scale - 1];
    const int width = (int)text_cell_count(value) * font->width;
    const int x = (EPD_WIDTH - width) / 2;
    text_with_font_color(x, y, value, font, color);
}

static void text_primary_centered_color(int x_offset, int y, const char *value,
                                        epd_color_t color)
{
    const bitmap_font_t *font = s_share_tech_mono_fonts[2];
    const int tracking = -6;
    const int width = text_width_with_tracking(value, font, tracking);
    text_with_font_color_tracking((EPD_WIDTH - width) / 2 + x_offset, y,
                                  value, font, color, tracking);
}

static void text_bold_centered(int y, const char *value, int scale)
{
    text_bold_centered_color(y, value, scale, EPD_COLOR_BLACK);
}

static void fill_rectangle(int left, int top, int right, int bottom,
                           epd_color_t color)
{
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            pixel_color(x, y, color);
        }
    }
}

static void fill_dithered_rectangle(int left, int top, int right, int bottom,
                                    epd_color_t first, epd_color_t second)
{
    /* The JD79660AA exposes only four native LUT states. A 4x4 Bayer pattern
     * spatially alternates two native states at 50%, producing an apparent
     * intermediate color when viewed from normal reading distance. */
    static const uint8_t bayer_4x4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const uint8_t threshold = bayer_4x4[(y - top) & 3][(x - left) & 3];
            pixel_color(x, y, threshold < 8 ? first : second);
        }
    }
}

static void draw_test_swatch(int column, int row, const char *label,
                             epd_color_t first, epd_color_t second)
{
    const int cell_width = EPD_WIDTH / 3;
    const int cell_height = EPD_HEIGHT / 4;
    const int cell_left = column * cell_width;
    const int cell_top = row * cell_height;
    const bitmap_font_t *font = s_share_tech_mono_fonts[0];
    const int label_width = (int)text_cell_count(label) * font->width;
    text_with_font(cell_left + (cell_width - label_width) / 2,
                   cell_top, label, font);

    const int swatch_size = 33;
    const int left = cell_left + (cell_width - swatch_size) / 2;
    const int top = cell_top + 16;
    if (first == second) {
        fill_rectangle(left, top, left + swatch_size - 1,
                       top + swatch_size - 1, first);
    } else {
        fill_dithered_rectangle(left, top, left + swatch_size - 1,
                                top + swatch_size - 1, first, second);
    }
}

static void battery_indicator(uint8_t percent)
{
    const int left = 142;
    const int top = 0;
    const int right = 195;
    const int bottom = 23;
    const epd_color_t interior = percent >= 60U ? EPD_COLOR_BLACK :
        (percent >= 35U && BOARD_DISPLAY_IS_FOUR_COLOR ?
            EPD_COLOR_YELLOW :
            (BOARD_DISPLAY_IS_FOUR_COLOR ? EPD_COLOR_RED : EPD_COLOR_WHITE));
    const epd_color_t text_color = percent >= 60U ?
        EPD_COLOR_WHITE : EPD_COLOR_BLACK;

    fill_rectangle(left, top, right, bottom, interior);
    fill_rectangle(right + 1, top + 5, right + 4, bottom - 5, EPD_COLOR_BLACK);
    for (int x = left; x <= right; ++x) {
        pixel_color(x, top, EPD_COLOR_BLACK);
        pixel_color(x, top + 1, EPD_COLOR_BLACK);
        pixel_color(x, bottom - 1, EPD_COLOR_BLACK);
        pixel_color(x, bottom, EPD_COLOR_BLACK);
    }
    for (int y = top; y <= bottom; ++y) {
        pixel_color(left, y, EPD_COLOR_BLACK);
        pixel_color(left + 1, y, EPD_COLOR_BLACK);
        pixel_color(right - 1, y, EPD_COLOR_BLACK);
        pixel_color(right, y, EPD_COLOR_BLACK);
    }

    char label[8];
    snprintf(label, sizeof(label), "%u%%", (unsigned)percent);
    const bitmap_font_t *font = &s_share_tech_mono_status;
    const int tracking = -1;
    const int scaled_font_width = (font->width * 110 + 50) / 100;
    const size_t cells = text_cell_count(label);
    const int width = (int)cells * scaled_font_width +
                      ((int)cells - 1) * tracking;
    text_with_font_color_tracking_scaled_x(
        left + (right - left + 1 - width) / 2, 3, label, font,
        text_color, tracking, 110);
}

static void rssi_indicator(int x, int y, int8_t rssi, bool connected)
{
    int level = connected ? 1 : 0;
    if (connected && rssi >= -55) {
        level = 4;
    } else if (connected && rssi >= -67) {
        level = 3;
    } else if (connected && rssi >= -75) {
        level = 2;
    }

    const int bottom = y + 12;
    for (int bar = 0; bar < 4; ++bar) {
        const int left = x + bar * 4;
        const int height = 3 * (bar + 1);
        for (int dx = 0; dx < 3; ++dx) {
            const int top = bottom - height + 1 - dx;
            for (int py = top; py <= bottom; ++py) {
                const bool outline = dx == 0 || dx == 2 || py == top || py == bottom;
                if (bar < level || outline) {
                    pixel(left + dx, py, true);
                }
            }
        }
    }

    if (!connected) {
        const epd_color_t slash_color = BOARD_DISPLAY_IS_FOUR_COLOR ?
            EPD_COLOR_RED : EPD_COLOR_BLACK;
        for (int offset = 0; offset < 3; ++offset) {
            const int x0 = x + offset;
            const int y0 = y;
            const int x1 = x + 15 + offset;
            const int y1 = y + 13;
            const int dx = x1 - x0;
            const int dy = y1 - y0;
            int error = dx - dy;
            int px = x0;
            int py = y0;
            while (true) {
                pixel_color(px, py, slash_color);
                if (px == x1 && py == y1) break;
                const int doubled = error * 2;
                if (doubled > -dy) {
                    error -= dy;
                    ++px;
                }
                if (doubled < dx) {
                    error += dx;
                    ++py;
                }
            }
        }
    }
}

static void line_segment(int x0, int y0, int x1, int y1)
{
    const int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx - dy;

    while (true) {
        pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int doubled = error * 2;
        if (doubled > -dy) {
            error -= dy;
            x0 += sx;
        }
        if (doubled < dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void mqtt_indicator(int x, int y)
{
    /* Compact mosquito silhouette for a successfully reached MQTT broker. */
    line_segment(x + 5, y + 7, x + 14, y + 7);
    line_segment(x + 2, y + 5, x + 6, y + 7);
    line_segment(x + 2, y + 9, x + 6, y + 7);
    line_segment(x + 7, y + 6, x + 3, y + 1);
    line_segment(x + 8, y + 6, x + 7, y + 1);
    line_segment(x + 10, y + 6, x + 14, y + 1);
    line_segment(x + 11, y + 6, x + 17, y + 3);
    line_segment(x + 7, y + 8, x + 4, y + 13);
    line_segment(x + 10, y + 8, x + 9, y + 14);
    line_segment(x + 12, y + 8, x + 16, y + 13);
    line_segment(x + 14, y + 6, x + 17, y + 4);
    line_segment(x + 14, y + 7, x + 18, y + 7);
    pixel(x + 13, y + 6, true);
    pixel(x + 13, y + 7, true);
    pixel(x + 14, y + 6, true);
    pixel(x + 14, y + 7, true);
}

static void line_segment_color_width(int x0, int y0, int x1, int y1,
                                     epd_color_t color, int width)
{
    const int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    const int offset_x = dy > dx ? 1 : 0;
    const int offset_y = dy > dx ? 0 : 1;

    for (int stroke = 0; stroke < width; ++stroke) {
        int px = x0 + stroke * offset_x;
        int py = y0 + stroke * offset_y;
        const int end_x = x1 + stroke * offset_x;
        const int end_y = y1 + stroke * offset_y;
        const int line_dx = end_x >= px ? end_x - px : px - end_x;
        const int step_x = px < end_x ? 1 : -1;
        const int line_dy = end_y >= py ? end_y - py : py - end_y;
        const int step_y = py < end_y ? 1 : -1;
        int error = line_dx - line_dy;

        while (true) {
            pixel_color(px, py, color);
            if (px == end_x && py == end_y) break;
            const int doubled = error * 2;
            if (doubled > -line_dy) {
                error -= line_dy;
                px += step_x;
            }
            if (doubled < line_dx) {
                error += line_dx;
                py += step_y;
            }
        }
    }
}

static void dashed_horizontal_line(int left, int right, int y,
                                   int on_pixels, int off_pixels,
                                   epd_color_t color)
{
    const int period = on_pixels + off_pixels;
    for (int x = left; x <= right; ++x) {
        if ((x - left) % period < on_pixels) pixel_color(x, y, color);
    }
}

static void dashed_vertical_line(int x, int top, int bottom,
                                 int on_pixels, int off_pixels,
                                 epd_color_t color)
{
    const int period = on_pixels + off_pixels;
    for (int y = top; y <= bottom; ++y) {
        if ((y - top) % period < on_pixels) pixel_color(x, y, color);
    }
}

static void draw_arrow(int x, int y, bool up)
{
    const int tip = y + (up ? -5 : 5);
    const int wing = y + (up ? -1 : 1);
    line_segment(x, y - 5, x, y + 5);
    line_segment(x, tip, x - 3, wing);
    line_segment(x, tip, x + 3, wing);
}

static void delta_row(int y, const char *period, bool valid, float delta)
{
    char line[24];
    if (valid) {
        snprintf(line, sizeof(line), "%s %.1f" "\xc2\xb0" "c", period,
                 fabsf(delta));
    } else {
        snprintf(line, sizeof(line), "%s n/d", period);
    }
    const bitmap_font_t *font = s_share_tech_mono_fonts[0];
    const int width = (int)text_cell_count(line) * font->width;
    const int x = (EPD_WIDTH - width) / 2;
    text_with_font(x, y, line, font);
    if (valid) {
        draw_arrow(x + width + 8, y + font->height / 2, delta >= 0.0f);
    }
}

static void history_chart(const float *values, uint8_t count,
                          uint8_t span_days)
{
    const int left = 7;
    const int top = 119;
    const int right = 192;
    const int bottom = 196;

    fill_rectangle(left, top, right, bottom, EPD_COLOR_WHITE);

    /* The frame remains two pixels thick around the white plot area. */
    line_segment(left, top, right, top);
    line_segment(left, top + 1, right, top + 1);
    line_segment(right, top, right, bottom);
    line_segment(right - 1, top, right - 1, bottom);
    line_segment(right, bottom, left, bottom);
    line_segment(right, bottom - 1, left, bottom - 1);
    line_segment(left, bottom, left, top);
    line_segment(left + 1, bottom, left + 1, top);

    const int plot_left = left + 2;
    const int plot_right = right - 2;
    const int plot_top = top + 2;
    const int plot_bottom = bottom - 2;
    const epd_color_t grid_color = EPD_COLOR_BLACK;

    /* Six primary divisions follow the adaptive history span. */
    for (int division = 1; division < 6; ++division) {
        const int x = plot_left + division * (plot_right - plot_left) / 6;
        dashed_vertical_line(x, plot_top, plot_bottom, 1, 1, grid_color);
    }

    const float graph_minimum = 2800.0f;
    const float graph_maximum = 4200.0f;
    for (int value = 2800; value <= 4200; value += 100) {
        const float normalized =
            (value - graph_minimum) / (graph_maximum - graph_minimum);
        const int y = plot_bottom -
            (int)lroundf(normalized * (plot_bottom - plot_top));
        if (((value - 2800) % 200) == 0) {
            dashed_horizontal_line(plot_left, plot_right, y, 1, 1,
                                   grid_color);
        } else {
            dashed_horizontal_line(plot_left, plot_right, y, 1, 2,
                                   grid_color);
        }
    }
    if (span_days) {
        char span_label[8];
        snprintf(span_label, sizeof(span_label), "%ud", (unsigned)span_days);
        const bitmap_font_t *font = &s_share_tech_mono_status;
        const int width = (int)text_cell_count(span_label) *
                          ((font->width + 1) / 2);
        const int height = (font->height + 1) / 2;
        text_with_font_half_size(right - width + 1, top - height - 2,
                                 span_label, font);
    }
    if (!values || !count) return;

    int previous_x = plot_left;
    int previous_y = (plot_top + plot_bottom) / 2;
    bool previous_valid = false;
    for (uint8_t i = 0; i < count; ++i) {
        const int x = count == 1 ? (plot_left + plot_right) / 2 :
            plot_left + (int)i * (plot_right - plot_left) / (count - 1U);
        if (!isfinite(values[i])) {
            previous_valid = false;
            continue;
        }
        const float normalized = fminf(1.0f, fmaxf(0.0f,
            (values[i] - graph_minimum) / (graph_maximum - graph_minimum)));
        const int y = plot_bottom - (int)lroundf(normalized * (plot_bottom - plot_top));
        const epd_color_t series_color = BOARD_DISPLAY_IS_FOUR_COLOR ?
            EPD_COLOR_RED : EPD_COLOR_BLACK;
        if (previous_valid) {
            line_segment_color_width(previous_x, previous_y, x, y,
                                     series_color, 3);
        }
        fill_rectangle(x, y, x + 2, y + 2, series_color);
        previous_x = x;
        previous_y = y;
        previous_valid = true;
    }
}

static void render(const epaper_view_t *view)
{
    clear_framebuffer();
    fill_rectangle(0, 0, EPD_WIDTH - 1, EPD_HEIGHT - 1,
                   BOARD_DISPLAY_IS_FOUR_COLOR ? EPD_COLOR_YELLOW :
                                                 EPD_COLOR_WHITE);
    char line[40];
    struct tm local = {0};
    const unsigned battery_pct = view->battery_pct > 100U ? 100U : view->battery_pct;
    if (view->time_valid) {
        localtime_r(&view->timestamp, &local);
        snprintf(line, sizeof(line), "%02d/%02d/%02d",
                 local.tm_mday, local.tm_mon + 1,
                 (local.tm_year + 1900) % 100);
    } else {
        snprintf(line, sizeof(line), "--/--/--");
    }
    rssi_indicator(1, 1, view->rssi, view->wifi_connected);
    text_status_header(1, line);
    if (view->mqtt_connected) mqtt_indicator(121, 3);
    battery_indicator((uint8_t)battery_pct);
    if (!view->environment_valid) {
        text_bold_centered(27, "sensore", 2);
        text_centered(63, "i2c assente", 1);
        text_centered(93, "riprovo", 1);
        text_centered(119, "prossimo", 1);
        text_centered(144, "ciclo", 1);
        return;
    }
    if (view->page == 0) {
        const epd_color_t primary_color = BOARD_DISPLAY_IS_FOUR_COLOR ?
            EPD_COLOR_RED : EPD_COLOR_BLACK;
        snprintf(line, sizeof(line), "%.1f" "\xc2\xb0" "c", view->temperature_c);
        text_primary_centered_color(5, 26, line, primary_color);
        delta_row(70, "1h", view->delta_valid, view->delta_c);
        snprintf(line, sizeof(line), "min %.1f" "\xc2\xb0" "c", view->day_min_c);
        text_centered(92, line, 1);
        snprintf(line, sizeof(line), "max %.1f" "\xc2\xb0" "c", view->day_max_c);
        text_centered(113, line, 1);
        snprintf(line, sizeof(line), "%.0f%%", view->humidity_pct);
        text_primary_centered_color(0, 136, line, primary_color);
        snprintf(line, sizeof(line), "dew %.1f" "\xc2\xb0" "c", view->dew_point_c);
        text_centered(179, line, 1);
    } else {
        delta_row(23, "1d", view->history_valid[0], view->history_delta_c[0]);
        delta_row(46, "1w", view->history_valid[1], view->history_delta_c[1]);
        delta_row(69, "1m", view->history_valid[2], view->history_delta_c[2]);
        delta_row(92, "1y", view->history_valid[3], view->history_delta_c[3]);
        history_chart(view->chart_values, view->chart_count,
                      view->chart_span_days);
    }
}

static esp_err_t prepare_display(void)
{
    ESP_RETURN_ON_ERROR(board_epaper_power(true), TAG, "display power");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "EPD power on; BUSY GPIO%d=%d (ready=%d)",
             BOARD_EPD_BUSY_GPIO, gpio_get_level(BOARD_EPD_BUSY_GPIO),
             BOARD_EPD_BUSY_READY_LEVEL);

    if (!s_spi) {
        const gpio_config_t output = {
            .pin_bit_mask = (1ULL << BOARD_EPD_DC_GPIO) |
                            (1ULL << BOARD_EPD_RESET_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        const gpio_config_t input = {
            .pin_bit_mask = 1ULL << BOARD_EPD_BUSY_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "display outputs");
        ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "display busy");

        const spi_bus_config_t bus = {
            .mosi_io_num = BOARD_EPD_MOSI_GPIO,
            .miso_io_num = -1,
            .sclk_io_num = BOARD_EPD_SCLK_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = EPD_COLOR_BUFFER_SIZE,
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");
        const spi_device_interface_config_t device = {
            .clock_speed_hz = 4 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = BOARD_EPD_CS_GPIO,
            .queue_size = 1,
        };
        ESP_RETURN_ON_ERROR(spi_bus_add_device(EPD_SPI_HOST, &device, &s_spi), TAG, "SPI device");
    }

    if (BOARD_DISPLAY_IS_FOUR_COLOR) {
        return four_color_controller_init();
    }
    ESP_RETURN_ON_ERROR(mono_controller_init(), TAG, "monochrome controller init");
    return clear_panel_to_white();
}

static esp_err_t present_framebuffer(bool partial)
{
    if (BOARD_DISPLAY_IS_FOUR_COLOR) {
        ESP_RETURN_ON_ERROR(send_command(0x10), TAG, "four-color RAM cmd");
        ESP_RETURN_ON_ERROR(send_data(s_framebuffer, EPD_COLOR_BUFFER_SIZE), TAG,
                            "four-color RAM");
        ESP_RETURN_ON_ERROR(send_command(0x50), TAG,
                            "four-color refresh interval cmd");
        ESP_RETURN_ON_ERROR(send_byte(0x37), TAG,
                            "four-color refresh interval");
        ESP_RETURN_ON_ERROR(send_command(0x12), TAG, "four-color refresh cmd");
        ESP_RETURN_ON_ERROR(send_byte(0x00), TAG, "four-color refresh");
        return wait_four_color_busy_cycle("refresh");
    }
    ESP_RETURN_ON_ERROR(set_window_and_cursor(), TAG, "set cursor");
    ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "write RAM cmd");
    ESP_RETURN_ON_ERROR(send_data(s_framebuffer, EPD_MONO_BUFFER_SIZE), TAG,
                        "write RAM");
    return partial ? activate_partial_refresh() : activate_full_refresh();
}

esp_err_t epaper_show(const epaper_view_t *view)
{
    if (!view) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(prepare_display(), TAG, "display init");
    render(view);
    return present_framebuffer(false);
}

esp_err_t epaper_show_test_pattern(void)
{
    ESP_RETURN_ON_ERROR(prepare_display(), TAG, "display init");
    clear_framebuffer();

    if (BOARD_DISPLAY_IS_FOUR_COLOR) {
        /* Four native colors followed by all C(4,2) two-color mixtures.
         * Mixtures are ordered-dither patterns, not extra controller colors. */
        draw_test_swatch(0, 0, "bk", EPD_COLOR_BLACK, EPD_COLOR_BLACK);
        draw_test_swatch(1, 0, "wh", EPD_COLOR_WHITE, EPD_COLOR_WHITE);
        draw_test_swatch(2, 0, "rd", EPD_COLOR_RED, EPD_COLOR_RED);
        draw_test_swatch(0, 1, "yl", EPD_COLOR_YELLOW, EPD_COLOR_YELLOW);
        draw_test_swatch(1, 1, "bk+wh", EPD_COLOR_BLACK, EPD_COLOR_WHITE);
        draw_test_swatch(2, 1, "bk+rd", EPD_COLOR_BLACK, EPD_COLOR_RED);
        draw_test_swatch(0, 2, "bk+yl", EPD_COLOR_BLACK, EPD_COLOR_YELLOW);
        draw_test_swatch(1, 2, "wh+rd", EPD_COLOR_WHITE, EPD_COLOR_RED);
        draw_test_swatch(2, 2, "wh+yl", EPD_COLOR_WHITE, EPD_COLOR_YELLOW);
        draw_test_swatch(1, 3, "rd+yl", EPD_COLOR_RED, EPD_COLOR_YELLOW);
    } else {
        for (int y = 0; y < EPD_HEIGHT; y += 20) {
            for (int x = 0; x < EPD_WIDTH; x += 20) {
                if (((x / 20) + (y / 20)) & 1) {
                    fill_rectangle(x, y, x + 19, y + 19, EPD_COLOR_BLACK);
                }
            }
        }
    }
    return present_framebuffer(false);
}

esp_err_t epaper_show_ota_progress(uint8_t percent, const char *status)
{
    if (percent > 100U) percent = 100U;
    if (!status) status = "updating";
    const bool first_frame = !s_ota_progress_started;
    if (first_frame) {
        ESP_RETURN_ON_ERROR(prepare_display(), TAG, "display init");
        s_ota_progress_started = true;
    }
    clear_framebuffer();
    text_bold_centered(28, "firmware ota", 1);
    text_centered(58, status, 1);
    char line[8];
    snprintf(line, sizeof(line), "%u%%", percent);
    text_bold_centered(78, line, 3);

    const int left = 18;
    const int right = EPD_WIDTH - 18;
    const int top = 126;
    const int bottom = 140;
    for (int x = left; x <= right; ++x) {
        pixel(x, top, true);
        pixel(x, top + 1, true);
        pixel(x, bottom, true);
        pixel(x, bottom - 1, true);
    }
    for (int y = top; y <= bottom; ++y) {
        pixel(left, y, true);
        pixel(left + 1, y, true);
        pixel(right, y, true);
        pixel(right - 1, y, true);
    }
    const int fill_right = left + 4 + ((right - left - 8) * percent) / 100;
    for (int y = top + 4; y < bottom - 3; ++y) {
        for (int x = left + 4; x <= fill_right; ++x) {
            pixel(x, y, true);
        }
    }
    /* The monochrome controller can update the existing image without a full
     * flashing refresh. The four-color controller cannot: keep its initial
     * OTA screen static during the transfer and refresh it only at completion. */
    if (BOARD_DISPLAY_IS_FOUR_COLOR && !first_frame && percent < 100U) {
        return ESP_OK;
    }
    const esp_err_t result = present_framebuffer(!BOARD_DISPLAY_IS_FOUR_COLOR &&
                                                 !first_frame);
    if (percent >= 100U) s_ota_progress_started = false;
    return result;
}

esp_err_t epaper_show_ota_result(const char *status)
{
    if (!status) status = "ota terminato";
    const bool display_ready = s_ota_progress_started;
    if (!display_ready) {
        ESP_RETURN_ON_ERROR(prepare_display(), TAG, "display init");
    }
    clear_framebuffer();
    text_bold_centered(48, "firmware ota", 1);
    text_bold_centered(86, status, 1);
    text_centered(126, "rilascia power", 1);
    const esp_err_t result = present_framebuffer(!BOARD_DISPLAY_IS_FOUR_COLOR &&
                                                 display_ready);
    s_ota_progress_started = false;
    return result;
}

static void provisioning_qr(esp_qrcode_handle_t qrcode)
{
    const int size = esp_qrcode_get_size(qrcode);
    const int quiet_modules = 4;
    const int available = 135;
    int scale = available / (size + quiet_modules * 2);
    if (scale > 5) scale = 5;
    if (scale < 2) scale = 2;
    const int total = (size + quiet_modules * 2) * scale;
    const int origin_x = (EPD_WIDTH - total) / 2 + quiet_modules * scale;
    const int origin_y = 20 + (available - total) / 2 + quiet_modules * scale;
    ESP_LOGI(TAG, "provisioning QR: %dx%d modules, scale=%d, origin=(%d,%d)",
             size, size, scale, origin_x, origin_y);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!esp_qrcode_get_module(qrcode, x, y)) continue;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    pixel(origin_x + x * scale + dx, origin_y + y * scale + dy, true);
                }
            }
        }
    }
}

esp_err_t epaper_show_provisioning(const char *ap_ssid, const char *ap_password)
{
    if (!ap_ssid || !ap_password) {
        return ESP_ERR_INVALID_ARG;
    }
    char qr_payload[96];
    const int length = snprintf(qr_payload, sizeof(qr_payload),
                                "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_password);
    if (length < 0 || length >= (int)sizeof(qr_payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(prepare_display(), TAG, "display init");
    clear_framebuffer();
    text_centered(1, "wifi setup", 1);
    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func = provisioning_qr;
    config.max_qrcode_version = 6;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    ESP_RETURN_ON_ERROR(esp_qrcode_generate(&config, qr_payload), TAG, "QR generation");
    text_status_centered(158, ap_ssid);
    char password_line[28];
    snprintf(password_line, sizeof(password_line), "pass %s", ap_password);
    text_status_centered(179, password_line);

    return present_framebuffer(false);
}

void epaper_shutdown(void)
{
    if (s_spi) {
        if (BOARD_DISPLAY_IS_FOUR_COLOR) {
            (void)send_command(0x02);
            (void)send_byte(0x00);
            (void)wait_four_color_busy_cycle("power off");
            (void)send_command(0x07);
            (void)send_byte(0xa5);
        } else {
            (void)send_command(0x10);
            (void)send_byte(0x01);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    (void)board_epaper_power(false);
    s_ota_progress_started = false;
}
