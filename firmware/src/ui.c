#include "ui.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hw.h"
#include "lvgl.h"

static const char *TAG = "ui";

LV_FONT_DECLARE(lv_font_hack_20); // fonts/gen.sh

#define IDLE_BACKLIGHT_MS 20000
#define APPROVE_DELAY_MS 700 // ignore taps right after a request appears

typedef enum { EVT_REQUEST, EVT_PROVISIONED } evt_type_t;
typedef struct {
    evt_type_t type;
    bool flag;
    struct req *req;
} evt_t;

struct req {
    char user[96];
    char cwd[256];
    char *cmd;
    int timeout_s;
};

static ui_result_cb_t result_cb;
static ui_forget_cb_t forget_cb;
static QueueHandle_t evq;

static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t draw_buf;
static lv_indev_drv_t indev_drv;

static lv_obj_t *scr_idle, *idle_status;
static bool provisioned;

// active request
static bool req_active;
static lv_obj_t *scr_req, *req_panel, *req_bar, *req_count, *req_hint, *btn_approve;
static lv_timer_t *req_timer;
static int64_t req_start_us;
static int req_timeout_ms;
static bool req_overflow, req_at_end;

static lv_timer_t *flash_timer;

// ---- LVGL glue -------------------------------------------------------------

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
    esp_lcd_panel_draw_bitmap(hw_panel(), a->x1, a->y1, a->x2 + 1, a->y2 + 1, c);
    lv_disp_flush_ready(d);
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static bool swallow; // first touch only wakes the screen
    uint16_t x[1], y[1];
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(hw_touch());
    bool pressed = esp_lcd_touch_get_coordinates(hw_touch(), x, y, NULL, &cnt, 1) && cnt > 0;

    data->state = LV_INDEV_STATE_RELEASED;
    if (!pressed) {
        swallow = false;
        return;
    }
    if (!hw_backlight_is_on()) {
        hw_backlight(true);
        lv_disp_trig_activity(NULL);
        swallow = true;
    }
    if (swallow) return;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x[0];
    data->point.y = y[0];
}

static void tick_cb(void *arg) { lv_tick_inc(2); }

static void glue_init(void)
{
    lv_init();

    size_t px = LCD_W * 40;
    lv_color_t *b1 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *b2 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(b1 && b2);
    lv_disp_draw_buf_init(&draw_buf, b1, b2, px);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_W;
    disp_drv.ver_res = LCD_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    indev_drv.long_press_time = 3000; // only the "forget host" button uses long press
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t ta = {.callback = tick_cb, .name = "lv_tick"};
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, 2000));
}

// ---- idle screen -----------------------------------------------------------

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_idle_status(const char *text, uint32_t color)
{
    lv_label_set_text(idle_status, text);
    lv_obj_set_style_text_color(idle_status, lv_color_hex(color), 0);
}

static void reset_idle_status(void)
{
    if (provisioned) set_idle_status("Ready", 0x4CAF50);
    else set_idle_status("NOT PAIRED  -  run sudo-btn-provision on the PC", 0xFFB300);
}

static void flash_timer_cb(lv_timer_t *t)
{
    flash_timer = NULL;
    lv_timer_del(t);
    reset_idle_status();
}

static void flash_result(const char *text, uint32_t color)
{
    set_idle_status(text, color);
    if (flash_timer) lv_timer_del(flash_timer);
    flash_timer = lv_timer_create(flash_timer_cb, 1500, NULL);
}

static void forget_ev(lv_event_t *e)
{
    if (forget_cb) forget_cb();
    provisioned = false;
    flash_result("Host forgotten", 0xFFB300);
}

static void backlight_timer_cb(lv_timer_t *t)
{
    if (!req_active && hw_backlight_is_on() && lv_disp_get_inactive_time(NULL) > IDLE_BACKLIGHT_MS)
        hw_backlight(false);
}

