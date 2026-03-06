#include "smart_clock_app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_data_service.h"
#include "disp_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "settings_web_server.h"
#include "wifi_settings.h"

#define QUOTA_TAB_TIMEOUT_US (30LL * 60LL * 1000000LL)

enum {
    TAB_TIME = 0,
    TAB_WEATHER,
    TAB_BUS,
    TAB_SUBWAY,
    TAB_FINANCE,
    TAB_SETTINGS,
};

static const char *TAG = "smart_clock_app";

static lv_obj_t *s_tabview;
static lv_obj_t *s_tab_buttons;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_status_label;
static lv_obj_t *s_connect_button_label;
static lv_obj_t *s_orientation_dropdown;
static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_brightness_value_label;
static lv_obj_t *s_ssid_input;
static lv_obj_t *s_password_input;
static lv_obj_t *s_web_hint_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_weather_summary_label;
static lv_obj_t *s_weather_temp_label;
static lv_obj_t *s_air_quality_label;
static lv_obj_t *s_weather_updated_label;
static lv_obj_t *s_bus_title_label;
static lv_obj_t *s_bus_updated_label;
static lv_obj_t *s_subway_title_label;
static lv_obj_t *s_subway_updated_label;
static lv_obj_t *s_finance_name_label;
static lv_obj_t *s_finance_price_label;
static lv_obj_t *s_finance_updated_label;
static lv_obj_t *s_finance_chart;
static lv_chart_series_t *s_finance_series;
static lv_obj_t *s_perf_panel;
static lv_obj_t *s_perf_time_label;
static lv_obj_t *s_perf_stats_label;
static lv_obj_t *s_bus_labels[APP_MAX_BUS_ITEMS];
static lv_obj_t *s_subway_labels[APP_MAX_SUBWAY_ITEMS];
static lv_timer_t *s_ui_timer;
static lv_obj_t *s_clock_meter;
static lv_meter_indicator_t *s_hour_indic;
static lv_meter_indicator_t *s_minute_indic;
static lv_meter_indicator_t *s_second_indic;

static app_preferences_t s_preferences;
static bool s_last_wifi_ready;
static uint32_t s_last_tab = TAB_SETTINGS;
static int64_t s_quota_tab_entered_at;

static void textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);

    if(code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, target);
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_view_recursive(target, LV_ANIM_OFF);
    } else if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_keyboard_set_textarea(s_keyboard, NULL);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_indev_reset(NULL, target);
    }
}

static lv_obj_t *create_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_BLUE), 0);
    return card;
}

static lv_obj_t *create_labeled_textarea(lv_obj_t *parent, const char *label_text, const char *placeholder, bool password_mode)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, label_text);

    lv_obj_t *textarea = lv_textarea_create(parent);
    lv_obj_set_width(textarea, LV_PCT(100));
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, password_mode);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_obj_add_event_cb(textarea, textarea_event_cb, LV_EVENT_ALL, NULL);
    return textarea;
}

static void format_update_time(char *buffer, size_t buffer_len, time_t timestamp)
{
    if(timestamp == 0) {
        strlcpy(buffer, "@--:--", buffer_len);
        return;
    }

    struct tm time_info;
    localtime_r(&timestamp, &time_info);
    strftime(buffer, buffer_len, "@%H:%M", &time_info);
}

static bool refresh_due(time_t now, time_t last_update, uint16_t minutes)
{
    return last_update == 0 || (now - last_update) >= (time_t)(minutes * 60);
}

static void apply_tab_lock(bool connected)
{
    for(uint32_t i = TAB_TIME; i < TAB_SETTINGS; ++i) {
        if(connected) {
            lv_btnmatrix_clear_btn_ctrl(s_tab_buttons, i, LV_BTNMATRIX_CTRL_DISABLED);
        } else {
            lv_btnmatrix_set_btn_ctrl(s_tab_buttons, i, LV_BTNMATRIX_CTRL_DISABLED);
        }
    }

    if(!connected) {
        lv_tabview_set_act(s_tabview, TAB_SETTINGS, LV_ANIM_OFF);
    }
}

