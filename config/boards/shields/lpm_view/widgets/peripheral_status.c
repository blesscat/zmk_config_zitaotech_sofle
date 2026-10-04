/*
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>

#include "peripheral_status.h"

// ==================== 打字貓圖片 ====================
// 原圖 50x26 取自 englmaxi/zmk-dongle-display (v0.3) 的 bongo_cat_images.c
// 2x 放大後左右各裁 14px 成 72x52，再逆時針轉 90° 存成 52x72（直式螢幕用）
LV_IMG_DECLARE(bongo_idle);
LV_IMG_DECLARE(bongo_left);
LV_IMG_DECLARE(bongo_right);
LV_IMG_DECLARE(paw);

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

// ==================== 狀態結構體 ====================
struct peripheral_status_state {
    bool connected;
};

// ==================== 打字貓切換控制 ====================
// 每次右半板按鍵按下就左右掌交替拍一下，停手後 700ms 回到待機幀
#define BONGO_IDLE_TIMEOUT K_MSEC(700)

static bool bongo_left_paw;
static struct k_work_delayable bongo_idle_work;

static void bongo_set_frame(const lv_img_dsc_t *frame) {
    struct zmk_widget_status *widget;

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        // art 是第2個 child（0=canvas, 1=img）
        lv_img_set_src(lv_obj_get_child(widget->obj, 1), frame);
    }
}

static void bongo_idle_handler(struct k_work *work) { bongo_set_frame(&bongo_idle); }

static int bongo_keypress_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return 0;
    }

    bongo_left_paw = !bongo_left_paw;
    bongo_set_frame(bongo_left_paw ? &bongo_left : &bongo_right);
    k_work_reschedule(&bongo_idle_work, BONGO_IDLE_TIMEOUT);

    return 0;
}

ZMK_LISTENER(widget_bongo, bongo_keypress_listener);
ZMK_SUBSCRIPTION(widget_bongo, zmk_position_state_changed);

// ================= 顶部绘制 =================
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw battery
    draw_battery(canvas, state);

    // Draw connection icon
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

// ================= 电池状态 =================
static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state){
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

// ================= 连接状态 =================
static struct peripheral_status_state get_state(const zmk_event_t *eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

static void set_connection_status(struct zmk_widget_status *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void output_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

// ================= 初始化 =================
int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 144, 72);

    k_work_init_delayable(&bongo_idle_work, bongo_idle_handler);

    // --- 顶部 canvas ---
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    // --- 打字貓（2x 大圖貼底，左右置中，超出部分已裁切）---
    lv_obj_t *art = lv_img_create(widget->obj);
    lv_img_set_src(art, &bongo_idle);
    lv_obj_align(art, LV_ALIGN_TOP_LEFT, 92, 0);

    // --- 貓掌印裝飾（滿版寬，置中於狀態列與貓之間）---
    lv_obj_t *deco = lv_img_create(widget->obj);
    lv_img_set_src(deco, &paw);
    lv_obj_align(deco, LV_ALIGN_TOP_LEFT, 23, 0);

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
    widget_peripheral_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