static void build_idle(void)
{
    scr_idle = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_idle, lv_color_black(), 0);
    lv_obj_t *title = mk_label(scr_idle, &lv_font_montserrat_32, 0xFFFFFF, "sudo-button");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);
    idle_status = mk_label(scr_idle, &lv_font_montserrat_24, 0x4CAF50, "");
    lv_obj_align(idle_status, LV_ALIGN_CENTER, 0, 0);
    reset_idle_status();

    lv_obj_t *btn = lv_btn_create(scr_idle);
    lv_obj_set_size(btn, 260, 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x303030), 0);
    lv_obj_add_event_cb(btn, forget_ev, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *bl = mk_label(btn, &lv_font_montserrat_24, 0xFFFFFF, "Hold 3s: forget host");
    lv_obj_center(bl);

    lv_timer_create(backlight_timer_cb, 1000, NULL);
}

// ---- request screen --------------------------------------------------------

static void end_request(bool approved, const char *reason)
{
    if (!req_active) return;
    req_active = false;
    if (req_timer) {
        lv_timer_del(req_timer);
        req_timer = NULL;
    }
    lv_obj_t *old = scr_req;
    scr_req = NULL;
    lv_scr_load(scr_idle);
    if (old) lv_obj_del(old);
    if (approved) flash_result("APPROVED", 0x4CAF50);
    else if (strcmp(reason, "timeout") == 0) flash_result("Timed out", 0xFFB300);
    else flash_result("DENIED", 0xE53935);
    lv_disp_trig_activity(NULL);
    if (result_cb) result_cb(approved, reason);
}

static void approve_ev(lv_event_t *e) { end_request(true, "ok"); }
static void deny_ev(lv_event_t *e) { end_request(false, "denied"); }

static void panel_scroll_ev(lv_event_t *e)
{
    if (lv_obj_get_scroll_bottom(req_panel) <= 2) req_at_end = true;
}

static void req_tick_cb(lv_timer_t *t)
{
    int64_t el = (esp_timer_get_time() - req_start_us) / 1000;
    if (el >= req_timeout_ms) {
        end_request(false, "timeout");
        return;
    }
    lv_bar_set_value(req_bar, (int)((req_timeout_ms - el) * 1000 / req_timeout_ms), LV_ANIM_OFF);
    lv_label_set_text_fmt(req_count, "%ds", (int)((req_timeout_ms - el + 999) / 1000));

    bool ready = el >= APPROVE_DELAY_MS && (!req_overflow || req_at_end);
    bool enabled = !lv_obj_has_state(btn_approve, LV_STATE_DISABLED);
    if (ready && !enabled) lv_obj_clear_state(btn_approve, LV_STATE_DISABLED);
    if (req_hint) lv_obj_add_flag(req_hint, req_overflow && !req_at_end ? 0 : LV_OBJ_FLAG_HIDDEN);
    if (req_hint && req_overflow && !req_at_end) lv_obj_clear_flag(req_hint, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *mk_btn(lv_obj_t *parent, int x, uint32_t color, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 385, 68);
    lv_obj_set_pos(b, x, 404);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x404040), LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = mk_label(b, &lv_font_montserrat_32, 0xFFFFFF, text);
    lv_obj_center(l);
    return b;
}

