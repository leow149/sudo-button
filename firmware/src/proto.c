#include "proto.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "proto";

#define MAX_LINE 4600
#define MAX_PAYLOAD 3072
#define MIN_TIMEOUT 5
#define MAX_TIMEOUT 60

static uint8_t secret[32];
static bool provisioned;

static struct {
    bool active;
    char nonce[33];
    uint8_t *payload;
    size_t plen;
} pend;
static SemaphoreHandle_t mtx;

static void out(const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    usb_serial_jtag_write_bytes(b, n, pdMS_TO_TICKS(200));
}

// ---- secret storage --------------------------------------------------------

static void load_secret(void)
{
    nvs_handle_t h;
    size_t len = sizeof secret;
    provisioned = false;
    if (nvs_open("sbtn", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "secret", secret, &len) == ESP_OK && len == sizeof secret) provisioned = true;
    nvs_close(h);
}

static bool store_secret(const uint8_t *s)
{
    nvs_handle_t h;
    if (nvs_open("sbtn", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "secret", s, 32) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void proto_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("sbtn", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "secret");
        nvs_commit(h);
        nvs_close(h);
    }
    memset(secret, 0, sizeof secret);
    provisioned = false;
}

// ---- helpers ---------------------------------------------------------------

static bool is_hex(const char *s, size_t n)
{
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

// Render untrusted bytes as printable ASCII only: anything else becomes \xNN,
// so nothing can hide in the command text (lookalikes, control chars, CR...).
static char *sanitise(const uint8_t *s, size_t n, bool keep_newlines)
{
    char *o = malloc(n * 4 + 1), *p = o;
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '\n' && keep_newlines) {
            *p++ = '\\';
            *p++ = 'n';
            *p++ = '\n';
        } else if (c == '\\') {
            *p++ = '\\';
            *p++ = '\\';
        } else if (c >= 0x20 && c <= 0x7e) {
            *p++ = c;
        } else if (c == '\n') {
            *p++ = '\\';
            *p++ = 'n';
        } else if (c == '\t') {
            *p++ = '\\';
            *p++ = 't';
        } else {
            p += sprintf(p, "\\x%02x", c);
        }
    }
    *p = 0;
    return o;
}

static void compute_hmac(const char *nonce, const uint8_t *payload, size_t plen, uint8_t outm[32])
{
    mbedtls_md_context_t c;
    mbedtls_md_init(&c);
    mbedtls_md_setup(&c, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&c, secret, sizeof secret);
    mbedtls_md_hmac_update(&c, (const uint8_t *)"sudo-btn-v1\n", 12);
    mbedtls_md_hmac_update(&c, (const uint8_t *)nonce, 32);
    mbedtls_md_hmac_update(&c, (const uint8_t *)"\n", 1);
    mbedtls_md_hmac_update(&c, payload, plen);
    mbedtls_md_hmac_finish(&c, outm);
    mbedtls_md_free(&c);
}

// ---- request handling ------------------------------------------------------

void proto_result(bool approved, const char *reason)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (pend.active) {
        if (approved) {
            uint8_t mac[32];
            char hex[65];
            compute_hmac(pend.nonce, pend.payload, pend.plen, mac);
            for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", mac[i]);
            out("@OK %s %s", pend.nonce, hex);
        } else {
            out("@NO %s %s", pend.nonce, reason);
        }
        free(pend.payload);
        pend.payload = NULL;
        pend.active = false;
    }
    xSemaphoreGive(mtx);
}

