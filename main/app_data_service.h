#ifndef APP_DATA_SERVICE_H
#define APP_DATA_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "wifi_settings.h"

#define APP_MAX_BUS_ITEMS 5
#define APP_MAX_SUBWAY_ITEMS 5
#define APP_MAX_FINANCE_POINTS 20

typedef enum {
    APP_REFRESH_NONE = 0,
    APP_REFRESH_WEATHER = 1 << 0,
    APP_REFRESH_BUS = 1 << 1,
    APP_REFRESH_SUBWAY = 1 << 2,
    APP_REFRESH_FINANCE = 1 << 3,
    APP_REFRESH_ALL = APP_REFRESH_WEATHER | APP_REFRESH_BUS | APP_REFRESH_SUBWAY | APP_REFRESH_FINANCE,
} app_refresh_flags_t;

typedef struct {
    char route[20];
    char arrival1[40];
    char arrival2[40];
    char direction[24];
} app_bus_item_t;

typedef struct {
    char line[48];
    char arrival[40];
    char destination[24];
} app_subway_item_t;

typedef struct {
    bool weather_valid;
    bool bus_valid;
    bool subway_valid;
    bool finance_valid;
    char weather_target[12];
    char weather_icon[12];
    char weather_summary[48];
    char weather_temp[48];
    char air_quality[48];
    time_t weather_updated_at;
    char stop_name[32];
    time_t bus_updated_at;
    char station_name[32];
    time_t subway_updated_at;
    char finance_name[32];
    char finance_price[40];
    char finance_ticker[WIFI_SETTINGS_TICKER_MAX_LEN + 1];
    time_t finance_updated_at;
    app_bus_item_t bus_items[APP_MAX_BUS_ITEMS];
    int bus_count;
    app_subway_item_t subway_items[APP_MAX_SUBWAY_ITEMS];
    int subway_count;
    int finance_points;
    int32_t finance_series[APP_MAX_FINANCE_POINTS];
} app_data_snapshot_t;

esp_err_t app_data_service_init(const app_preferences_t *prefs);
void app_data_service_set_preferences(const app_preferences_t *prefs);
void app_data_service_set_wifi_ready(bool ready);
void app_data_service_request_refresh(uint32_t flags);
void app_data_service_get_snapshot(app_data_snapshot_t *snapshot);

#endif
