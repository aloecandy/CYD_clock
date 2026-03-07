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

LV_FONT_DECLARE(lv_font_korean_9);
LV_FONT_DECLARE(lv_font_korean_10);
LV_FONT_DECLARE(lv_font_korean_11);
LV_FONT_DECLARE(lv_font_korean_12);

#define QUOTA_TAB_TIMEOUT_US (30LL * 60LL * 1000000LL)
#define WEATHER_CANVAS_SIZE 120
#define AIR_QUALITY_CANVAS_SIZE 120
#define ICON_BASE_SIZE 120

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
static lv_obj_t *s_digital_date_label;
static lv_obj_t *s_digital_time_label;
static lv_obj_t *s_weather_canvas;
static lv_obj_t *s_weather_summary_label;
static lv_obj_t *s_weather_temp_label;
static lv_obj_t *s_air_quality_canvas;
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
static lv_obj_t *s_bus_labels[APP_MAX_BUS_ITEMS];
static lv_obj_t *s_subway_labels[APP_MAX_SUBWAY_ITEMS];
static lv_timer_t *s_ui_timer;
static lv_timer_t *s_clock_timer;
static lv_obj_t *s_clock_meter;
static lv_meter_indicator_t *s_hour_indic;
static lv_meter_indicator_t *s_minute_indic;
static lv_meter_indicator_t *s_second_indic;

static app_preferences_t s_preferences;
static bool s_last_wifi_ready;
static uint32_t s_last_tab = TAB_SETTINGS;
static int64_t s_quota_tab_entered_at;
static int s_weather_visual_code = -1;
static int s_air_quality_visual_value = -1;
static time_t s_last_clock_rendered_at = (time_t)-1;

static lv_coord_t scale_icon_coord(lv_coord_t value, lv_coord_t size)
{
    return (lv_coord_t)(((int32_t)value * size + (ICON_BASE_SIZE / 2)) / ICON_BASE_SIZE);
}