static void load_preferences_from_ui(void)
{
    strlcpy(s_preferences.ssid, lv_textarea_get_text(s_ssid_input), sizeof(s_preferences.ssid));
    strlcpy(s_preferences.password, lv_textarea_get_text(s_password_input), sizeof(s_preferences.password));
    s_preferences.orientation = lv_dropdown_get_selected(s_orientation_dropdown) == 0 ? 2 : 3;
    s_preferences.brightness = (uint8_t)lv_slider_get_value(s_brightness_slider);
    wifi_settings_sanitize_preferences(&s_preferences);
}

static void sync_controls_from_preferences(void)
{
    lv_textarea_set_text(s_ssid_input, s_preferences.ssid);
    lv_textarea_set_text(s_password_input, s_preferences.password);
    lv_dropdown_set_selected(s_orientation_dropdown, s_preferences.orientation == 3 ? 1 : 0);
    lv_slider_set_value(s_brightness_slider, s_preferences.brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_brightness_value_label, "%u%%", s_preferences.brightness);
}

static void apply_runtime_preferences(void)
{
    disp_driver_set_orientation(s_preferences.orientation);
    disp_driver_set_backlight(s_preferences.brightness);
    app_data_service_set_preferences(&s_preferences);
    settings_web_server_set_current_preferences(&s_preferences);
}

static void orientation_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    load_preferences_from_ui();
    if(disp_driver_set_orientation(s_preferences.orientation) == ESP_OK) {
        wifi_settings_save_preferences(&s_preferences);
        settings_web_server_set_current_preferences(&s_preferences);
    }
}

static void brightness_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    s_preferences.brightness = (uint8_t)lv_slider_get_value(s_brightness_slider);
    lv_label_set_text_fmt(s_brightness_value_label, "%u%%", s_preferences.brightness);
    disp_driver_set_backlight(s_preferences.brightness);
    wifi_settings_save_preferences(&s_preferences);
    settings_web_server_set_current_preferences(&s_preferences);
}

static void connect_button_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    load_preferences_from_ui();
    wifi_settings_save_preferences(&s_preferences);
    settings_web_server_set_current_preferences(&s_preferences);

    esp_err_t err = wifi_settings_connect(s_preferences.ssid, s_preferences.password);
    if(err != ESP_OK) {
        lv_label_set_text_fmt(s_status_label, "Wi-Fi connect failed: %s", esp_err_to_name(err));
        return;
    }

    lv_label_set_text(s_connect_button_label, "Connecting...");
}

static void request_tab_refresh(uint32_t tab_index)
{
    switch(tab_index) {
    case TAB_BUS:
        app_data_service_request_refresh(APP_REFRESH_BUS);
        s_quota_tab_entered_at = esp_timer_get_time();
        break;
    case TAB_SUBWAY:
        app_data_service_request_refresh(APP_REFRESH_SUBWAY);
        s_quota_tab_entered_at = esp_timer_get_time();
        break;
    default:
        s_quota_tab_entered_at = 0;
        break;
    }
}

static void tabview_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    uint32_t active_tab = lv_tabview_get_tab_act(s_tabview);
    if(active_tab != s_last_tab) {
        request_tab_refresh(active_tab);
        s_last_tab = active_tab;
    }
}

static void create_perf_panel(void)
{
    s_perf_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_perf_panel, 110, LV_SIZE_CONTENT);
    lv_obj_align(s_perf_panel, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_pad_all(s_perf_panel, 6, 0);
    lv_obj_set_style_radius(s_perf_panel, 8, 0);
    lv_obj_set_style_bg_opa(s_perf_panel, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_perf_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_perf_panel, 0, 0);
    lv_obj_clear_flag(s_perf_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_perf_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_perf_panel, LV_FLEX_FLOW_COLUMN);

    s_perf_time_label = lv_label_create(s_perf_panel);
    lv_obj_set_style_text_color(s_perf_time_label, lv_color_white(), 0);
    s_perf_stats_label = lv_label_create(s_perf_panel);
    lv_obj_set_style_text_color(s_perf_stats_label, lv_color_white(), 0);
}