static void show_request(struct req *r)
{
    if (req_active) end_request(false, "superseded");

    hw_backlight(true);
    lv_disp_trig_activity(NULL);

    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x0B0E11), 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(s);
    lv_obj_set_size(bar, LCD_W, 60);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xB00020), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = mk_label(bar, &lv_font_montserrat_32, 0xFFFFFF, "SUDO REQUEST");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    req_count = mk_label(bar, &lv_font_montserrat_32, 0xFFFFFF, "");
    lv_obj_align(req_count, LV_ALIGN_RIGHT_MID, 0, 0);

    char who[400];
    snprintf(who, sizeof who, "%s  in  %s", r->user, r->cwd);
    lv_obj_t *wl = mk_label(s, &lv_font_montserrat_24, 0xB0BEC5, who);
    lv_obj_set_width(wl, 780);
    lv_label_set_long_mode(wl, LV_LABEL_LONG_DOT);
    lv_obj_set_height(wl, 30);
    lv_obj_set_pos(wl, 10, 68);

    req_panel = lv_obj_create(s);
    lv_obj_set_size(req_panel, 780, 262);
    lv_obj_set_pos(req_panel, 10, 108);
    lv_obj_set_style_bg_color(req_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(req_panel, lv_color_hex(0x455A64), 0);
    lv_obj_set_style_border_width(req_panel, 2, 0);
    lv_obj_set_style_radius(req_panel, 4, 0);
    lv_obj_set_scrollbar_mode(req_panel, LV_SCROLLBAR_MODE_ON);
    lv_obj_t *cl = mk_label(req_panel, &lv_font_hack_20, 0xE0E0E0, r->cmd);
    lv_label_set_long_mode(cl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cl, 740);
    lv_obj_add_event_cb(req_panel, panel_scroll_ev, LV_EVENT_SCROLL, NULL);

    req_bar = lv_bar_create(s);
    lv_obj_set_size(req_bar, 780, 8);
    lv_obj_set_pos(req_bar, 10, 384);
    lv_bar_set_range(req_bar, 0, 1000);
    lv_bar_set_value(req_bar, 1000, LV_ANIM_OFF);

    req_hint = mk_label(s, &lv_font_montserrat_24, 0xFFB300, "Scroll to the end of the command to enable Approve");
    lv_obj_set_pos(req_hint, 10, 350);
    lv_obj_add_flag(req_hint, LV_OBJ_FLAG_HIDDEN);

    mk_btn(s, 10, 0x616161, "DENY", deny_ev);
    btn_approve = mk_btn(s, 405, 0x2E7D32, "APPROVE", approve_ev);
    lv_obj_add_state(btn_approve, LV_STATE_DISABLED);

    lv_obj_update_layout(s);
    req_overflow = lv_obj_get_scroll_bottom(req_panel) > 2;
    req_at_end = false;

    req_timeout_ms = r->timeout_s * 1000;
    req_start_us = esp_timer_get_time();
    scr_req = s;
    req_active = true;
    lv_scr_load(s);
    req_timer = lv_timer_create(req_tick_cb, 100, NULL);
    req_tick_cb(req_timer);
}

// ---- task / public API -----------------------------------------------------

static void handle_evt(evt_t *e)
{
    switch (e->type) {
    case EVT_REQUEST:
        show_request(e->req);
        free(e->req->cmd);
        free(e->req);
        break;
    case EVT_PROVISIONED:
        provisioned = e->flag;
        if (!flash_timer && !req_active) reset_idle_status();
        break;
    }
}

static void ui_task(void *arg)
{
    glue_init();
    build_idle();
    lv_scr_load(scr_idle);
    hw_backlight(true);
    ESP_LOGI(TAG, "ui running");

    for (;;) {
        evt_t e;
        while (xQueueReceive(evq, &e, 0) == pdTRUE) handle_evt(&e);
        uint32_t d = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(d < 5 ? 5 : (d > 30 ? 30 : d)));
    }
}

void ui_start(ui_result_cb_t on_result, ui_forget_cb_t on_forget)
{
    result_cb = on_result;
    forget_cb = on_forget;
    evq = xQueueCreate(8, sizeof(evt_t));
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 4, NULL, 1);
}

void ui_set_provisioned(bool p)
{
    evt_t e = {.type = EVT_PROVISIONED, .flag = p};
    xQueueSend(evq, &e, pdMS_TO_TICKS(100));
}

void ui_show_request(const char *user, const char *cwd, const char *cmd, int timeout_s)
{
    struct req *r = calloc(1, sizeof *r);
    if (!r) return;
    strlcpy(r->user, user, sizeof r->user);
    strlcpy(r->cwd, cwd, sizeof r->cwd);
    r->cmd = strdup(cmd);
    r->timeout_s = timeout_s;
    evt_t e = {.type = EVT_REQUEST, .req = r};
    if (!r->cmd || xQueueSend(evq, &e, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(r->cmd);
        free(r);
    }
}
