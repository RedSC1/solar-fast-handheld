/* ESP32-S3 + ST7735S + K1..K8 wiring self-test.
 * This is deliberately separate from the full UI: verify the actual hardware
 * before attributing a blank screen or a dead key to the calendar code. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_W 160
#define LCD_H 128
#define LCD_DC GPIO_NUM_9
#define LCD_RST GPIO_NUM_8

static const char *TAG = "hwtest";
static const gpio_num_t keys[8] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_13,
    GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17,
};
static spi_device_handle_t lcd_spi;
static uint8_t *frame;

static void lcd_send(int dc, const void *data, size_t n)
{
    gpio_set_level(LCD_DC, dc);
    /* S3 SPI DMA has a 2^18-bit transaction limit. Keep chunks smaller and
     * 4-byte aligned so a complete 40 KiB framebuffer can be streamed. */
    const uint8_t *p = data;
    while (n) {
        size_t chunk = n > 4092 ? 4092 : n;
        spi_transaction_t t = { .length = chunk * 8, .tx_buffer = p };
        ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
        p += chunk;
        n -= chunk;
    }
}

static void lcd_cmd(uint8_t cmd)
{
    lcd_send(0, &cmd, 1);
}

static void lcd_data(const uint8_t *data, size_t n)
{
    lcd_send(1, data, n);
}

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
        .max_transfer_sz = LCD_W * LCD_H * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = GPIO_NUM_10,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd_spi));

    lcd_cmd(0x01); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11); /* SLPOUT */
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
    lcd_reg(0x36, (const uint8_t[]){0x60}, 1); /* landscape, RGB */
    lcd_cmd(0x13); /* NORON */
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_cmd(0x29); /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(100));

    frame = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!frame) abort();
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            size_t p = ((size_t)y * LCD_W + x) * 2;
            frame[p] = color >> 8;
            frame[p + 1] = color & 0xff;
        }
    }
}

static void lcd_show(uint8_t pressed)
{
    static const uint16_t rainbow[8] = {
        0xf800, 0xfc00, 0xffe0, 0x07e0,
        0x07ff, 0x001f, 0x781f, 0xffff,
    };
    fill_rect(0, 0, LCD_W, LCD_H, 0x0000);
    for (int i = 0; i < 8; ++i) {
        int x = i * 20;
        fill_rect(x, 0, x + 20, 28, rainbow[i]);
        fill_rect(x + 2, 45, x + 18, 110,
                  (pressed & (1u << i)) ? 0x07e0 : 0x39e7);
    }
    const uint8_t col[] = {0x00, 0x00, 0x00, 0x9f};
    const uint8_t row[] = {0x00, 0x00, 0x00, 0x7f};
    lcd_reg(0x2a, col, sizeof col); /* CASET */
    lcd_reg(0x2b, row, sizeof row); /* RASET */
    lcd_cmd(0x2c); /* RAMWR */
    lcd_data(frame, LCD_W * LCD_H * 2);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ST7735S + K1..K8 self-test starting");
    lcd_init();

    uint64_t mask = 0;
    for (int i = 0; i < 8; ++i) mask |= 1ULL << keys[i];
    gpio_config_t in = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    uint8_t last = 0xff;
    int ticks = 0;
    for (;;) {
        uint8_t pressed = 0;
        for (int i = 0; i < 8; ++i) {
            if (gpio_get_level(keys[i]) == 0) pressed |= 1u << i;
        }
        if (pressed != last) {
            lcd_show(pressed);
            ESP_LOGI(TAG, "K1..K8 pressed mask: 0x%02x", pressed);
            last = pressed;
        }
        if (++ticks == 100) {
            ESP_LOGI(TAG, "alive; pressed mask: 0x%02x", pressed);
            ticks = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