static void populate_time_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = create_card(tab, "Clock");
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_clock_meter = lv_meter_create(card);
    lv_obj_set_size(s_clock_meter, 210, 210);
    lv_meter_scale_t *scale = lv_meter_add_scale(s_clock_meter);
    lv_meter_set_scale_ticks(s_clock_meter, scale, 60, 2, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(s_clock_meter, scale, 12, 4, 18, lv_color_black(), 10);
    lv_meter_set_scale_range(s_clock_meter, scale, 0, 60, 360, 270);
    s_hour_indic = lv_meter_add_needle_line(s_clock_meter, scale, 5, lv_palette_main(LV_PALETTE_BLUE), -20);
    s_minute_indic = lv_meter_add_needle_line(s_clock_meter, scale, 3, lv_color_black(), -10);
    s_second_indic = lv_meter_add_needle_line(s_clock_meter, scale, 2, lv_palette_main(LV_PALETTE_RED), 0);

    s_date_label = lv_label_create(card);
    lv_obj_set_width(s_date_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void populate_weather_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = create_card(tab, "Weather and AQI");
    s_weather_summary_label = lv_label_create(card);
    s_weather_temp_label = lv_label_create(card);
    s_air_quality_label = lv_label_create(card);
    s_weather_updated_label = lv_label_create(card);
}

static void populate_bus_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *card = create_card(tab, "Bus Arrival");
    s_bus_title_label = lv_label_create(card);
    s_bus_updated_label = lv_label_create(card);

    for(int i = 0; i < APP_MAX_BUS_ITEMS; ++i) {
        s_bus_labels[i] = lv_label_create(card);
        lv_obj_set_width(s_bus_labels[i], LV_PCT(100));
        lv_label_set_long_mode(s_bus_labels[i], LV_LABEL_LONG_WRAP);
    }
}

static void populate_subway_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *card = create_card(tab, "Subway Arrival");
    s_subway_title_label = lv_label_create(card);
    s_subway_updated_label = lv_label_create(card);

    for(int i = 0; i < APP_MAX_SUBWAY_ITEMS; ++i) {
        s_subway_labels[i] = lv_label_create(card);
        lv_obj_set_width(s_subway_labels[i], LV_PCT(100));
        lv_label_set_long_mode(s_subway_labels[i], LV_LABEL_LONG_WRAP);
    }
}

static void populate_finance_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = create_card(tab, "Yahoo Finance");
    s_finance_name_label = lv_label_create(card);
    s_finance_price_label = lv_label_create(card);
    s_finance_updated_label = lv_label_create(card);

    s_finance_chart = lv_chart_create(card);
    lv_obj_set_size(s_finance_chart, LV_PCT(100), 160);
    lv_chart_set_type(s_finance_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_finance_chart, 4, 4);
    lv_chart_set_point_count(s_finance_chart, APP_MAX_FINANCE_POINTS);
    s_finance_series = lv_chart_add_series(s_finance_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
}

