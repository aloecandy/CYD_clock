#include "app_data_service.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BUS_API_KEY "ptRjIMHrjuCyMweSf4pkfDr605tdaww0uy5XABjuE9Jj0bWy59X0XGKDidiwwdBsweO2kGFoJX5IiWwyJV7c%2Fw%3D%3D"
#define SUBWAY_API_KEY "7342777a6d65746836354d424c6947"
#define APP_DATA_TASK_STACK_SIZE 12288

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buffer_t;

static const char *TAG = "app_data_service";

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static app_preferences_t s_preferences;
static app_data_snapshot_t s_snapshot;
static bool s_wifi_ready;
static bool s_sntp_started;
static uint32_t s_pending_refresh_flags = APP_REFRESH_WEATHER | APP_REFRESH_FINANCE;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buffer_t *buffer = evt->user_data;
    if(buffer == NULL) {
        return ESP_OK;
    }

    if(evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t required = buffer->len + evt->data_len + 1;
        if(required > buffer->cap) {
            size_t new_cap = buffer->cap == 0 ? 512 : buffer->cap;
            while(new_cap < required) {
                new_cap *= 2;
            }

            char *new_data = realloc(buffer->data, new_cap);
            if(new_data == NULL) {
                return ESP_ERR_NO_MEM;
            }

            buffer->data = new_data;
            buffer->cap = new_cap;
        }

        memcpy(buffer->data + buffer->len, evt->data, evt->data_len);
        buffer->len += evt->data_len;
        buffer->data[buffer->len] = '\0';
    }

    return ESP_OK;
}

