#include "wifi_settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_NAMESPACE "smart_clock"
#define WIFI_MAX_RETRIES 5
#define WIFI_REFRESH_UNIT_VERSION 2

static bool s_initialized;
static bool s_should_reconnect;
static int s_retry_count;
static wifi_connection_state_t s_state = WIFI_CONNECTION_IDLE;
static char s_connected_ssid[WIFI_SETTINGS_SSID_MAX_LEN + 1];
static char s_pending_ssid[WIFI_SETTINGS_SSID_MAX_LEN + 1];
static char s_pending_password[WIFI_SETTINGS_PASSWORD_MAX_LEN + 1];
static char s_ip_address[16];
static wifi_err_reason_t s_last_reason = WIFI_REASON_UNSPECIFIED;

static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

static esp_err_t open_namespace(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    return nvs_open(WIFI_NAMESPACE, mode, handle);
}

static esp_err_t read_string(nvs_handle_t handle, const char *key, char *buffer, size_t buffer_len)
{
    size_t required = buffer_len;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required);
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        buffer[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t write_string(nvs_handle_t handle, const char *key, const char *value)
{
    return nvs_set_str(handle, key, value ? value : "");
}

static uint16_t clamp_refresh_minutes(uint16_t value, uint16_t fallback)
{
    if(value < WIFI_SETTINGS_MIN_REFRESH_MINUTES || value > WIFI_SETTINGS_MAX_REFRESH_MINUTES) {
        return fallback;
    }
    return value;
}

static uint16_t clamp_refresh_seconds(uint16_t value, uint16_t fallback)
{
    if(value < WIFI_SETTINGS_MIN_REFRESH_SECONDS || value > WIFI_SETTINGS_MAX_REFRESH_SECONDS) {
        return fallback;
    }
    return value;
}

static void set_default_preferences(app_preferences_t *prefs)
{
    memset(prefs, 0, sizeof(*prefs));
    prefs->orientation = 2;
    prefs->brightness = 100;
    prefs->weather_refresh_minutes = 30;
    prefs->bus_refresh_seconds = 120;
    prefs->subway_refresh_seconds = 120;
    prefs->finance_refresh_minutes = 15;
    strlcpy(prefs->timezone, "KST-9", sizeof(prefs->timezone));
    strlcpy(prefs->weather_location, "37.5665,126.9780", sizeof(prefs->weather_location));
    strlcpy(prefs->bus_stop_id, "12012", sizeof(prefs->bus_stop_id));
    strlcpy(prefs->subway_station, "\xEA\xB0\x95\xEB\x82\xA8", sizeof(prefs->subway_station));
    strlcpy(prefs->finance_ticker, "^KS11", sizeof(prefs->finance_ticker));
}

void wifi_settings_sanitize_preferences(app_preferences_t *prefs)
{
    if(prefs == NULL) {
        return;
    }

    if(prefs->orientation != 2 && prefs->orientation != 3) {
        prefs->orientation = 2;
    }

    if(prefs->brightness < 10 || prefs->brightness > 100) {
        prefs->brightness = 100;
    }

    prefs->weather_refresh_minutes = clamp_refresh_minutes(prefs->weather_refresh_minutes, 30);
    prefs->bus_refresh_seconds = clamp_refresh_seconds(prefs->bus_refresh_seconds, 120);
    prefs->subway_refresh_seconds = clamp_refresh_seconds(prefs->subway_refresh_seconds, 120);
    prefs->finance_refresh_minutes = clamp_refresh_minutes(prefs->finance_refresh_minutes, 15);

    if(prefs->timezone[0] == '\0') {
        strlcpy(prefs->timezone, "KST-9", sizeof(prefs->timezone));
    }
    if(prefs->weather_location[0] == '\0') {
        strlcpy(prefs->weather_location, "37.5665,126.9780", sizeof(prefs->weather_location));
    }
    if(prefs->bus_stop_id[0] == '\0') {
        strlcpy(prefs->bus_stop_id, "12012", sizeof(prefs->bus_stop_id));
    }
    if(prefs->subway_station[0] == '\0') {
        strlcpy(prefs->subway_station, "\xEA\xB0\x95\xEB\x82\xA8", sizeof(prefs->subway_station));
    }
    if(prefs->finance_ticker[0] == '\0') {
        strlcpy(prefs->finance_ticker, "^KS11", sizeof(prefs->finance_ticker));
    }
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = open_namespace(&handle, NVS_READWRITE);
    if(err != ESP_OK) {
        return err;
    }

    err = write_string(handle, "ssid", ssid);
    if(err == ESP_OK) {
        err = write_string(handle, "password", password);
    }
    if(err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = event_data;
        s_last_reason = disconnected->reason;
        s_ip_address[0] = '\0';

        if(s_should_reconnect && s_retry_count < WIFI_MAX_RETRIES) {
            s_retry_count++;
            s_state = WIFI_CONNECTION_CONNECTING;
            esp_wifi_connect();
            return;
        }

        s_state = s_retry_count == 0 ? WIFI_CONNECTION_IDLE : WIFI_CONNECTION_FAILED;
    }

    if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = event_data;
        s_retry_count = 0;
        s_state = WIFI_CONNECTION_CONNECTED;
        esp_ip4addr_ntoa(&got_ip->ip_info.ip, s_ip_address, sizeof(s_ip_address));
        strlcpy(s_connected_ssid, s_pending_ssid, sizeof(s_connected_ssid));
        save_wifi_credentials(s_pending_ssid, s_pending_password);
    }
}

