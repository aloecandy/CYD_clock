#include "settings_web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "settings_web_server";

#define SETTINGS_HTML_BUFFER_SIZE 4096
#define SETTINGS_POST_BODY_MAX_LEN 2048
#define SETTINGS_HTTPD_STACK_SIZE 8192
#define SETTINGS_HTTPD_MAX_REQ_HDR_LEN 2048

static httpd_handle_t s_server;
static bool s_enabled;
static app_preferences_t s_current_preferences;
static app_preferences_t s_pending_preferences;
static bool s_has_pending_update;
static bool s_pending_wifi_change;

static char from_hex(char value)
{
    if(value >= '0' && value <= '9') return (char)(value - '0');
    if(value >= 'a' && value <= 'f') return (char)(value - 'a' + 10);
    if(value >= 'A' && value <= 'F') return (char)(value - 'A' + 10);
    return 0;
}

static void url_decode(char *text)
{
    char *src = text;
    char *dst = text;

    while(*src != '\0') {
        if(src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            *dst++ = (char)((from_hex(src[1]) << 4) | from_hex(src[2]));
            src += 3;
        } else if(*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}

static bool get_form_value(char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    char *cursor = body;

    while(cursor != NULL && *cursor != '\0') {
        char *next = strchr(cursor, '&');
        char *value = strchr(cursor, '=');
        size_t name_len = value != NULL ? (size_t)(value - cursor) : strlen(cursor);
        if(value != NULL && name_len == key_len && strncmp(cursor, key, key_len) == 0) {
            size_t value_len = next != NULL ? (size_t)(next - value - 1) : strlen(value + 1);
            if(value_len >= out_len) {
                value_len = out_len - 1;
            }
            memcpy(out, value + 1, value_len);
            out[value_len] = '\0';
            url_decode(out);
            return true;
        }

        cursor = next != NULL ? next + 1 : NULL;
    }

    out[0] = '\0';
    return false;
}

static uint16_t parse_u16_or_default(const char *text, uint16_t fallback)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if(text == end || value < 0 || value > 65535) {
        return fallback;
    }
    return (uint16_t)value;
}

static const char *selected_attr(bool selected)
{
    return selected ? " selected" : "";
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char *html = malloc(SETTINGS_HTML_BUFFER_SIZE);
    if(html == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    app_preferences_t prefs = s_current_preferences;

    int written = snprintf(
        html, SETTINGS_HTML_BUFFER_SIZE,
        "<!doctype html><html><head><meta charset='utf-8'><title>Smart Clock Settings</title>"
        "<style>body{font-family:sans-serif;max-width:760px;margin:24px auto;padding:0 16px;}"
        "label{display:block;margin:10px 0 4px;}input,select{width:100%%;padding:10px;font-size:16px;}"
        "button{margin-top:18px;padding:12px 18px;font-size:16px;}small{color:#555;}</style></head><body>"
        "<h2>Smart Clock Settings</h2><form method='post' action='/save'>"
        "<label>SSID</label><input name='ssid' value='%s'>"
        "<label>Password</label><input name='password' type='password' value='%s'>"
        "<label>Orientation</label><select name='orientation'>"
        "<option value='2'%s>Landscape</option><option value='3'%s>Landscape Inverted</option></select>"
        "<label>Brightness (10-100)</label><input name='brightness' type='number' min='10' max='100' value='%u'>"
        "<label>Timezone</label><input name='timezone' value='%s'><small>Example: KST-9</small>"
        "<label>Weather Region</label><input name='weather_location' value='%s'><small>Format: lat,lon</small>"
        "<label>Bus Stop ARS ID</label><input name='bus_stop_id' value='%s'>"
        "<label>Subway Station</label><input name='subway_station' value='%s'><small>Use the Korean station name.</small>"
        "<label>Yahoo Ticker</label><input name='finance_ticker' value='%s'>"
        "<label>Weather Refresh (min)</label><input name='weather_refresh' type='number' min='1' max='240' value='%u'>"
        "<label>Bus Refresh While Viewing (sec)</label><input name='bus_refresh' type='number' min='1' max='3600' value='%u'>"
        "<label>Subway Refresh While Viewing (sec)</label><input name='subway_refresh' type='number' min='1' max='3600' value='%u'>"
        "<label>Finance Refresh (min)</label><input name='finance_refresh' type='number' min='1' max='240' value='%u'>"
        "<button type='submit'>Save</button></form></body></html>",
        prefs.ssid,
        prefs.password,
        selected_attr(prefs.orientation == 2),
        selected_attr(prefs.orientation == 3),
        prefs.brightness,
        prefs.timezone,
        prefs.weather_location,
        prefs.bus_stop_id,
        prefs.subway_station,
        prefs.finance_ticker,
        prefs.weather_refresh_minutes,
        prefs.bus_refresh_seconds,
        prefs.subway_refresh_seconds,
        prefs.finance_refresh_minutes);

    if(written < 0 || written >= SETTINGS_HTML_BUFFER_SIZE) {
        free(html);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTML buffer too small");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if(req->content_len <= 0 || req->content_len >= SETTINGS_POST_BODY_MAX_LEN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body size");
    }

    char *body = malloc((size_t)req->content_len + 1);
    if(body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    int total_received = 0;
    while(total_received < req->content_len) {
        int received = httpd_req_recv(req, body + total_received, req->content_len - total_received);
        if(received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if(received <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read form body");
        }
        total_received += received;
    }

    body[total_received] = '\0';
    app_preferences_t prefs = s_current_preferences;
    char temp[96];

    if(get_form_value(body, "ssid", temp, sizeof(temp))) {
        strlcpy(prefs.ssid, temp, sizeof(prefs.ssid));
    }
    if(get_form_value(body, "password", temp, sizeof(temp))) {
        strlcpy(prefs.password, temp, sizeof(prefs.password));
    }
    if(get_form_value(body, "timezone", temp, sizeof(temp))) {
        strlcpy(prefs.timezone, temp, sizeof(prefs.timezone));
    }
    if(get_form_value(body, "weather_location", temp, sizeof(temp))) {
        strlcpy(prefs.weather_location, temp, sizeof(prefs.weather_location));
    }
    if(get_form_value(body, "bus_stop_id", temp, sizeof(temp))) {
        strlcpy(prefs.bus_stop_id, temp, sizeof(prefs.bus_stop_id));
    }
    if(get_form_value(body, "subway_station", temp, sizeof(temp))) {
        strlcpy(prefs.subway_station, temp, sizeof(prefs.subway_station));
    }
    if(get_form_value(body, "finance_ticker", temp, sizeof(temp))) {
        strlcpy(prefs.finance_ticker, temp, sizeof(prefs.finance_ticker));
    }
    if(get_form_value(body, "orientation", temp, sizeof(temp))) {
        prefs.orientation = (uint8_t)parse_u16_or_default(temp, prefs.orientation);
    }
    if(get_form_value(body, "brightness", temp, sizeof(temp))) {
        prefs.brightness = (uint8_t)parse_u16_or_default(temp, prefs.brightness);
    }
    if(get_form_value(body, "weather_refresh", temp, sizeof(temp))) {
        prefs.weather_refresh_minutes = parse_u16_or_default(temp, prefs.weather_refresh_minutes);
    }
    if(get_form_value(body, "bus_refresh", temp, sizeof(temp))) {
        prefs.bus_refresh_seconds = parse_u16_or_default(temp, prefs.bus_refresh_seconds);
    }
    if(get_form_value(body, "subway_refresh", temp, sizeof(temp))) {
        prefs.subway_refresh_seconds = parse_u16_or_default(temp, prefs.subway_refresh_seconds);
    }
    if(get_form_value(body, "finance_refresh", temp, sizeof(temp))) {
        prefs.finance_refresh_minutes = parse_u16_or_default(temp, prefs.finance_refresh_minutes);
    }

    wifi_settings_sanitize_preferences(&prefs);
    if(wifi_settings_save_preferences(&prefs) != ESP_OK) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
    }

    s_pending_wifi_change = strcmp(s_current_preferences.ssid, prefs.ssid) != 0 ||
                            strcmp(s_current_preferences.password, prefs.password) != 0;
    s_current_preferences = prefs;
    s_pending_preferences = prefs;
    s_has_pending_update = true;
    free(body);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
                              "<!doctype html><html><head><meta charset='utf-8'>"
                              "<meta http-equiv='refresh' content='1; url=/'></head>"
                              "<body><p>Saved. Returning to settings page...</p></body></html>");
}

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = SETTINGS_HTTPD_STACK_SIZE;
    config.max_req_hdr_len = SETTINGS_HTTPD_MAX_REQ_HDR_LEN;
    config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&s_server, &config);
    if(err != ESP_OK) {
        return err;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &save_uri));
    ESP_LOGI(TAG, "Settings web server started");
    return ESP_OK;
}

static void stop_server(void)
{
    if(s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Settings web server stopped");
    }
}

esp_err_t settings_web_server_init(void)
{
    memset(&s_current_preferences, 0, sizeof(s_current_preferences));
    memset(&s_pending_preferences, 0, sizeof(s_pending_preferences));
    return ESP_OK;
}

void settings_web_server_set_enabled(bool enabled)
{
    if(enabled == s_enabled) {
        return;
    }

    s_enabled = enabled;
    if(enabled) {
        if(start_server() != ESP_OK) {
            s_enabled = false;
        }
    } else {
        stop_server();
    }
}

void settings_web_server_set_current_preferences(const app_preferences_t *prefs)
{
    if(prefs != NULL) {
        s_current_preferences = *prefs;
    }
}

bool settings_web_server_consume_pending_update(app_preferences_t *prefs, bool *wifi_changed)
{
    if(!s_has_pending_update) {
        return false;
    }

    if(prefs != NULL) {
        *prefs = s_pending_preferences;
    }
    if(wifi_changed != NULL) {
        *wifi_changed = s_pending_wifi_change;
    }

    s_has_pending_update = false;
    s_pending_wifi_change = false;
    return true;
}
