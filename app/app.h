/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "CLAIR FM"
#define APP_SPLASH_RGB RGB(232, 93, 74)
#define APP_BLOCK 256u
#define APP_TOUCH
#define APP_BTDEV
#define APP_OVERLOAD
#define APP_CANVAS_PX (240u * 144u)
#define APP_BLE_AT_BOOT (!clair_ble_off())
int clair_ble_off(void);

/* Persistent store objects: settings, platform panel calibration, own BLE bond, 4 user songs */
enum {
    OBJ_SETTINGS,
    OBJ_PANEL,
    OBJ_BLE,
    OBJ_SONG0,
    OBJ_SONG1,
    OBJ_SONG2,
    OBJ_SONG3,
    OBJ_NOBJ
};
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u, 0x98000u, 0x9A000u, 0x9C000u, 0x9E000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_overload(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_LOG
void app_log(void (*line)(const char *t));

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
