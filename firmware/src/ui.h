#pragma once
#include <stdbool.h>

typedef void (*ui_result_cb_t)(bool approved, const char *reason);
typedef void (*ui_forget_cb_t)(void);

// Starts the LVGL task. Hardware (hw_init) must be up.
void ui_start(ui_result_cb_t on_result, ui_forget_cb_t on_forget);

// Thread-safe: these queue work for the LVGL task.
void ui_set_provisioned(bool provisioned);
// All strings must already be sanitised printable ASCII.
void ui_show_request(const char *user, const char *cwd, const char *cmd, int timeout_s);