static void populate_settings_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *wifi_card = create_card(tab, "Wi-Fi");
    s_ssid_input = create_labeled_textarea(wifi_card, "SSID", "Wi-Fi SSID", false);
    s_password_input = create_labeled_textarea(wifi_card, "Password", "Wi-Fi password", true);

    lv_obj_t *connect_btn = lv_btn_create(wifi_card);
    lv_obj_set_width(connect_btn, LV_PCT(100));
    lv_obj_add_event_cb(connect_btn, connect_button_event_cb, LV_EVENT_CLICKED, NULL);
    s_connect_button_label = lv_label_create(connect_btn);
    lv_label_set_text(s_connect_button_label, "Connect Wi-Fi");
    lv_obj_center(s_connect_button_label);

    s_status_label = lv_label_create(wifi_card);
    lv_obj_set_width(s_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);

    s_web_hint_label = lv_label_create(wifi_card);
    lv_obj_set_width(s_web_hint_label, LV_PCT(100));
    lv_label_set_long_mode(s_web_hint_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *display_card = create_card(tab, "Display");
    lv_obj_t *orientation_label = lv_label_create(display_card);
    lv_label_set_text(orientation_label, "Orientation");
    s_orientation_dropdown = lv_dropdown_create(display_card);
    lv_obj_set_width(s_orientation_dropdown, LV_PCT(100));
    lv_dropdown_set_options(s_orientation_dropdown, "Landscape\nLandscape Inverted");
    lv_obj_add_event_cb(s_orientation_dropdown, orientation_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *brightness_row = lv_obj_create(display_card);
    lv_obj_set_width(brightness_row, LV_PCT(100));
    lv_obj_set_height(brightness_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(brightness_row, 0, 0);
    lv_obj_set_style_border_width(brightness_row, 0, 0);
    lv_obj_set_style_bg_opa(brightness_row, LV_OPA_TRANSP, 0);
    lv_obj_set_layout(brightness_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(brightness_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brightness_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *brightness_label = lv_label_create(brightness_row);
    lv_label_set_text(brightness_label, "Brightness");
    s_brightness_value_label = lv_label_create(brightness_row);

    s_brightness_slider = lv_slider_create(display_card);
    lv_obj_set_width(s_brightness_slider, LV_PCT(100));
    lv_slider_set_range(s_brightness_slider, 10, 100);
    lv_obj_add_event_cb(s_brightness_slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_keyboard = lv_keyboard_create(lv_scr_act());
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void update_clock_ui(void)
{
    time_t now = time(NULL);
    struct tm time_info;
    char date_text[64];
    char current_time[16];

    localtime_r(&now, &time_info);
    lv_meter_set_indicator_value(s_clock_meter, s_hour_indic, (time_info.tm_hour % 12) * 5 + time_info.tm_min / 12);
    lv_meter_set_indicator_value(s_clock_meter, s_minute_indic, time_info.tm_min);
    lv_meter_set_indicator_value(s_clock_meter, s_second_indic, time_info.tm_sec);

    strftime(date_text, sizeof(date_text), "%Y-%m-%d %a", &time_info);
    strftime(current_time, sizeof(current_time), "[%H:%M]", &time_info);
    lv_label_set_text(s_date_label, date_text);
    lv_label_set_text(s_perf_time_label, current_time);
    lv_label_set_text_fmt(s_perf_stats_label, "FPS %lu\nCPU %u%%",
                          (unsigned long)lv_refr_get_fps_avg(),
                          100U - lv_timer_get_idle());
}

static void schedule_due_refreshes(const app_data_snapshot_t *snapshot)
{
    time_t now = time(NULL);
    uint32_t flags = APP_REFRESH_NONE;
    uint32_t active_tab = lv_tabview_get_tab_act(s_tabview);

    if(refresh_due(now, snapshot->weather_updated_at, s_preferences.weather_refresh_minutes)) {
        flags |= APP_REFRESH_WEATHER;
    }
    if(refresh_due(now, snapshot->finance_updated_at, s_preferences.finance_refresh_minutes)) {
        flags |= APP_REFRESH_FINANCE;
    }
    if(active_tab == TAB_BUS && refresh_due(now, snapshot->bus_updated_at, s_preferences.bus_refresh_minutes)) {
        flags |= APP_REFRESH_BUS;
    }
    if(active_tab == TAB_SUBWAY && refresh_due(now, snapshot->subway_updated_at, s_preferences.subway_refresh_minutes)) {
        flags |= APP_REFRESH_SUBWAY;
    }

    if(flags != APP_REFRESH_NONE) {
        app_data_service_request_refresh(flags);
    }
}

static void update_data_labels(void)
{
    app_data_snapshot_t snapshot;
    char updated_text[16];

    app_data_service_get_snapshot(&snapshot);

    lv_label_set_text(s_weather_summary_label,
                      snapshot.weather_summary[0] ? snapshot.weather_summary : "Weather pending...");
    lv_label_set_text(s_weather_temp_label, snapshot.weather_temp[0] ? snapshot.weather_temp : "-");
    lv_label_set_text(s_air_quality_label, snapshot.air_quality[0] ? snapshot.air_quality : "-");
    format_update_time(updated_text, sizeof(updated_text), snapshot.weather_updated_at);
    lv_label_set_text(s_weather_updated_label, updated_text);

    lv_label_set_text_fmt(s_bus_title_label, "Stop: %s",
                          snapshot.stop_name[0] ? snapshot.stop_name : s_preferences.bus_stop_id);
    format_update_time(updated_text, sizeof(updated_text), snapshot.bus_updated_at);
    lv_label_set_text(s_bus_updated_label, updated_text);
    for(int i = 0; i < APP_MAX_BUS_ITEMS; ++i) {
        if(i < snapshot.bus_count) {
            lv_label_set_text_fmt(s_bus_labels[i], "%s -> %s / %s",
                                  snapshot.bus_items[i].route,
                                  snapshot.bus_items[i].arrival1,
                                  snapshot.bus_items[i].arrival2);
        } else {
            lv_label_set_text(s_bus_labels[i], "-");
        }
    }

    lv_label_set_text_fmt(s_subway_title_label, "Station: %s",
                          snapshot.station_name[0] ? snapshot.station_name : s_preferences.subway_station);
    format_update_time(updated_text, sizeof(updated_text), snapshot.subway_updated_at);
    lv_label_set_text(s_subway_updated_label, updated_text);
    for(int i = 0; i < APP_MAX_SUBWAY_ITEMS; ++i) {
        if(i < snapshot.subway_count) {
            lv_label_set_text_fmt(s_subway_labels[i], "%s / %s",
                                  snapshot.subway_items[i].line,
                                  snapshot.subway_items[i].arrival);
        } else {
            lv_label_set_text(s_subway_labels[i], "-");
        }
    }

    lv_label_set_text(s_finance_name_label,
                      snapshot.finance_name[0] ? snapshot.finance_name : s_preferences.finance_ticker);
    lv_label_set_text(s_finance_price_label,
                      snapshot.finance_price[0] ? snapshot.finance_price : "Finance pending...");
    format_update_time(updated_text, sizeof(updated_text), snapshot.finance_updated_at);
    lv_label_set_text(s_finance_updated_label, updated_text);

    if(snapshot.finance_points > 1) {
        int32_t min_value = snapshot.finance_series[0];
        int32_t max_value = snapshot.finance_series[0];

        lv_chart_set_point_count(s_finance_chart, snapshot.finance_points);
        lv_chart_set_all_value(s_finance_chart, s_finance_series, snapshot.finance_series[0]);
        for(int i = 0; i < snapshot.finance_points; ++i) {
            if(snapshot.finance_series[i] < min_value) min_value = snapshot.finance_series[i];
            if(snapshot.finance_series[i] > max_value) max_value = snapshot.finance_series[i];
            lv_chart_set_next_value(s_finance_chart, s_finance_series, snapshot.finance_series[i]);
        }
        if(min_value == max_value) {
            min_value -= 1;
            max_value += 1;
        }
        lv_chart_set_range(s_finance_chart, LV_CHART_AXIS_PRIMARY_Y, min_value, max_value);
        lv_chart_refresh(s_finance_chart);
    }

    if(wifi_settings_is_connected()) {
        schedule_due_refreshes(&snapshot);
    }
}

static void update_web_hint(void)
{
    char ip[16];
    wifi_settings_get_ip_address(ip, sizeof(ip));
    if(ip[0] != '\0') {
        lv_label_set_text_fmt(s_web_hint_label, "More settings: http://%s/", ip);
    } else {
        lv_label_set_text(s_web_hint_label, "More settings available from browser after Wi-Fi connects.");
    }
}

static void apply_web_updates_if_needed(void)
{
    app_preferences_t new_prefs;
    bool wifi_changed = false;

    if(!settings_web_server_consume_pending_update(&new_prefs, &wifi_changed)) {
        return;
    }

    s_preferences = new_prefs;
    wifi_settings_sanitize_preferences(&s_preferences);
    sync_controls_from_preferences();
    apply_runtime_preferences();

    if(wifi_changed && s_preferences.ssid[0] != '\0') {
        esp_err_t err = wifi_settings_connect(s_preferences.ssid, s_preferences.password);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "Web Wi-Fi update failed: %s", esp_err_to_name(err));
        }
    } else {
        app_data_service_request_refresh(APP_REFRESH_WEATHER | APP_REFRESH_FINANCE);
    }
}

static void refresh_ui(lv_timer_t *timer)
{
    char status[96];
    wifi_connection_state_t state;
    bool wifi_ready;

    (void)timer;

    update_clock_ui();
    apply_web_updates_if_needed();
    update_data_labels();

    state = wifi_settings_get_state();
    wifi_settings_get_status_text(status, sizeof(status));
    lv_label_set_text(s_status_label, status);
    update_web_hint();

    wifi_ready = wifi_settings_is_connected();
    apply_tab_lock(wifi_ready);
    lv_label_set_text(s_connect_button_label,
                      state == WIFI_CONNECTION_CONNECTING ? "Connecting..." :
                      (wifi_ready ? "Reconnect Wi-Fi" : "Connect Wi-Fi"));

    if(wifi_ready != s_last_wifi_ready) {
        app_data_service_set_wifi_ready(wifi_ready);
        settings_web_server_set_enabled(wifi_ready);
        if(wifi_ready) {
            app_data_service_request_refresh(APP_REFRESH_WEATHER | APP_REFRESH_FINANCE);
        }
        s_last_wifi_ready = wifi_ready;
    }

    if((s_last_tab == TAB_BUS || s_last_tab == TAB_SUBWAY) && s_quota_tab_entered_at != 0 &&
       (esp_timer_get_time() - s_quota_tab_entered_at) >= QUOTA_TAB_TIMEOUT_US) {
        lv_tabview_set_act(s_tabview, TAB_TIME, LV_ANIM_ON);
    }
}

void smart_clock_app_start(void)
{
    ESP_ERROR_CHECK(wifi_settings_init());
    ESP_ERROR_CHECK(wifi_settings_load_preferences(&s_preferences));
    wifi_settings_sanitize_preferences(&s_preferences);

    disp_driver_set_orientation(s_preferences.orientation);
    disp_driver_set_backlight(s_preferences.brightness);
    ESP_ERROR_CHECK(app_data_service_init(&s_preferences));
    ESP_ERROR_CHECK(settings_web_server_init());
    settings_web_server_set_current_preferences(&s_preferences);

    s_tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 44);
    s_tab_buttons = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_clear_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_tabview, tabview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    populate_time_tab(lv_tabview_add_tab(s_tabview, "Time"));
    populate_weather_tab(lv_tabview_add_tab(s_tabview, "Weather"));
    populate_bus_tab(lv_tabview_add_tab(s_tabview, "Bus"));
    populate_subway_tab(lv_tabview_add_tab(s_tabview, "Subway"));
    populate_finance_tab(lv_tabview_add_tab(s_tabview, "Finance"));
    populate_settings_tab(lv_tabview_add_tab(s_tabview, "Settings"));
    create_perf_panel();

    sync_controls_from_preferences();
    apply_tab_lock(false);
    lv_tabview_set_act(s_tabview, TAB_SETTINGS, LV_ANIM_OFF);

    if(s_preferences.ssid[0] != '\0') {
        esp_err_t err = wifi_settings_connect(s_preferences.ssid, s_preferences.password);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "Auto-connect failed to start: %s", esp_err_to_name(err));
        }
    }

    s_ui_timer = lv_timer_create(refresh_ui, 1000, NULL);
    refresh_ui(s_ui_timer);
}
