/* ESP32-S3 host for the portable 160x128 solar-fast-handheld UI. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "bootloader_random.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui.h"
#include "solar_fast.h"

#ifndef HANDHELD_BUILD_EPOCH
#define HANDHELD_BUILD_EPOCH 1790121600LL
#endif

#define LCD_DC GPIO_NUM_9
#define LCD_RST GPIO_NUM_8
#define LCD_BYTES (UI_W * UI_H * 2)

static const char *TAG = "handheld";
static const gpio_num_t key_pins[8] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_13,
    GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17,
};
static spi_device_handle_t lcd_spi;
static uint8_t *lcd_bytes;
static uint16_t ui_pixels[UI_W * UI_H];
static ui_app app;
static uint32_t handheld_random_u32(void *ctx)
{
    (void)ctx;
    return esp_random();
}
static int64_t epoch_at_sync = HANDHELD_BUILD_EPOCH;
static int64_t us_at_sync;
static char command[40];
static size_t command_len;

static void lcd_send(int dc, const void *data, size_t n)
{
    gpio_set_level(LCD_DC, dc);
    const uint8_t *p = data;
    while (n) {
        /* ESP32-S3 DMA transaction limit is 2^18 bits; 4092 also keeps
         * every framebuffer chunk 4-byte aligned. */
        size_t chunk = n > 4092 ? 4092 : n;
        spi_transaction_t t = { .length = chunk * 8, .tx_buffer = p };
        ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
        p += chunk;
        n -= chunk;
    }
}

static void lcd_cmd(uint8_t cmd) { lcd_send(0, &cmd, 1); }
static void lcd_data(const uint8_t *data, size_t n) { lcd_send(1, data, n); }

static void lcd_reg(uint8_t cmd, const uint8_t *args, size_t n)
{
    lcd_cmd(cmd);
    if (n) lcd_data(args, n);
}

static void lcd_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << LCD_DC) | (1ULL << LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = {
        .mosi_io_num = GPIO_NUM_11,
        .miso_io_num = -1,
        .sclk_io_num = GPIO_NUM_12,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = GPIO_NUM_10,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd_spi));

    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t frm[] = {0x01, 0x2c, 0x2d};
    const uint8_t frm3[] = {0x01, 0x2c, 0x2d, 0x01, 0x2c, 0x2d};
    lcd_reg(0xb1, frm, sizeof frm);
    lcd_reg(0xb2, frm, sizeof frm);
    lcd_reg(0xb3, frm3, sizeof frm3);
    lcd_reg(0xb4, (const uint8_t[]){0x07}, 1);
    lcd_reg(0xc0, (const uint8_t[]){0xa2, 0x02, 0x84}, 3);
    lcd_reg(0xc1, (const uint8_t[]){0xc5}, 1);
    lcd_reg(0xc2, (const uint8_t[]){0x0a, 0x00}, 2);
    lcd_reg(0xc3, (const uint8_t[]){0x8a, 0x2a}, 2);
    lcd_reg(0xc4, (const uint8_t[]){0x8a, 0xee}, 2);
    lcd_reg(0xc5, (const uint8_t[]){0x0e}, 1);
    lcd_reg(0x3a, (const uint8_t[]){0x05}, 1); /* RGB565 */
    lcd_reg(0x36, (const uint8_t[]){0x60}, 1); /* landscape RGB */
    lcd_cmd(0x13);
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(100));

    lcd_bytes = heap_caps_malloc(LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!lcd_bytes) abort();
}

static void lcd_present(void)
{
    const uint16_t *src = ui_fb();
    for (size_t i = 0; i < UI_W * UI_H; ++i) {
        lcd_bytes[i * 2] = src[i] >> 8;
        lcd_bytes[i * 2 + 1] = src[i] & 0xff;
    }
    const uint8_t col[] = {0, 0, 0, UI_W - 1};
    const uint8_t row[] = {0, 0, 0, UI_H - 1};
    lcd_reg(0x2a, col, sizeof col);
    lcd_reg(0x2b, row, sizeof row);
    lcd_cmd(0x2c);
    lcd_data(lcd_bytes, LCD_BYTES);
}

