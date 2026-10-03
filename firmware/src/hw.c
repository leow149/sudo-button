// Waveshare ESP32-S3-Touch-LCD-4.3: CH422G expander, RGB565 panel, GT911 touch.
#include "hw.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw";

#define PIN_SDA 8
#define PIN_SCL 9
#define PIN_TP_INT 4

// CH422G: 0x24 = system config (bit0 = IO output enable), 0x38 = IO0..7 output
#define CH422G_ADDR_SET 0x24
#define CH422G_ADDR_IO 0x38
// EXIOn is bit n of the IO byte
#define EXIO_TP_RST (1 << 1)
#define EXIO_BL (1 << 2)
#define EXIO_LCD_RST (1 << 3)
#define EXIO_SD_CS (1 << 4)
#define EXIO_USB_SEL (1 << 5) // low = native USB, high = CAN

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t exio_set, exio_io;
static uint8_t exio_state;
static esp_lcd_panel_handle_t panel;
static esp_lcd_touch_handle_t touch;

static esp_err_t exio_flush(void)
{
    return i2c_master_transmit(exio_io, &exio_state, 1, 100);
}

static void exio_set_bits(uint8_t mask, bool on)
{
    exio_state = on ? (exio_state | mask) : (exio_state & ~mask);
    ESP_ERROR_CHECK_WITHOUT_ABORT(exio_flush());
}

void hw_backlight(bool on) { exio_set_bits(EXIO_BL, on); }
bool hw_backlight_is_on(void) { return exio_state & EXIO_BL; }
esp_lcd_panel_handle_t hw_panel(void) { return panel; }
esp_lcd_touch_handle_t hw_touch(void) { return touch; }

static esp_err_t init_i2c_expander(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 100000,
    };
    dc.device_address = CH422G_ADDR_SET;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &exio_set));
    dc.device_address = CH422G_ADDR_IO;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &exio_io));

    uint8_t oe = 0x01;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(exio_set, &oe, 1, 100), TAG, "CH422G not responding");

    // Backlight off, touch in reset, LCD in reset, SD deselected, USB selected
    exio_state = EXIO_SD_CS;
    ESP_RETURN_ON_ERROR(exio_flush(), TAG, "exio write");
    vTaskDelay(pdMS_TO_TICKS(10));
    exio_set_bits(EXIO_LCD_RST, true);
    return ESP_OK;
}

static esp_err_t init_touch(void)
{
    // GT911 picks its I2C address from the INT level while reset is released:
    // INT low -> 0x5D
    gpio_config_t g = {.pin_bit_mask = 1ULL << PIN_TP_INT, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&g);
    gpio_set_level(PIN_TP_INT, 0);
    exio_set_bits(EXIO_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    exio_set_bits(EXIO_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(PIN_TP_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        ESP_RETURN_ON_ERROR(i2c_master_probe(bus, addr, 50), TAG, "GT911 not found");
    }
    ESP_LOGI(TAG, "GT911 at 0x%02x", addr);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t ioc = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ioc.dev_addr = addr;
    ioc.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &ioc, &io), TAG, "touch io");

    esp_lcd_touch_config_t tc = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tc, &touch);
}

static esp_err_t init_panel(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_W * 10,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = GPIO_NUM_NC,
        // B0..B4, G0..G5, R0..R4
        .data_gpio_nums = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40},
        .flags.fb_in_psram = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &panel), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    return ESP_OK;
}

esp_err_t hw_init(void)
{
    ESP_RETURN_ON_ERROR(init_i2c_expander(), TAG, "expander");
    ESP_RETURN_ON_ERROR(init_touch(), TAG, "touch");
    ESP_RETURN_ON_ERROR(init_panel(), TAG, "panel");
    return ESP_OK;
}