static void http_buffer_free(http_buffer_t *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static esp_err_t http_get(const char *url, char **response_out)
{
    http_buffer_t buffer = {0};
    bool is_https = strncmp(url, "https://", 8) == 0;
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = 10000,
        .crt_bundle_attach = is_https ? esp_crt_bundle_attach : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if(err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if(status != 200) {
            ESP_LOGW(TAG, "HTTP %d for %s", status, url);
            err = ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);
    if(err != ESP_OK) {
        http_buffer_free(&buffer);
        return err;
    }

    *response_out = buffer.data;
    return ESP_OK;
}

static void apply_timezone(const char *tz_string)
{
    setenv("TZ", tz_string, 1);
    tzset();
}

static void ensure_sntp_started(void)
{
    if(!s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        s_sntp_started = true;
    } else {
        esp_sntp_restart();
    }
}

static const char *weather_icon_for_code(int code)
{
    if(code == 0) return "SUN";
    if(code <= 3) return "CLOUD";
    if(code == 45 || code == 48) return "FOG";
    if((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "RAIN";
    if(code >= 71 && code <= 77) return "SNOW";
    if(code >= 95) return "STORM";
    return "SKY";
}

static void url_encode(char *dst, size_t dst_len, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;

    while(*src != '\0' && out + 4 < dst_len) {
        unsigned char ch = (unsigned char)*src++;
        if(isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            dst[out++] = (char)ch;
        } else {
            dst[out++] = '%';
            dst[out++] = hex[ch >> 4];
            dst[out++] = hex[ch & 0x0F];
        }
    }

    dst[out] = '\0';
}

static bool parse_location(const char *text, double *lat, double *lon)
{
    return sscanf(text, "%lf,%lf", lat, lon) == 2;
}

static cJSON *get_array_item(cJSON *array, int index)
{
    cJSON *item = cJSON_GetArrayItem(array, index);
    return cJSON_IsNull(item) ? NULL : item;
}

static const char *find_tag_value(const char *start, const char *end, const char *tag, char *out, size_t out_len)
{
    char open_tag[32];
    char close_tag[32];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *value_start = strstr(start, open_tag);
    if(value_start == NULL || value_start >= end) {
        out[0] = '\0';
        return NULL;
    }

    value_start += strlen(open_tag);
    const char *value_end = strstr(value_start, close_tag);
    if(value_end == NULL || value_end > end) {
        out[0] = '\0';
        return NULL;
    }

    size_t len = (size_t)(value_end - value_start);
    if(len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, value_start, len);
    out[len] = '\0';
    return out;
}

static void set_subway_status(app_data_snapshot_t *snapshot, const char *station_name, const char *summary, const char *detail)
{
    snapshot->subway_count = 1;
    snapshot->subway_valid = true;
    strlcpy(snapshot->station_name, station_name ? station_name : "", sizeof(snapshot->station_name));
    strlcpy(snapshot->subway_items[0].line, summary ? summary : "", sizeof(snapshot->subway_items[0].line));
    strlcpy(snapshot->subway_items[0].arrival, detail ? detail : "", sizeof(snapshot->subway_items[0].arrival));
    snapshot->subway_items[0].destination[0] = '\0';
    snapshot->subway_updated_at = time(NULL);
}

static const char *subway_service_name_for_id(const char *subway_id)
{
    if(subway_id == NULL || subway_id[0] == '\0') return NULL;
    if(strcmp(subway_id, "1001") == 0) return "1호선";
    if(strcmp(subway_id, "1002") == 0) return "2호선";
    if(strcmp(subway_id, "1003") == 0) return "3호선";
    if(strcmp(subway_id, "1004") == 0) return "4호선";
    if(strcmp(subway_id, "1005") == 0) return "5호선";
    if(strcmp(subway_id, "1006") == 0) return "6호선";
    if(strcmp(subway_id, "1007") == 0) return "7호선";
    if(strcmp(subway_id, "1008") == 0) return "8호선";
    if(strcmp(subway_id, "1009") == 0) return "9호선";
    if(strcmp(subway_id, "1032") == 0) return "GTX-A";
    if(strcmp(subway_id, "1061") == 0) return "중앙선";
    if(strcmp(subway_id, "1063") == 0) return "경의중앙선";
    if(strcmp(subway_id, "1065") == 0) return "공항철도";
    if(strcmp(subway_id, "1067") == 0) return "경춘선";
    if(strcmp(subway_id, "1075") == 0) return "수인분당선";
    if(strcmp(subway_id, "1077") == 0) return "신분당선";
    if(strcmp(subway_id, "1081") == 0) return "경강선";
    if(strcmp(subway_id, "1092") == 0) return "우이신설선";
    if(strcmp(subway_id, "1093") == 0) return "서해선";
    if(strcmp(subway_id, "1094") == 0) return "신림선";
    return NULL;
}

static void format_subway_service_label(char *buffer, size_t buffer_len,
                                        const char *subway_id, const char *subway_nm,
                                        const char *updn_line)
{
    const char *service_name = subway_nm;

    if(buffer_len == 0) {
        return;
    }

    buffer[0] = '\0';
    if(service_name == NULL || service_name[0] == '\0') {
        service_name = subway_service_name_for_id(subway_id);
    }

    if(service_name != NULL && service_name[0] != '\0') {
        if(updn_line != NULL && updn_line[0] != '\0') {
            snprintf(buffer, buffer_len, "%s %s", service_name, updn_line);
        } else {
            strlcpy(buffer, service_name, buffer_len);
        }
    } else if(updn_line != NULL && updn_line[0] != '\0') {
        strlcpy(buffer, updn_line, buffer_len);
    }
}

static esp_err_t fetch_weather_data(const app_preferences_t *prefs, app_data_snapshot_t *snapshot)
{
    double lat = 0.0;
    double lon = 0.0;
    if(!parse_location(prefs->weather_location, &lat, &lon)) {
        strlcpy(snapshot->weather_summary, "Weather region format: lat,lon", sizeof(snapshot->weather_summary));
        return ESP_ERR_INVALID_ARG;
    }

    char weather_url[384];
    char air_url[256];
    snprintf(weather_url, sizeof(weather_url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=Asia%%2FSeoul&forecast_days=2",
             lat, lon);
    snprintf(air_url, sizeof(air_url),
             "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f&current=us_aqi,pm10,pm2_5&timezone=Asia%%2FSeoul",
             lat, lon);

    char *weather_json = NULL;
    char *air_json = NULL;
    esp_err_t err = http_get(weather_url, &weather_json);
    if(err != ESP_OK) {
        return err;
    }

    err = http_get(air_url, &air_json);
    if(err != ESP_OK) {
        free(weather_json);
        return err;
    }

    cJSON *weather_root = cJSON_Parse(weather_json);
    cJSON *air_root = cJSON_Parse(air_json);
    free(weather_json);
    free(air_json);
    if(weather_root == NULL || air_root == NULL) {
        cJSON_Delete(weather_root);
        cJSON_Delete(air_root);
        return ESP_FAIL;
    }

    cJSON *current = cJSON_GetObjectItem(weather_root, "current");
    cJSON *daily = cJSON_GetObjectItem(weather_root, "daily");
    cJSON *daily_max = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *daily_min = cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON *daily_code = cJSON_GetObjectItem(daily, "weather_code");
    cJSON *aqi_current = cJSON_GetObjectItem(air_root, "current");

    time_t now = time(NULL);
    struct tm time_info;
    localtime_r(&now, &time_info);
    int day_index = time_info.tm_hour >= 20 ? 1 : 0;
    if(day_index >= cJSON_GetArraySize(daily_max)) {
        day_index = 0;
    }

    int weather_code = (int)cJSON_GetNumberValue(get_array_item(daily_code, day_index));
    int current_temp = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "temperature_2m"));
    int min_temp = (int)cJSON_GetNumberValue(get_array_item(daily_min, day_index));
    int max_temp = (int)cJSON_GetNumberValue(get_array_item(daily_max, day_index));
    int aqi = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(aqi_current, "us_aqi"));
    double pm10 = cJSON_GetNumberValue(cJSON_GetObjectItem(aqi_current, "pm10"));
    double pm25 = cJSON_GetNumberValue(cJSON_GetObjectItem(aqi_current, "pm2_5"));

    snapshot->weather_code = weather_code;
    snapshot->weather_current_temp = (int16_t)current_temp;
    snapshot->weather_min_temp = (int16_t)min_temp;
    snapshot->weather_max_temp = (int16_t)max_temp;
    strlcpy(snapshot->weather_target, day_index == 0 ? "TODAY" : "TOMORROW", sizeof(snapshot->weather_target));
    strlcpy(snapshot->weather_icon, weather_icon_for_code(weather_code), sizeof(snapshot->weather_icon));
    snprintf(snapshot->weather_summary, sizeof(snapshot->weather_summary), "%s %s", snapshot->weather_target,
             snapshot->weather_icon);
    snprintf(snapshot->weather_temp, sizeof(snapshot->weather_temp), "Now %dC  Low %dC  High %dC",
             current_temp, min_temp, max_temp);
    snapshot->air_quality_index = aqi;
    snapshot->air_pm10 = (float)pm10;
    snapshot->air_pm25 = (float)pm25;
    snprintf(snapshot->air_quality, sizeof(snapshot->air_quality), "AQI %d\nPM10 %.1f  PM2.5 %.1f",
             aqi, pm10, pm25);
    snapshot->weather_valid = true;
    snapshot->weather_updated_at = now;

    cJSON_Delete(weather_root);
    cJSON_Delete(air_root);
    return ESP_OK;
}

static esp_err_t fetch_bus_data(const app_preferences_t *prefs, app_data_snapshot_t *snapshot)
{
    char url[512];
    snprintf(url, sizeof(url),
             "http://ws.bus.go.kr/api/rest/stationinfo/getStationByUid?ServiceKey=%s&arsId=%s",
             BUS_API_KEY, prefs->bus_stop_id);

    char *xml = NULL;
    esp_err_t err = http_get(url, &xml);
    if(err != ESP_OK) {
        return err;
    }

    snapshot->bus_count = 0;
    snapshot->bus_valid = false;
    snapshot->stop_name[0] = '\0';

    const char *cursor = xml;
    while((cursor = strstr(cursor, "<itemList>")) != NULL && snapshot->bus_count < APP_MAX_BUS_ITEMS) {
        const char *item_end = strstr(cursor, "</itemList>");
        char route[20];
        char arr1[40];
        char arr2[40];
        char direction[24];
        char stop_name[32];

        if(item_end == NULL) {
            break;
        }

        find_tag_value(cursor, item_end, "rtNm", route, sizeof(route));
        find_tag_value(cursor, item_end, "arrmsg1", arr1, sizeof(arr1));
        find_tag_value(cursor, item_end, "arrmsg2", arr2, sizeof(arr2));
        find_tag_value(cursor, item_end, "adirection", direction, sizeof(direction));
        find_tag_value(cursor, item_end, "stNm", stop_name, sizeof(stop_name));

        if(snapshot->bus_count == 0 && stop_name[0] != '\0') {
            strlcpy(snapshot->stop_name, stop_name, sizeof(snapshot->stop_name));
        }

        if(route[0] != '\0') {
            app_bus_item_t *item = &snapshot->bus_items[snapshot->bus_count++];
            strlcpy(item->route, route, sizeof(item->route));
            strlcpy(item->arrival1, arr1, sizeof(item->arrival1));
            strlcpy(item->arrival2, arr2, sizeof(item->arrival2));
            strlcpy(item->direction, direction, sizeof(item->direction));
        }

        cursor = item_end + strlen("</itemList>");
    }

    free(xml);
    snapshot->bus_valid = snapshot->bus_count > 0;
    if(snapshot->bus_valid) {
        snapshot->bus_updated_at = time(NULL);
    }

    return snapshot->bus_valid ? ESP_OK : ESP_FAIL;
}

static esp_err_t fetch_subway_data(const app_preferences_t *prefs, app_data_snapshot_t *snapshot)
{
    char station_encoded[WIFI_SETTINGS_SUBWAY_MAX_LEN * 3 + 1];
    char url[384];
    url_encode(station_encoded, sizeof(station_encoded), prefs->subway_station);
    snprintf(url, sizeof(url),
             "http://swopenapi.seoul.go.kr/api/subway/%s/json/realtimeStationArrival/0/5/%s",
             SUBWAY_API_KEY, station_encoded);

    char *json = NULL;
    esp_err_t err = http_get(url, &json);
    if(err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if(root == NULL) {
        return ESP_FAIL;
    }

    snapshot->subway_count = 0;
    snapshot->subway_valid = false;
    snapshot->station_name[0] = '\0';

    cJSON *arrivals = cJSON_GetObjectItem(root, "realtimeArrivalList");
    if(cJSON_IsArray(arrivals) && cJSON_GetArraySize(arrivals) > 0) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, arrivals) {
            if(snapshot->subway_count >= APP_MAX_SUBWAY_ITEMS) {
                break;
            }

            const char *subway_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "subwayId"));
            const char *subway_nm = cJSON_GetStringValue(cJSON_GetObjectItem(item, "subwayNm"));
            const char *updn_line = cJSON_GetStringValue(cJSON_GetObjectItem(item, "updnLine"));
            const char *train_line = cJSON_GetStringValue(cJSON_GetObjectItem(item, "trainLineNm"));
            const char *arrival = cJSON_GetStringValue(cJSON_GetObjectItem(item, "arvlMsg2"));
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "statnNm"));
            char service_label[sizeof(snapshot->subway_items[0].line)];

            if(name != NULL && snapshot->subway_count == 0) {
                strlcpy(snapshot->station_name, name, sizeof(snapshot->station_name));
            }

            if(train_line != NULL && arrival != NULL) {
                format_subway_service_label(service_label, sizeof(service_label), subway_id, subway_nm, updn_line);

                app_subway_item_t *out = &snapshot->subway_items[snapshot->subway_count++];
                if(service_label[0] != '\0') {
                    strlcpy(out->line, service_label, sizeof(out->line));
                    strlcpy(out->destination, train_line, sizeof(out->destination));
                } else {
                    strlcpy(out->line, train_line, sizeof(out->line));
                    out->destination[0] = '\0';
                }
                strlcpy(out->arrival, arrival, sizeof(out->arrival));
            }
        }
    } else {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(root, "code"));
        if(code != NULL && strcmp(code, "INFO-200") == 0) {
            set_subway_status(snapshot, prefs->subway_station, "No live arrivals", "Check again later");
            cJSON_Delete(root);
            return ESP_OK;
        }
    }

    cJSON_Delete(root);
    snapshot->subway_valid = snapshot->subway_count > 0;
    if(snapshot->subway_valid) {
        snapshot->subway_updated_at = time(NULL);
    }

    return snapshot->subway_valid ? ESP_OK : ESP_FAIL;
}