static void handle_req(char *args)
{
    char *sp = NULL;
    char *nonce = strtok_r(args, " ", &sp);
    char *tmo = strtok_r(NULL, " ", &sp);
    char *b64 = strtok_r(NULL, " ", &sp);
    if (!nonce || !is_hex(nonce, 32)) {
        out("@NO %s badreq", nonce ? "-" : "-");
        return;
    }
    if (!tmo || !b64) {
        out("@NO %s badreq", nonce);
        return;
    }
    if (!provisioned) {
        out("@NO %s unprovisioned", nonce);
        return;
    }

    xSemaphoreTake(mtx, portMAX_DELAY);
    bool busy = pend.active;
    xSemaphoreGive(mtx);
    if (busy) {
        out("@NO %s busy", nonce);
        return;
    }

    int secs = atoi(tmo);
    if (secs < MIN_TIMEOUT) secs = MIN_TIMEOUT;
    if (secs > MAX_TIMEOUT) secs = MAX_TIMEOUT;

    uint8_t *pl = malloc(MAX_PAYLOAD + 1);
    size_t plen = 0;
    if (!pl || mbedtls_base64_decode(pl, MAX_PAYLOAD, &plen, (const uint8_t *)b64, strlen(b64)) != 0 || plen == 0) {
        free(pl);
        out("@NO %s badreq", nonce);
        return;
    }

    // payload = user \n cwd \n command
    uint8_t *nl1 = memchr(pl, '\n', plen);
    uint8_t *nl2 = nl1 ? memchr(nl1 + 1, '\n', plen - (nl1 + 1 - pl)) : NULL;
    if (!nl1 || !nl2) {
        free(pl);
        out("@NO %s badreq", nonce);
        return;
    }
    char *user = sanitise(pl, nl1 - pl, false);
    char *cwd = sanitise(nl1 + 1, nl2 - nl1 - 1, false);
    char *cmd = sanitise(nl2 + 1, plen - (nl2 + 1 - pl), true);
    if (!user || !cwd || !cmd) {
        free(user);
        free(cwd);
        free(cmd);
        free(pl);
        out("@NO %s nomem", nonce);
        return;
    }

    xSemaphoreTake(mtx, portMAX_DELAY);
    memcpy(pend.nonce, nonce, 32);
    pend.nonce[32] = 0;
    pend.payload = pl;
    pend.plen = plen;
    pend.active = true;
    xSemaphoreGive(mtx);

    ui_show_request(user, cwd, cmd, secs);
    free(user);
    free(cwd);
    free(cmd);
}

static void handle_prov(char *arg)
{
    if (provisioned) {
        out("@PROVERR already");
        return;
    }
    if (!arg || !is_hex(arg, 64)) {
        out("@PROVERR badkey");
        return;
    }
    uint8_t s[32];
    for (int i = 0; i < 32; i++) s[i] = hexval(arg[i * 2]) << 4 | hexval(arg[i * 2 + 1]);
    if (!store_secret(s)) {
        out("@PROVERR nvs");
        return;
    }
    memcpy(secret, s, sizeof s);
    memset(s, 0, sizeof s);
    provisioned = true;
    ui_set_provisioned(true);
    out("@PROVOK");
}

static void handle_line(char *line)
{
    if (!strncmp(line, "REQ ", 4)) handle_req(line + 4);
    else if (!strncmp(line, "PROV ", 5)) handle_prov(line + 5);
    else if (!strcmp(line, "PING")) out("@PONG v1 %d", provisioned);
    // anything else (incl. ROM boot chatter) is ignored
}

static void serial_task(void *arg)
{
    static char line[MAX_LINE];
    size_t len = 0;
    bool overflow = false;
    uint8_t buf[256];

    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (!overflow) {
                    line[len] = 0;
                    handle_line(line);
                }
                len = 0;
                overflow = false;
            } else if (c != '\r') {
                if (len < MAX_LINE - 1) line[len++] = c;
                else overflow = true;
            }
        }
    }
}

void proto_init(void)
{
    mtx = xSemaphoreCreateMutex();
    load_secret();
    ui_set_provisioned(provisioned);

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;
    cfg.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    ESP_LOGI(TAG, "provisioned=%d", provisioned);
    xTaskCreate(serial_task, "serial", 6144, NULL, 5, NULL);
}
