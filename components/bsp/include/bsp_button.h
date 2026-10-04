// components/bsp/include/bsp_button.h
// 三个按键共用一个 ADC 引脚,靠分压电阻区分。电压窗口见 bsp_pins.h。
#pragma once

#include "esp_err.h"
#include <stdint.h>

// 按键索引。数量用 bsp_pins.h 的 BSP_BTN_COUNT(硬件属性,归引脚表管),
// 这里不再定义尾项计数,避免出现 BSP_BTN_COUNT / BSP_BTN_COUNT_ 两个近似名字。
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,   // 按下瞬间(低延迟,适合游戏类即时响应)
    BSP_BTN_CLICK,       // 单击(按下并抬起)
    BSP_BTN_DOUBLE,      // 双击
    BSP_BTN_LONG,        // 长按
    BSP_BTN_RELEASE,     // 实际抬起（BUTTON_PRESS_UP，不等待单击判定结束）
} bsp_btn_ev_t;

// 按键事件回调。运行于 button 组件使用的共享 esp_timer 任务,只能入队或执行同等级
// 的有界操作；勿在其中阻塞、访问 LVGL 或做重活。
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 成功调用可重复，并更新回调与 user；失败会回滚本次已创建的按键和 ADC 资源。
// ADC 校准失败时返回错误而不是把无效电压解码为按键，修正故障后可重试。
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// 读当前 ADC 原始电压(mV)。松开时约 3300;按住某键时约为该键的分压值。
// ★ 换了分压/上拉阻值后,用它测出自己的三档电压,再改 bsp_pins.h 的 BSP_BTN_MV_TABLE。
// 读取失败返回 -1。
int bsp_button_read_mv(void);

// Manual deep-sleep handoff. Owner-task only; serialize against every button
// API/read and do not call from esp_timer/LVGL callbacks. This BSP owns all
// iot_button handles (the component's shared timer is stopped during handoff).
// Require a stable released ADC window, drain the polling timer, unregister and
// delete every button, then free ADC and configure the mapped GPIO as input with
// NO internal pulls (the board has its own 10 kOhm pull-up). Require stable HIGH
// again. timeout_ms is bounded to 2000 ms; >= 250 ms is required. On error,
// attempt full button/callback restoration and remain awake. Returns the GPIO
// number only on success. Does NOT configure or enter sleep.
esp_err_t bsp_button_prepare_deep_sleep(uint32_t timeout_ms, int *wake_gpio);

// Idempotent pre-terminal rollback, retaining the original callback/user.
// A restoration failure leaves resources owned and returns an error, never
// frees ADC beneath a surviving button. The caller must report/retry recovery.
esp_err_t bsp_button_resume_after_deep_sleep_cancel(void);

// Returns 0/1 only after a successful handoff, -1 otherwise. Recheck immediately
// before terminal shutdown; GPIO wake is level sensitive.
int bsp_button_deep_sleep_level(void);