static esp_err_t fetch_finance_data(const app_preferences_t *prefs, app_data_snapshot_t *snapshot)
{
    char ticker_encoded[WIFI_SETTINGS_TICKER_MAX_LEN * 3 + 1];
    char url[320];
    url_encode(ticker_encoded, sizeof(ticker_encoded), prefs->finance_ticker);
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1mo",
             ticker_encoded);

    char *json = NULL;
    esp_err_t err = http_get(url, &json);
    if(err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if(root == NULL) {
        return ESP_FAIL;
    }

    cJSON *chart = cJSON_GetObjectItem(root, "chart");
    cJSON *result = cJSON_GetObjectItem(chart, "result");
    cJSON *result_item = get_array_item(result, 0);
    cJSON *meta = cJSON_GetObjectItem(result_item, "meta");
    cJSON *indicators = cJSON_GetObjectItem(result_item, "indicators");
    cJSON *quote = cJSON_GetObjectItem(indicators, "quote");
    cJSON *quote_item = get_array_item(quote, 0);
    cJSON *close_array = cJSON_GetObjectItem(quote_item, "close");

    if(result_item == NULL || meta == NULL || close_array == NULL) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "shortName"));
    double price = cJSON_GetNumberValue(cJSON_GetObjectItem(meta, "regularMarketPrice"));
    int total = cJSON_GetArraySize(close_array);
    int start = total > APP_MAX_FINANCE_POINTS ? total - APP_MAX_FINANCE_POINTS : 0;
    int out_index = 0;

    for(int i = start; i < total && out_index < APP_MAX_FINANCE_POINTS; ++i) {
        cJSON *value_item = get_array_item(close_array, i);
        if(value_item == NULL) {
            continue;
        }

        snapshot->finance_series[out_index++] = (int32_t)(cJSON_GetNumberValue(value_item) + 0.5);
    }

    snapshot->finance_points = out_index;
    snapshot->finance_valid = out_index > 1;
    strlcpy(snapshot->finance_ticker, prefs->finance_ticker, sizeof(snapshot->finance_ticker));
    strlcpy(snapshot->finance_name, name ? name : prefs->finance_ticker, sizeof(snapshot->finance_name));
    snprintf(snapshot->finance_price, sizeof(snapshot->finance_price), "Now %.2f", price);
    if(snapshot->finance_valid) {
        snapshot->finance_updated_at = time(NULL);
    }

    cJSON_Delete(root);
    return snapshot->finance_valid ? ESP_OK : ESP_FAIL;
}

