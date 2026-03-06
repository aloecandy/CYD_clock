#ifndef WIFI_SETTINGS_H
#define WIFI_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_SETTINGS_SSID_MAX_LEN 32
#define WIFI_SETTINGS_PASSWORD_MAX_LEN 64
#define WIFI_SETTINGS_TIMEZONE_MAX_LEN 48
#define WIFI_SETTINGS_LOCATION_MAX_LEN 32
#define WIFI_SETTINGS_BUS_STOP_MAX_LEN 16
#define WIFI_SETTINGS_SUBWAY_MAX_LEN 32
#define WIFI_SETTINGS_TICKER_MAX_LEN 16
#define WIFI_SETTINGS_MIN_REFRESH_MINUTES 1
#define WIFI_SETTINGS_MAX_REFRESH_MINUTES 240

typedef enum {
    WIFI_CONNECTION_IDLE = 0,
    WIFI_CONNECTION_CONNECTING,
    WIFI_CONNECTION_CONNECTED,
    WIFI_CONNECTION_FAILED,
} wifi_connection_state_t;

typedef struct {
    char ssid[WIFI_SETTINGS_SSID_MAX_LEN + 1];
    char password[WIFI_SETTINGS_PASSWORD_MAX_LEN + 1];
    char timezone[WIFI_SETTINGS_TIMEZONE_MAX_LEN + 1];
    char weather_location[WIFI_SETTINGS_LOCATION_MAX_LEN + 1];
    char bus_stop_id[WIFI_SETTINGS_BUS_STOP_MAX_LEN + 1];
    char subway_station[WIFI_SETTINGS_SUBWAY_MAX_LEN + 1];
    char finance_ticker[WIFI_SETTINGS_TICKER_MAX_LEN + 1];
    uint8_t orientation;
    uint8_t brightness;
    uint16_t weather_refresh_minutes;
    uint16_t bus_refresh_minutes;
    uint16_t subway_refresh_minutes;
    uint16_t finance_refresh_minutes;
} app_preferences_t;

esp_err_t wifi_settings_init(void);
esp_err_t wifi_settings_load_preferences(app_preferences_t *prefs);
esp_err_t wifi_settings_save_preferences(const app_preferences_t *prefs);
esp_err_t wifi_settings_connect(const char *ssid, const char *password);

wifi_connection_state_t wifi_settings_get_state(void);
bool wifi_settings_is_connected(void);
void wifi_settings_get_status_text(char *buffer, size_t buffer_len);
void wifi_settings_get_ip_address(char *buffer, size_t buffer_len);
void wifi_settings_sanitize_preferences(app_preferences_t *prefs);

#endif