static double now_jd_tt(void)
{
    double seconds = (double)epoch_at_sync
                   + (double)(esp_timer_get_time() - us_at_sync) / 1000000.0;
    return sf_utc_to_tt(2440587.5 + seconds / 86400.0);
}

static void accept_command(void)
{
    command[command_len] = 0;
    if (command[0] == 'T') {
        char *end;
        long long value = strtoll(command + 1, &end, 10);
        if (end != command + 1 && *end == 0 && value > 1609459200LL
            && value < 4102444800LL) {
            epoch_at_sync = value;
            us_at_sync = esp_timer_get_time();
            ESP_LOGI(TAG, "UTC synced: %lld", value);
        }
    }
    command_len = 0;
}

static void poll_clock_serial(void)
{
    uint8_t ch;
    while (uart_read_bytes(UART_NUM_0, &ch, 1, 0) == 1) {
        if (ch == '\r' || ch == '\n') {
            if (command_len) accept_command();
        } else if (command_len < sizeof command - 1) {
            command[command_len++] = (char)ch;
        } else {
            command_len = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "solar-fast-handheld starting");
    us_at_sync = esp_timer_get_time();
    lcd_init();
    ui_init(ui_pixels);

    uint64_t key_mask = 0;
    for (int i = 0; i < 8; ++i) key_mask |= 1ULL << key_pins[i];
    gpio_config_t in = {
        .pin_bit_mask = key_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    esp_err_t uart_err = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (uart_err != ESP_OK) ESP_LOGW(TAG, "UART clock sync unavailable: %s", esp_err_to_name(uart_err));

    ui_app_init(&app, now_jd_tt());
    ESP_LOGI(TAG, "UI initialized; build-time clock is provisional");

    uint8_t stable = 0, candidate = 0;
    int64_t change_us[8] = {0};
    for (int i = 0; i < 8; ++i) {
        if (gpio_get_level(key_pins[i]) == 0) {
            stable |= 1u << i;
            candidate |= 1u << i;
        }
    }
    int64_t last_render_us = 0;
    for (;;) {
        if (uart_err == ESP_OK) poll_clock_serial();
        int64_t now_us = esp_timer_get_time();
        int changed = 0;
        for (int i = 0; i < 8; ++i) {
            const uint8_t bit = 1u << i;
            int raw = gpio_get_level(key_pins[i]) == 0;
            int wanted = (candidate & bit) != 0;
            if (raw != wanted) {
                candidate ^= bit;
                change_us[i] = now_us;
            }
            int accepted = (stable & bit) != 0;
            if (raw != accepted && now_us - change_us[i] >= 25000) {
                if (raw) stable |= bit;
                else stable &= ~bit;
                if (raw && i == UI_BTN_MENU
                    && app.feature == UI_FEATURE_LIUYAO
                    && app.page == UI_LIUYAO_PAGE_CHART) {
                    /* 只有第二按停币才取随机数；无 RF 时临时打开 SAR ADC 噪声源。 */
                    const int needs_entropy = app.liuyao_cast_active
                        && app.liuyao_cast_spinning && app.liuyao_cast_count < 6;
                    if (needs_entropy) bootloader_random_enable();
                    (void)ui_app_yijing_cast_press(&app, handheld_random_u32, NULL);
                    if (needs_entropy) bootloader_random_disable();
                } else
                    ui_app_button(&app, (ui_btn)i, raw);
                ESP_LOGI(TAG, "K%d %s", i + 1, raw ? "down" : "up");
                changed = 1;
            }
        }
        const int cast_animating = app.liuyao_cast_exiting
            || (app.liuyao_cast_active
                && (app.liuyao_cast_spinning
                || app.liuyao_cast_slide < UI_CAST_SLIDE_FRAMES
                || app.liuyao_cast_land < UI_CAST_LAND_FRAMES
                || app.liuyao_cast_reveal < UI_CAST_REVEAL_FRAMES));
        if (changed || now_us - last_render_us >=
                (cast_animating ? 70000 : 250000)) {
            ui_app_tick(&app, now_jd_tt());
            ui_render(&app);
            lcd_present();
            last_render_us = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