static void refresh_requested_data(const app_preferences_t *prefs, app_data_snapshot_t *snapshot, uint32_t flags)
{
    if(flags & APP_REFRESH_WEATHER) {
        if(fetch_weather_data(prefs, snapshot) != ESP_OK) {
            snapshot->weather_valid = false;
            snapshot->weather_code = 0;
            snapshot->weather_current_temp = 0;
            snapshot->weather_min_temp = 0;
            snapshot->weather_max_temp = 0;
            snapshot->air_quality_index = 0;
            snapshot->air_pm10 = 0.0f;
            snapshot->air_pm25 = 0.0f;
            snapshot->weather_temp[0] = '\0';
            snapshot->air_quality[0] = '\0';
            snapshot->weather_updated_at = 0;
            strlcpy(snapshot->weather_summary, "Weather fetch failed", sizeof(snapshot->weather_summary));
        }
    }

    if(flags & APP_REFRESH_BUS) {
        if(fetch_bus_data(prefs, snapshot) != ESP_OK) {
            strlcpy(snapshot->stop_name, "Bus fetch failed", sizeof(snapshot->stop_name));
        }
    }

    if(flags & APP_REFRESH_SUBWAY) {
        if(fetch_subway_data(prefs, snapshot) != ESP_OK) {
            strlcpy(snapshot->station_name, "Subway fetch failed", sizeof(snapshot->station_name));
        }
    }

    if(flags & APP_REFRESH_FINANCE) {
        if(fetch_finance_data(prefs, snapshot) != ESP_OK) {
            strlcpy(snapshot->finance_price, "Finance fetch failed", sizeof(snapshot->finance_price));
        }
    }
}

