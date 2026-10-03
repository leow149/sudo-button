#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#define LCD_W 800
#define LCD_H 480

esp_err_t hw_init(void);
void hw_backlight(bool on);
bool hw_backlight_is_on(void);
esp_lcd_panel_handle_t hw_panel(void);
esp_lcd_touch_handle_t hw_touch(void);
