#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#if defined(LV_LVGL_H_INCLUDE_SIMPLE) || defined(LV_CONF_INCLUDE_SIMPLE)
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_LOADING_SCREEN = 1,
    SCREEN_ID_BOOTING_SCREEN = 2,
    SCREEN_ID_SCAN = 3,
    SCREEN_ID_RESULT = 4,
    SCREEN_ID_REGISTER = 5,
    _SCREEN_ID_LAST = 5
};

typedef struct _objects_t {
    lv_obj_t *loading_screen;
    lv_obj_t *booting_screen;
    lv_obj_t *scan;
    lv_obj_t *result;
    lv_obj_t *register_screen;
    lv_obj_t *obj0;
    lv_obj_t *obj1;
    lv_obj_t *obj2;
    lv_obj_t *obj3;
    lv_obj_t *obj4;
    lv_obj_t *obj5;
    lv_obj_t *obj6;
    lv_obj_t *obj7;
    lv_obj_t *logo_unram;
    lv_obj_t *unram_label;
    lv_obj_t *boot_2;
    lv_obj_t *obj8;
    lv_obj_t *obj9;
    lv_obj_t *obj10;
    lv_obj_t *obj11;
    lv_obj_t *obj12;
    lv_obj_t *obj13;
    lv_obj_t *obj14;
    lv_obj_t *obj15;
    lv_obj_t *obj16;
    lv_obj_t *result_name;
    lv_obj_t *result_nim;
    lv_obj_t *register_name;
    lv_obj_t *register_nim;
    lv_obj_t *register_status;
} objects_t;

extern objects_t objects;

void create_screen_loading_screen();
void tick_screen_loading_screen();

void create_screen_booting_screen();
void tick_screen_booting_screen();

void create_screen_scan();
void tick_screen_scan();

void create_screen_result();
void tick_screen_result();

void create_screen_register();
void tick_screen_register();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/
