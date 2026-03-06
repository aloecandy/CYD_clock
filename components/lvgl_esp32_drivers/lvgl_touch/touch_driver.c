/**
 * @file touch_driver.c
 */

#include "touch_driver.h"
#include "tp_spi.h"
#include "disp_driver.h"


void touch_driver_init(void)
{
#if defined (CONFIG_LV_TOUCH_CONTROLLER_XPT2046)
    xpt2046_init();
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_FT6X06)
    ft6x06_init(FT6236_I2C_SLAVE_ADDR);
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_STMPE610)
    stmpe610_init();
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_ADCRAW)
    adcraw_init();
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_FT81X)
    /* nothing to do */
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_RA8875)
    ra8875_touch_init();
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_GT911)
    gt911_init(GT911_I2C_SLAVE_ADDR);
#endif
}

#if LVGL_VERSION_MAJOR >= 8
void touch_driver_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
#else
bool touch_driver_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
#endif
{
    bool res = false;

#if defined (CONFIG_LV_TOUCH_CONTROLLER_XPT2046)
    res = xpt2046_read(drv, data);
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_FT6X06)
    res = ft6x36_read(drv, data);
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_STMPE610)
    res = stmpe610_read(drv, data);
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_ADCRAW)
    res = adcraw_read(drv, data);
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_FT81X)
    res = FT81x_read(drv, data);
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_RA8875)
    res = ra8875_touch_read(drv, data);
#elif defined (CONFIG_LV_TOUCH_CONTROLLER_GT911)
    res = gt911_read(drv, data);
#endif

    if(data->state == LV_INDEV_STATE_PR || data->state == LV_INDEV_STATE_REL) {
        uint8_t orientation = disp_driver_get_orientation();
        if(orientation == 3) {
            data->point.x = LV_HOR_RES - data->point.x - 1;
            data->point.y = LV_VER_RES - data->point.y - 1;
        }
    }

#if LVGL_VERSION_MAJOR >= 8
    data->continue_reading = res;
#else
    return res;
#endif
}