static lv_coord_t scale_icon_span(lv_coord_t value, lv_coord_t size)
{
    lv_coord_t scaled = scale_icon_coord(value, size);
    return scaled > 0 ? scaled : 1;
}

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
    bool compact = LV_HOR_RES <= 320;
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, compact ? 10 : 14, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(title_label,
                               compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
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

static void style_plain_container(lv_obj_t *obj)
{
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void configure_scrollable_tab(lv_obj_t *tab)
{
    bool compact = LV_HOR_RES <= 320;
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_right(tab, compact ? 8 : 18, 0);
    lv_obj_set_style_width(tab, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(tab, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(tab, lv_palette_lighten(LV_PALETTE_BLUE_GREY, 2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(tab, LV_OPA_70, LV_PART_SCROLLBAR);
}

static void apply_korean_font(lv_obj_t *obj, const lv_font_t *font)
{
    lv_obj_set_style_text_font(obj, font, 0);
}

static lv_obj_t *create_visual_panel(lv_obj_t *parent, const char *title)
{
    bool compact = LV_HOR_RES <= 320;
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, compact ? 140 : 216, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(panel, compact ? 8 : 12, 0);
    lv_obj_set_style_pad_gap(panel, compact ? 6 : 8, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(panel);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(title_label,
                               compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
    return panel;
}

static void format_digital_clock(struct tm *time_info, char *date_buf, size_t date_buf_len, char *time_buf, size_t time_buf_len)
{
    static const char *weekday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int hour = time_info->tm_hour % 12;
    if(hour == 0) {
        hour = 12;
    }

    snprintf(date_buf, date_buf_len, "%s %s %d",
             weekday_names[time_info->tm_wday],
             month_names[time_info->tm_mon],
             time_info->tm_mday);
    snprintf(time_buf, time_buf_len, "%d%s%02d", hour, (time_info->tm_sec % 2) == 0 ? ":" : " ", time_info->tm_min);
}

static const char *weather_description_for_code(int code)
{
    if(code == 0) return "Clear";
    if(code <= 3) return "Cloudy";
    if(code == 45 || code == 48) return "Fog";
    if((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
    if(code >= 71 && code <= 77) return "Snow";
    if(code >= 95) return "Storm";
    return "Sky";
}

static const char *air_quality_description_for_value(int aqi)
{
    if(aqi <= 50) return "Good";
    if(aqi <= 100) return "Moderate";
    return "Bad";
}

static void draw_icon_rect(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy, lv_coord_t size,
                           lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                           lv_coord_t radius, lv_color_t color, lv_opa_t opa,
                           lv_coord_t border_width, lv_color_t border_color)
{
    lv_draw_rect_dsc_t rect_dsc;
    lv_coord_t scaled_x = scale_icon_coord(x, size);
    lv_coord_t scaled_y = scale_icon_coord(y, size);
    lv_coord_t scaled_w = scale_icon_span(w, size);
    lv_coord_t scaled_h = scale_icon_span(h, size);
    lv_coord_t scaled_radius = radius == LV_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : scale_icon_coord(radius, size);
    lv_coord_t scaled_border = border_width > 0 ? scale_icon_span(border_width, size) : 0;
    lv_area_t coords = {
        .x1 = ox + scaled_x,
        .y1 = oy + scaled_y,
        .x2 = ox + scaled_x + scaled_w - 1,
        .y2 = oy + scaled_y + scaled_h - 1,
    };

    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.radius = scaled_radius;
    rect_dsc.bg_color = color;
    rect_dsc.bg_opa = opa;
    rect_dsc.border_width = scaled_border;
    rect_dsc.border_color = border_color;
    rect_dsc.border_opa = scaled_border > 0 ? LV_OPA_COVER : LV_OPA_TRANSP;
    rect_dsc.shadow_width = 0;
    lv_draw_rect(draw_ctx, &rect_dsc, &coords);
}

static void draw_icon_circle(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy, lv_coord_t size,
                             lv_coord_t x, lv_coord_t y, lv_coord_t diameter,
                             lv_color_t color, lv_opa_t opa)
{
    draw_icon_rect(draw_ctx, ox, oy, size, x, y, diameter, diameter, LV_RADIUS_CIRCLE, color, opa, 0, color);
}

static void draw_icon_line(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy, lv_coord_t size,
                           lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2,
                           lv_color_t color, lv_coord_t width)
{
    lv_draw_line_dsc_t line_dsc;
    lv_point_t p1 = {.x = ox + scale_icon_coord(x1, size), .y = oy + scale_icon_coord(y1, size)};
    lv_point_t p2 = {.x = ox + scale_icon_coord(x2, size), .y = oy + scale_icon_coord(y2, size)};

    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = scale_icon_span(width, size);
    line_dsc.round_end = 1;
    line_dsc.round_start = 1;
    lv_draw_line(draw_ctx, &line_dsc, &p1, &p2);
}

static void draw_icon_arc(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy, lv_coord_t size,
                          lv_coord_t center_x, lv_coord_t center_y, lv_coord_t radius,
                          int32_t start_angle, int32_t end_angle, lv_color_t color, lv_coord_t width)
{
    lv_draw_arc_dsc_t arc_dsc;
    lv_point_t center = {
        .x = ox + scale_icon_coord(center_x, size),
        .y = oy + scale_icon_coord(center_y, size),
    };

    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = color;
    arc_dsc.width = scale_icon_span(width, size);
    arc_dsc.rounded = 1;
    lv_draw_arc(draw_ctx, &arc_dsc, &center, scale_icon_coord(radius, size), start_angle, end_angle);
}

static void draw_weather_cloud(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy, lv_coord_t size,
                               lv_coord_t x, lv_coord_t y, lv_color_t color)
{
    draw_icon_circle(draw_ctx, ox, oy, size, x + 4, y + 18, 28, color, LV_OPA_COVER);
    draw_icon_circle(draw_ctx, ox, oy, size, x + 24, y + 6, 34, color, LV_OPA_COVER);
    draw_icon_circle(draw_ctx, ox, oy, size, x + 50, y + 18, 30, color, LV_OPA_COVER);
    draw_icon_rect(draw_ctx, ox, oy, size, x + 14, y + 28, 54, 20, 10, color, LV_OPA_COVER, 0, color);
}

static void draw_weather_sun(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy, lv_coord_t size)
{
    lv_color_t sun_color = lv_palette_main(LV_PALETTE_AMBER);
    draw_icon_circle(draw_ctx, ox, oy, size, 26, 18, 38, sun_color, LV_OPA_COVER);

    for(int i = 0; i < 8; ++i) {
        static const lv_point_t ray_offsets[8][2] = {
            {{45, 4}, {45, 14}}, {{59, 12}, {53, 20}}, {{67, 36}, {57, 36}}, {{59, 59}, {52, 52}},
            {{45, 68}, {45, 58}}, {{31, 59}, {37, 52}}, {{22, 36}, {32, 36}}, {{31, 12}, {37, 20}},
        };
        draw_icon_line(draw_ctx, ox, oy, size,
                       ray_offsets[i][0].x, ray_offsets[i][0].y,
                       ray_offsets[i][1].x, ray_offsets[i][1].y,
                       sun_color, 4);
    }
}

static void draw_weather_icon(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy,
                              lv_coord_t size, int weather_code)
{
    lv_color_t cloud_light = lv_palette_lighten(LV_PALETTE_BLUE_GREY, 4);
    lv_color_t cloud_dark = lv_palette_darken(LV_PALETTE_BLUE_GREY, 1);
    lv_color_t water = lv_palette_main(LV_PALETTE_BLUE);

    if(weather_code == 0) {
        draw_weather_sun(draw_ctx, ox, oy, size);
        return;
    }

    if(weather_code <= 3) {
        draw_weather_sun(draw_ctx, ox, oy, size);
        draw_weather_cloud(draw_ctx, ox, oy, size, 24, 38, cloud_light);
        return;
    }

    if(weather_code == 45 || weather_code == 48) {
        draw_weather_cloud(draw_ctx, ox, oy, size, 22, 28, cloud_light);
        for(int i = 0; i < 3; ++i) {
            draw_icon_line(draw_ctx, ox, oy, size, 26, 74 + (i * 10), 92, 74 + (i * 10),
                           lv_palette_lighten(LV_PALETTE_BLUE_GREY, 2), 4);
        }
        return;
    }

    if((weather_code >= 51 && weather_code <= 67) || (weather_code >= 80 && weather_code <= 82)) {
        draw_weather_cloud(draw_ctx, ox, oy, size, 22, 28, cloud_dark);
        draw_icon_line(draw_ctx, ox, oy, size, 38, 80, 32, 96, water, 4);
        draw_icon_line(draw_ctx, ox, oy, size, 58, 80, 52, 96, water, 4);
        draw_icon_line(draw_ctx, ox, oy, size, 78, 80, 72, 96, water, 4);
        return;
    }

    if(weather_code >= 71 && weather_code <= 77) {
        draw_weather_cloud(draw_ctx, ox, oy, size, 22, 28, cloud_light);
        for(int i = 0; i < 3; ++i) {
            lv_coord_t x = 36 + (i * 20);
            lv_coord_t y = 82;
            draw_icon_line(draw_ctx, ox, oy, size, x - 4, y, x + 4, y, lv_color_white(), 3);
            draw_icon_line(draw_ctx, ox, oy, size, x, y - 4, x, y + 4, lv_color_white(), 3);
            draw_icon_line(draw_ctx, ox, oy, size, x - 3, y - 3, x + 3, y + 3, lv_color_white(), 2);
            draw_icon_line(draw_ctx, ox, oy, size, x - 3, y + 3, x + 3, y - 3, lv_color_white(), 2);
        }
        return;
    }

    if(weather_code >= 95) {
        draw_weather_cloud(draw_ctx, ox, oy, size, 18, 22, cloud_dark);
        draw_icon_line(draw_ctx, ox, oy, size, 54, 66, 44, 86, lv_palette_main(LV_PALETTE_YELLOW), 5);
        draw_icon_line(draw_ctx, ox, oy, size, 44, 86, 58, 86, lv_palette_main(LV_PALETTE_YELLOW), 5);
        draw_icon_line(draw_ctx, ox, oy, size, 58, 86, 46, 104, lv_palette_main(LV_PALETTE_YELLOW), 5);
        draw_icon_line(draw_ctx, ox, oy, size, 34, 78, 28, 96, water, 3);
        draw_icon_line(draw_ctx, ox, oy, size, 78, 78, 72, 96, water, 3);
        return;
    }

    draw_weather_sun(draw_ctx, ox, oy, size);
    draw_weather_cloud(draw_ctx, ox, oy, size, 26, 40, cloud_light);
}

static void draw_air_quality_face(lv_draw_ctx_t *draw_ctx, lv_coord_t ox, lv_coord_t oy,
                                  lv_coord_t size, int aqi)
{
    lv_color_t face_color = lv_color_hex(0xFFD9B3);
    lv_color_t eye_color = lv_color_hex(0x202020);
    lv_color_t mask_color = lv_color_hex(0xD9EEF8);

    draw_icon_circle(draw_ctx, ox, oy, size, 14, 14, 92, face_color, LV_OPA_COVER);
    draw_icon_circle(draw_ctx, ox, oy, size, 37, 40, 10, eye_color, LV_OPA_COVER);
    draw_icon_circle(draw_ctx, ox, oy, size, 73, 40, 10, eye_color, LV_OPA_COVER);

    if(aqi <= 50) {
        draw_icon_arc(draw_ctx, ox, oy, size, 60, 58, 22, 20, 160, eye_color, 4);
        return;
    }

    if(aqi <= 100) {
        draw_icon_line(draw_ctx, ox, oy, size, 38, 78, 82, 78, eye_color, 4);
        return;
    }

    draw_icon_line(draw_ctx, ox, oy, size, 30, 28, 44, 22, eye_color, 4);
    draw_icon_line(draw_ctx, ox, oy, size, 76, 22, 90, 28, eye_color, 4);
    draw_icon_line(draw_ctx, ox, oy, size, 32, 46, 44, 42, eye_color, 4);
    draw_icon_line(draw_ctx, ox, oy, size, 76, 42, 88, 46, eye_color, 4);
    draw_icon_rect(draw_ctx, ox, oy, size, 28, 60, 64, 24, 8, mask_color, LV_OPA_COVER, 2,
                   lv_palette_main(LV_PALETTE_BLUE));
    draw_icon_line(draw_ctx, ox, oy, size, 18, 68, 28, 66, lv_palette_main(LV_PALETTE_BLUE), 3);
    draw_icon_line(draw_ctx, ox, oy, size, 92, 66, 102, 68, lv_palette_main(LV_PALETTE_BLUE), 3);
}

static void weather_icon_draw_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_DRAW_MAIN || s_weather_visual_code < 0) {
        return;
    }

    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t coords;
    lv_coord_t width;
    lv_coord_t height;
    lv_coord_t size;
    lv_coord_t ox;
    lv_coord_t oy;

    lv_obj_get_content_coords(obj, &coords);
    width = lv_area_get_width(&coords);
    height = lv_area_get_height(&coords);
    size = LV_MIN(width, height);
    ox = coords.x1 + (width - size) / 2;
    oy = coords.y1 + (height - size) / 2;
    draw_weather_icon(draw_ctx, ox, oy, size, s_weather_visual_code);
}

static void air_quality_icon_draw_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_DRAW_MAIN || s_air_quality_visual_value < 0) {
        return;
    }

    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t coords;
    lv_coord_t width;
    lv_coord_t height;
    lv_coord_t size;
    lv_coord_t ox;
    lv_coord_t oy;

    lv_obj_get_content_coords(obj, &coords);
    width = lv_area_get_width(&coords);
    height = lv_area_get_height(&coords);
    size = LV_MIN(width, height);
    ox = coords.x1 + (width - size) / 2;
    oy = coords.y1 + (height - size) / 2;
    draw_air_quality_face(draw_ctx, ox, oy, size, s_air_quality_visual_value);
}

static void draw_clock_ticks(lv_draw_ctx_t *draw_ctx, const lv_area_t *coords)
{
    lv_coord_t radius = LV_MIN(lv_area_get_width(coords), lv_area_get_height(coords)) / 2 - 4;
    lv_point_t center = {
        .x = coords->x1 + lv_area_get_width(coords) / 2,
        .y = coords->y1 + lv_area_get_height(coords) / 2,
    };
    lv_draw_line_dsc_t line_dsc;

    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.round_end = 1;
    line_dsc.round_start = 1;

    for(int i = 0; i < 60; ++i) {
        int32_t angle = 270 + (i * 6);
        lv_coord_t outer_radius = radius;
        lv_coord_t inner_radius;

        if((i % 15) == 0) {
            inner_radius = radius - 22;
            line_dsc.width = 5;
        } else if((i % 5) == 0) {
            inner_radius = radius - 16;
            line_dsc.width = 3;
        } else {
            inner_radius = radius - 10;
            line_dsc.width = 2;
        }

        lv_point_t p1 = {
            .x = center.x + (lv_trigo_cos(angle) * inner_radius) / LV_TRIGO_SIN_MAX,
            .y = center.y + (lv_trigo_sin(angle) * inner_radius) / LV_TRIGO_SIN_MAX,
        };
        lv_point_t p2 = {
            .x = center.x + (lv_trigo_cos(angle) * outer_radius) / LV_TRIGO_SIN_MAX,
            .y = center.y + (lv_trigo_sin(angle) * outer_radius) / LV_TRIGO_SIN_MAX,
        };

        lv_draw_line(draw_ctx, &line_dsc, &p1, &p2);
    }
}

static void clock_meter_draw_event_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_DRAW_POST || s_clock_meter == NULL) {
        return;
    }

    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t coords;
    lv_obj_get_content_coords(s_clock_meter, &coords);
    draw_clock_ticks(draw_ctx, &coords);
}

static void format_update_time(char *buffer, size_t buffer_len, time_t timestamp)
{
    if(timestamp == 0) {
        strlcpy(buffer, "@--:--:--", buffer_len);
        return;
    }

    struct tm time_info;
    localtime_r(&timestamp, &time_info);
    strftime(buffer, buffer_len, "@%H:%M:%S", &time_info);
}

static bool refresh_due_minutes(time_t now, time_t last_update, uint16_t minutes)
{
    return last_update == 0 || (now - last_update) >= (time_t)(minutes * 60);
}

static bool refresh_due_seconds(time_t now, time_t last_update, uint16_t seconds)
{
    return last_update == 0 || (now - last_update) >= (time_t)seconds;
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

static void populate_time_tab(lv_obj_t *tab)
{
    bool compact = LV_HOR_RES <= 320;
    lv_coord_t clock_card_size = compact ? 156 : 224;
    lv_coord_t meter_size = compact ? 152 : 208;
    lv_coord_t hour_r_mod = compact ? -42 : -56;
    lv_coord_t minute_r_mod = compact ? -20 : -24;

    lv_obj_set_style_pad_all(tab, compact ? 2 : 12, 0);
    lv_obj_set_style_pad_gap(tab, compact ? 2 : 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    configure_scrollable_tab(tab);

    lv_obj_t *clock_card = lv_obj_create(tab);
    lv_obj_set_size(clock_card, clock_card_size, clock_card_size);
    lv_obj_set_style_pad_all(clock_card, compact ? 2 : 8, 0);
    lv_obj_set_style_radius(clock_card, 16, 0);
    lv_obj_set_style_border_width(clock_card, compact ? 0 : 1, 0);
    lv_obj_set_style_shadow_width(clock_card, 0, 0);
    lv_obj_clear_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clock_card, clock_meter_draw_event_cb, LV_EVENT_DRAW_POST, NULL);

    s_clock_meter = lv_meter_create(clock_card);
    lv_obj_center(s_clock_meter);
    lv_obj_set_size(s_clock_meter, meter_size, meter_size);
    lv_obj_set_style_bg_opa(s_clock_meter, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_clock_meter, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_clock_meter, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_width(s_clock_meter, 12, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_clock_meter, 12, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_clock_meter, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_clock_meter, 0, LV_PART_INDICATOR);

    lv_meter_scale_t *hour_scale = lv_meter_add_scale(s_clock_meter);
    lv_meter_set_scale_ticks(s_clock_meter, hour_scale, 2, 0, 0, lv_color_black());
    lv_meter_set_scale_major_ticks(s_clock_meter, hour_scale, 0, 0, 0, lv_color_black(), 0);
    lv_meter_set_scale_range(s_clock_meter, hour_scale, 0, 12 * 60 * 60, 360, 270);
    s_hour_indic = lv_meter_add_needle_line(s_clock_meter, hour_scale, 6, lv_color_black(), hour_r_mod);

    lv_meter_scale_t *minute_scale = lv_meter_add_scale(s_clock_meter);
    lv_meter_set_scale_ticks(s_clock_meter, minute_scale, 2, 0, 0, lv_color_black());
    lv_meter_set_scale_major_ticks(s_clock_meter, minute_scale, 0, 0, 0, lv_color_black(), 0);
    lv_meter_set_scale_range(s_clock_meter, minute_scale, 0, 60 * 60, 360, 270);
    s_minute_indic = lv_meter_add_needle_line(s_clock_meter, minute_scale, 4,
                                              lv_palette_darken(LV_PALETTE_GREY, 3), minute_r_mod);

    lv_meter_scale_t *second_scale = lv_meter_add_scale(s_clock_meter);
    lv_meter_set_scale_ticks(s_clock_meter, second_scale, 2, 0, 0, lv_color_black());
    lv_meter_set_scale_major_ticks(s_clock_meter, second_scale, 0, 0, 0, lv_color_black(), 0);
    lv_meter_set_scale_range(s_clock_meter, second_scale, 0, 60, 360, 270);
    s_second_indic = lv_meter_add_needle_line(s_clock_meter, second_scale, 2, lv_palette_main(LV_PALETTE_RED), 0);

    lv_obj_t *digital_col = lv_obj_create(tab);
    style_plain_container(digital_col);
    lv_obj_set_height(digital_col, compact ? clock_card_size : 224);
    lv_obj_set_width(digital_col, compact ? 150 : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(digital_col, 1);
    lv_obj_set_style_pad_gap(digital_col, compact ? 4 : 10, 0);
    lv_obj_set_layout(digital_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(digital_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(digital_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_digital_date_label = lv_label_create(digital_col);
    lv_obj_set_width(s_digital_date_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_digital_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_digital_date_label,
                               compact ? &lv_font_montserrat_20 : &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_digital_date_label, lv_palette_darken(LV_PALETTE_BLUE_GREY, 1), 0);

    s_digital_time_label = lv_label_create(digital_col);
    lv_obj_set_width(s_digital_time_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_digital_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_digital_time_label,
                               compact ? &lv_font_montserrat_34 : &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_digital_time_label, lv_color_black(), 0);
}

static void populate_weather_tab(lv_obj_t *tab)
{
    bool compact = LV_HOR_RES <= 320;
    lv_coord_t icon_size = compact ? 76 : WEATHER_CANVAS_SIZE;

    lv_obj_set_style_pad_all(tab, compact ? 8 : 12, 0);
    lv_obj_set_style_pad_gap(tab, compact ? 8 : 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    configure_scrollable_tab(tab);

    lv_obj_t *weather_panel = create_visual_panel(tab, "Forecast");
    s_weather_canvas = lv_obj_create(weather_panel);
    style_plain_container(s_weather_canvas);
    lv_obj_set_size(s_weather_canvas, icon_size, icon_size);
    lv_obj_add_event_cb(s_weather_canvas, weather_icon_draw_event_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_weather_summary_label = lv_label_create(weather_panel);
    lv_obj_set_width(s_weather_summary_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_weather_summary_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_weather_summary_label,
                               compact ? &lv_font_montserrat_16 : &lv_font_montserrat_24, 0);
    s_weather_temp_label = lv_label_create(weather_panel);
    lv_obj_set_width(s_weather_temp_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_weather_temp_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_weather_temp_label,
                               compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_weather_temp_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *air_panel = create_visual_panel(tab, "Air Quality");
    s_air_quality_canvas = lv_obj_create(air_panel);
    style_plain_container(s_air_quality_canvas);
    lv_obj_set_size(s_air_quality_canvas, compact ? 72 : AIR_QUALITY_CANVAS_SIZE,
                    compact ? 72 : AIR_QUALITY_CANVAS_SIZE);
    lv_obj_add_event_cb(s_air_quality_canvas, air_quality_icon_draw_event_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_air_quality_label = lv_label_create(air_panel);
    lv_obj_set_width(s_air_quality_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_air_quality_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_air_quality_label,
                               compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_air_quality_label, LV_LABEL_LONG_WRAP);

    s_weather_updated_label = lv_label_create(tab);
    lv_obj_set_width(s_weather_updated_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_weather_updated_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_weather_updated_label, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    apply_korean_font(s_weather_updated_label, &lv_font_korean_9);
}

static void populate_bus_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    configure_scrollable_tab(tab);

    lv_obj_t *card = create_card(tab, "Bus Arrival");
    s_bus_title_label = lv_label_create(card);
    apply_korean_font(s_bus_title_label, &lv_font_korean_12);
    s_bus_updated_label = lv_label_create(card);
    apply_korean_font(s_bus_updated_label, &lv_font_korean_10);

    for(int i = 0; i < APP_MAX_BUS_ITEMS; ++i) {
        s_bus_labels[i] = lv_label_create(card);
        lv_obj_set_width(s_bus_labels[i], LV_PCT(100));
        lv_label_set_long_mode(s_bus_labels[i], LV_LABEL_LONG_WRAP);
        apply_korean_font(s_bus_labels[i], &lv_font_korean_11);
    }
}

static void populate_subway_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    configure_scrollable_tab(tab);

    lv_obj_t *card = create_card(tab, "Subway Arrival");
    s_subway_title_label = lv_label_create(card);
    apply_korean_font(s_subway_title_label, &lv_font_korean_12);
    s_subway_updated_label = lv_label_create(card);
    apply_korean_font(s_subway_updated_label, &lv_font_korean_10);

    for(int i = 0; i < APP_MAX_SUBWAY_ITEMS; ++i) {
        s_subway_labels[i] = lv_label_create(card);
        lv_obj_set_width(s_subway_labels[i], LV_PCT(100));
        lv_label_set_long_mode(s_subway_labels[i], LV_LABEL_LONG_WRAP);
        apply_korean_font(s_subway_labels[i], &lv_font_korean_11);
    }
}

static void populate_finance_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    configure_scrollable_tab(tab);

    lv_obj_t *card = create_card(tab, "Yahoo Finance");
    s_finance_name_label = lv_label_create(card);
    s_finance_price_label = lv_label_create(card);
    s_finance_updated_label = lv_label_create(card);
    apply_korean_font(s_finance_updated_label, &lv_font_korean_9);

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
    configure_scrollable_tab(tab);

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
    char date_text[32];
    char time_text[16];

    if(now == s_last_clock_rendered_at) {
        return;
    }
    s_last_clock_rendered_at = now;

    localtime_r(&now, &time_info);
    lv_meter_set_indicator_value(s_clock_meter, s_hour_indic,
                                 (time_info.tm_hour % 12) * 3600 + time_info.tm_min * 60 + time_info.tm_sec);
    lv_meter_set_indicator_value(s_clock_meter, s_minute_indic, time_info.tm_min * 60 + time_info.tm_sec);
    lv_meter_set_indicator_value(s_clock_meter, s_second_indic, time_info.tm_sec);

    format_digital_clock(&time_info, date_text, sizeof(date_text), time_text, sizeof(time_text));
    lv_label_set_text(s_digital_date_label, date_text);
    lv_label_set_text(s_digital_time_label, time_text);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_clock_ui();
}

static void schedule_due_refreshes(const app_data_snapshot_t *snapshot)
{
    time_t now = time(NULL);
    uint32_t flags = APP_REFRESH_NONE;
    uint32_t active_tab = lv_tabview_get_tab_act(s_tabview);

    if(refresh_due_minutes(now, snapshot->weather_updated_at, s_preferences.weather_refresh_minutes)) {
        flags |= APP_REFRESH_WEATHER;
    }
    if(refresh_due_minutes(now, snapshot->finance_updated_at, s_preferences.finance_refresh_minutes)) {
        flags |= APP_REFRESH_FINANCE;
    }
    if(active_tab == TAB_BUS && refresh_due_seconds(now, snapshot->bus_updated_at, s_preferences.bus_refresh_seconds)) {
        flags |= APP_REFRESH_BUS;
    }
    if(active_tab == TAB_SUBWAY &&
       refresh_due_seconds(now, snapshot->subway_updated_at, s_preferences.subway_refresh_seconds)) {
        flags |= APP_REFRESH_SUBWAY;
    }

    if(flags != APP_REFRESH_NONE) {
        app_data_service_request_refresh(flags);
    }
}

static void update_data_labels(void)
{
    app_data_snapshot_t snapshot;
    char weather_text[48];
    char air_quality_text[96];
    char updated_text[16];

    app_data_service_get_snapshot(&snapshot);

    if(snapshot.weather_valid) {
        snprintf(weather_text, sizeof(weather_text), "%s\n%s",
                 strcmp(snapshot.weather_target, "TOMORROW") == 0 ? "Tomorrow" : "Today",
                 weather_description_for_code(snapshot.weather_code));
        lv_label_set_text(s_weather_summary_label, weather_text);
        lv_label_set_text_fmt(s_weather_temp_label, "Now %dC\nLow %dC  High %dC",
                              snapshot.weather_current_temp,
                              snapshot.weather_min_temp,
                              snapshot.weather_max_temp);
        snprintf(air_quality_text, sizeof(air_quality_text), "%s\nAQI %d\nPM10 %.1f  PM2.5 %.1f",
                 air_quality_description_for_value(snapshot.air_quality_index),
                 snapshot.air_quality_index,
                 snapshot.air_pm10,
                 snapshot.air_pm25);
        lv_label_set_text(s_air_quality_label, air_quality_text);
        s_weather_visual_code = snapshot.weather_code;
        s_air_quality_visual_value = snapshot.air_quality_index;
        lv_obj_invalidate(s_weather_canvas);
        lv_obj_invalidate(s_air_quality_canvas);
    } else {
        lv_label_set_text(s_weather_summary_label,
                          snapshot.weather_summary[0] ? snapshot.weather_summary : "Weather pending...");
        lv_label_set_text(s_weather_temp_label, snapshot.weather_temp[0] ? snapshot.weather_temp : "-");
        lv_label_set_text(s_air_quality_label, snapshot.air_quality[0] ? snapshot.air_quality : "-");
        s_weather_visual_code = -1;
        s_air_quality_visual_value = -1;
        lv_obj_invalidate(s_weather_canvas);
        lv_obj_invalidate(s_air_quality_canvas);
    }

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
            if(snapshot.subway_items[i].destination[0] != '\0' && snapshot.subway_items[i].arrival[0] != '\0') {
                lv_label_set_text_fmt(s_subway_labels[i], "%s | %s\n%s",
                                      snapshot.subway_items[i].line,
                                      snapshot.subway_items[i].destination,
                                      snapshot.subway_items[i].arrival);
            } else if(snapshot.subway_items[i].destination[0] != '\0') {
                lv_label_set_text_fmt(s_subway_labels[i], "%s | %s",
                                      snapshot.subway_items[i].line,
                                      snapshot.subway_items[i].destination);
            } else if(snapshot.subway_items[i].arrival[0] != '\0') {
                lv_label_set_text_fmt(s_subway_labels[i], "%s / %s",
                                      snapshot.subway_items[i].line,
                                      snapshot.subway_items[i].arrival);
            } else {
                lv_label_set_text(s_subway_labels[i], snapshot.subway_items[i].line);
            }
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
    bool compact = LV_HOR_RES <= 320;

    ESP_ERROR_CHECK(wifi_settings_init());
    ESP_ERROR_CHECK(wifi_settings_load_preferences(&s_preferences));
    wifi_settings_sanitize_preferences(&s_preferences);

    disp_driver_set_orientation(s_preferences.orientation);
    disp_driver_set_backlight(s_preferences.brightness);
    ESP_ERROR_CHECK(app_data_service_init(&s_preferences));
    ESP_ERROR_CHECK(settings_web_server_init());
    settings_web_server_set_current_preferences(&s_preferences);

    s_tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, compact ? 36 : 44);
    s_tab_buttons = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_set_style_text_font(s_tab_buttons,
                               compact ? &lv_font_montserrat_10 : &lv_font_montserrat_14,
                               LV_PART_ITEMS);
    lv_obj_set_style_pad_top(s_tab_buttons, compact ? 2 : 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(s_tab_buttons, compact ? 2 : 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(s_tab_buttons, compact ? 3 : 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(s_tab_buttons, compact ? 3 : 6, LV_PART_ITEMS);
    lv_obj_clear_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_tabview, tabview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    populate_time_tab(lv_tabview_add_tab(s_tabview, "Time"));
    populate_weather_tab(lv_tabview_add_tab(s_tabview, "Weather"));
    populate_bus_tab(lv_tabview_add_tab(s_tabview, "Bus"));
    populate_subway_tab(lv_tabview_add_tab(s_tabview, "Subway"));
    populate_finance_tab(lv_tabview_add_tab(s_tabview, "Finance"));
    populate_settings_tab(lv_tabview_add_tab(s_tabview, "Settings"));

    sync_controls_from_preferences();
    apply_tab_lock(false);
    lv_tabview_set_act(s_tabview, TAB_SETTINGS, LV_ANIM_OFF);

    if(s_preferences.ssid[0] != '\0') {
        esp_err_t err = wifi_settings_connect(s_preferences.ssid, s_preferences.password);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "Auto-connect failed to start: %s", esp_err_to_name(err));
        }
    }

    s_last_clock_rendered_at = (time_t)-1;
    s_clock_timer = lv_timer_create(clock_timer_cb, 200, NULL);
    update_clock_ui();

    s_ui_timer = lv_timer_create(refresh_ui, 1000, NULL);
    refresh_ui(s_ui_timer);
}
