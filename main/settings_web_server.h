#ifndef SETTINGS_WEB_SERVER_H
#define SETTINGS_WEB_SERVER_H

#include <stdbool.h>
#include "esp_err.h"
#include "wifi_settings.h"

esp_err_t settings_web_server_init(void);
void settings_web_server_set_enabled(bool enabled);
void settings_web_server_set_current_preferences(const app_preferences_t *prefs);
bool settings_web_server_consume_pending_update(app_preferences_t *prefs, bool *wifi_changed);

#endif