static void data_task(void *arg)
{
    (void)arg;

    app_preferences_t *prefs = malloc(sizeof(*prefs));
    app_data_snapshot_t *next_snapshot = malloc(sizeof(*next_snapshot));
    if(prefs == NULL || next_snapshot == NULL) {
        ESP_LOGE(TAG, "Failed to allocate data task buffers");
        free(prefs);
        free(next_snapshot);
        vTaskDelete(NULL);
        return;
    }

    while(true) {
        uint32_t flags = APP_REFRESH_NONE;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        *prefs = s_preferences;
        *next_snapshot = s_snapshot;
        if(s_wifi_ready && s_pending_refresh_flags != APP_REFRESH_NONE) {
            flags = s_pending_refresh_flags;
            s_pending_refresh_flags = APP_REFRESH_NONE;
        }
        xSemaphoreGive(s_mutex);

        if(flags != APP_REFRESH_NONE) {
            refresh_requested_data(prefs, next_snapshot, flags);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_snapshot = *next_snapshot;
            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t app_data_service_init(const app_preferences_t *prefs)
{
    if(s_mutex != NULL) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if(s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_preferences = *prefs;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    strlcpy(s_snapshot.finance_ticker, prefs->finance_ticker, sizeof(s_snapshot.finance_ticker));
    xSemaphoreGive(s_mutex);

    apply_timezone(prefs->timezone);

    BaseType_t created = xTaskCreate(data_task, "app_data_task", APP_DATA_TASK_STACK_SIZE, NULL, 5, &s_task);
    return created == pdPASS ? ESP_OK : ESP_FAIL;
}

void app_data_service_set_preferences(const app_preferences_t *prefs)
{
    if(prefs == NULL || s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_preferences = *prefs;
    strlcpy(s_snapshot.finance_ticker, prefs->finance_ticker, sizeof(s_snapshot.finance_ticker));
    xSemaphoreGive(s_mutex);

    apply_timezone(prefs->timezone);
}

void app_data_service_set_wifi_ready(bool ready)
{
    if(s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_wifi_ready = ready;
    if(ready) {
        s_pending_refresh_flags |= APP_REFRESH_WEATHER | APP_REFRESH_FINANCE;
    }
    xSemaphoreGive(s_mutex);

    if(ready) {
        ensure_sntp_started();
    }
}

void app_data_service_request_refresh(uint32_t flags)
{
    if(s_mutex == NULL || flags == APP_REFRESH_NONE) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending_refresh_flags |= flags;
    xSemaphoreGive(s_mutex);
}

void app_data_service_get_snapshot(app_data_snapshot_t *snapshot)
{
    if(snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if(s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *snapshot = s_snapshot;
    xSemaphoreGive(s_mutex);
}
