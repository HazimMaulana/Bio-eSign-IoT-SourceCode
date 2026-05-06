#ifndef EEZ_LVGL_UI_GUI_H
#define EEZ_LVGL_UI_GUI_H

#if defined(LV_LVGL_H_INCLUDE_SIMPLE) || defined(LV_CONF_INCLUDE_SIMPLE)
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif

#include "eez-flow.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t assets[3772];

void ui_init();
void ui_tick();

#ifdef __cplusplus
}
#endif

#endif // EEZ_LVGL_UI_GUI_H