esp_err_t wifi_settings_init(void)
{
    if(s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
                                                        &s_wifi_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
                                                        &s_ip_event_instance));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_settings_load_preferences(app_preferences_t *prefs)
{
    if(prefs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    set_default_preferences(prefs);

    nvs_handle_t handle;
    esp_err_t err = open_namespace(&handle, NVS_READONLY);
    bool legacy_refresh_units = false;
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if(err != ESP_OK) {
        return err;
    }

    err = read_string(handle, "ssid", prefs->ssid, sizeof(prefs->ssid));
    if(err == ESP_OK) {
        err = read_string(handle, "password", prefs->password, sizeof(prefs->password));
    }
    if(err == ESP_OK) {
        uint8_t value = prefs->orientation;
        err = nvs_get_u8(handle, "orient", &value);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        prefs->orientation = value;
    }
    if(err == ESP_OK) {
        uint8_t value = prefs->brightness;
        err = nvs_get_u8(handle, "bright", &value);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        prefs->brightness = value;
    }
    if(err == ESP_OK) {
        err = read_string(handle, "tz", prefs->timezone, sizeof(prefs->timezone));
    }
    if(err == ESP_OK) {
        err = read_string(handle, "wxloc", prefs->weather_location, sizeof(prefs->weather_location));
    }
    if(err == ESP_OK) {
        err = read_string(handle, "busstop", prefs->bus_stop_id, sizeof(prefs->bus_stop_id));
    }
    if(err == ESP_OK) {
        err = read_string(handle, "subway", prefs->subway_station, sizeof(prefs->subway_station));
    }
    if(err == ESP_OK) {
        err = read_string(handle, "ticker", prefs->finance_ticker, sizeof(prefs->finance_ticker));
    }
    if(err == ESP_OK) {
        uint16_t value = prefs->weather_refresh_minutes;
        err = nvs_get_u16(handle, "wxint", &value);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        prefs->weather_refresh_minutes = value;
    }
    if(err == ESP_OK) {
        uint16_t value = prefs->bus_refresh_seconds;
        err = nvs_get_u16(handle, "busint", &value);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        prefs->bus_refresh_seconds = value;
    }
    if(err == ESP_OK) {
        uint16_t value = prefs->subway_refresh_seconds;
        err = nvs_get_u16(handle, "subint", &value);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        prefs->subway_refresh_seconds = value;
    }
    if(err == ESP_OK) {
        uint16_t value = prefs->finance_refresh_minutes;
        err = nvs_get_u16(handle, "finint", &value);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        prefs->finance_refresh_minutes = value;
    }
    if(err == ESP_OK) {
        uint8_t value = 0;
        err = nvs_get_u8(handle, "rver", &value);
        if(err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
            legacy_refresh_units = true;
        }
    }

    nvs_close(handle);
    if(err == ESP_OK) {
        if(legacy_refresh_units) {
            prefs->bus_refresh_seconds *= 60;
            prefs->subway_refresh_seconds *= 60;
        }
        wifi_settings_sanitize_preferences(prefs);
        if(legacy_refresh_units) {
            err = wifi_settings_save_preferences(prefs);
        }
    }
    return err;
}

esp_err_t wifi_settings_save_preferences(const app_preferences_t *prefs)
{
    if(prefs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_preferences_t sanitized = *prefs;
    wifi_settings_sanitize_preferences(&sanitized);

    nvs_handle_t handle;
    esp_err_t err = open_namespace(&handle, NVS_READWRITE);
    if(err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, "orient", sanitized.orientation);
    if(err == ESP_OK) err = nvs_set_u8(handle, "bright", sanitized.brightness);
    if(err == ESP_OK) err = write_string(handle, "tz", sanitized.timezone);
    if(err == ESP_OK) err = write_string(handle, "wxloc", sanitized.weather_location);
    if(err == ESP_OK) err = write_string(handle, "busstop", sanitized.bus_stop_id);
    if(err == ESP_OK) err = write_string(handle, "subway", sanitized.subway_station);
    if(err == ESP_OK) err = write_string(handle, "ticker", sanitized.finance_ticker);
    if(err == ESP_OK) err = nvs_set_u16(handle, "wxint", sanitized.weather_refresh_minutes);
    if(err == ESP_OK) err = nvs_set_u16(handle, "busint", sanitized.bus_refresh_seconds);
    if(err == ESP_OK) err = nvs_set_u16(handle, "subint", sanitized.subway_refresh_seconds);
    if(err == ESP_OK) err = nvs_set_u16(handle, "finint", sanitized.finance_refresh_minutes);
    if(err == ESP_OK) err = nvs_set_u8(handle, "rver", WIFI_REFRESH_UNIT_VERSION);
    if(err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

esp_err_t wifi_settings_connect(const char *ssid, const char *password)
{
    if(!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if(ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password ? password : "", sizeof(wifi_config.sta.password));
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_password, password ? password : "", sizeof(s_pending_password));
    s_should_reconnect = true;
    s_retry_count = 0;
    s_state = WIFI_CONNECTION_CONNECTING;
    s_last_reason = WIFI_REASON_UNSPECIFIED;
    s_ip_address[0] = '\0';

    esp_err_t err = esp_wifi_disconnect();
    if(err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    return esp_wifi_connect();
}

wifi_connection_state_t wifi_settings_get_state(void)
{
    return s_state;
}

bool wifi_settings_is_connected(void)
{
    return s_state == WIFI_CONNECTION_CONNECTED;
}

void wifi_settings_get_status_text(char *buffer, size_t buffer_len)
{
    if(buffer == NULL || buffer_len == 0) {
        return;
    }

    switch(s_state) {
    case WIFI_CONNECTION_IDLE:
        snprintf(buffer, buffer_len, "Enter SSID and password to connect.");
        break;
    case WIFI_CONNECTION_CONNECTING:
        snprintf(buffer, buffer_len, "Connecting to %s...", s_pending_ssid[0] ? s_pending_ssid : "Wi-Fi");
        break;
    case WIFI_CONNECTION_CONNECTED:
        snprintf(buffer, buffer_len, "Connected to %s (%s)", s_connected_ssid, s_ip_address);
        break;
    case WIFI_CONNECTION_FAILED:
        snprintf(buffer, buffer_len, "Connection failed. Reason: %d", s_last_reason);
        break;
    default:
        snprintf(buffer, buffer_len, "Unknown Wi-Fi state");
        break;
    }
}

void wifi_settings_get_ip_address(char *buffer, size_t buffer_len)
{
    if(buffer == NULL || buffer_len == 0) {
        return;
    }

    strlcpy(buffer, s_ip_address, buffer_len);
}
