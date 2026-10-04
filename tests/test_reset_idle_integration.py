#!/usr/bin/env python3
"""Source contracts connecting the host-tested idle policy to app hardware.

These checks protect application wiring and terminal-admission fences. They do
not simulate FreeRTOS scheduling or establish physical timing on the board.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read_code(relative_path: str) -> str:
    source = (ROOT / relative_path).read_text(encoding="utf-8")
    # Comments must not satisfy a contract intended to check executable code.
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)


def block_after(source: str, anchor: str) -> str:
    start = source.find(anchor)
    if start < 0:
        raise AssertionError(f"code anchor not found: {anchor}")
    start = source.find("{", start + len(anchor))
    if start < 0:
        raise AssertionError(f"block not found after: {anchor}")
    depth = 1
    for end in range(start + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start + 1:end]
    raise AssertionError(f"unterminated block after: {anchor}")


class ResetIdleIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = read_code("main/main.c")
        cls.idle = read_code("main/reset_idle.c")
        cls.power = read_code("main/reset_power.c")
        cls.buttons = read_code("components/bsp/src/bsp_button.c")
        cls.display = read_code("components/bsp/src/bsp_display.c")
        cls.loop = cls.main[cls.main.index("for (;;) {"):]

    def assert_order(self, source: str, *tokens: str) -> None:
        positions = [source.index(token) for token in tokens]
        self.assertEqual(positions, sorted(positions), tokens)

    def test_auto_admission_never_selects_the_manual_sleep_page(self) -> None:
        automatic = block_after(self.loop, "if (!sleep_phase && reset_idle_screen_tick(")
        self.assertIn("automatic_sleep = true;", automatic)
        self.assertIn("sleep_phase = 1;", automatic)
        for forbidden in ("RESET_UI_SLEEP_WAIT", "controls.page", "controls.editing",
                          "reset_ui_", "bsp_display_backlight", "reset_wifi_suspend",
                          "reset_power_"):
            self.assertNotIn(forbidden, automatic)
        manual = block_after(self.loop, "if (hold_result == RESET_HOLD_SLEEP)")
        self.assertIn("automatic_sleep = false;", manual)
        self.assertIn("controls.page = RESET_UI_SLEEP_WAIT;", manual)
        self.assertEqual(self.loop.count("controls.page = RESET_UI_SLEEP_WAIT;"), 1)

    def test_idle_observation_includes_current_input_and_all_blockers(self) -> None:
        paused = re.search(r"bool idle_paused\s*=([^;]+);", self.loop)
        self.assertIsNotNone(paused)
        assert paused is not None
        for blocker in ("sleep_phase", "wifi.provisioning", "hold.active",
                        "controls.editing", "controls.page == RESET_UI_TIMEZONE",
                        "!atomic_load(&input_ready)", "!idle_keys_released", "queued_input"):
            self.assertIn(blocker, paused.group(1))
        self.assert_order(self.loop, "bool idle_keys_released",
                          "bool queued_input", "bool idle_paused",
                          "reset_idle_observe(&idle_policy, now_ms, user_activity, idle_paused)",
                          "if (!sleep_phase && reset_idle_screen_tick(")

    def test_screen_off_starts_grace_before_slow_service_work(self) -> None:
        off = block_after(self.loop, "if (idle_screen.screen_off && applied_brightness != 0)")
        self.assert_order(off, "bsp_display_backlight(0)", "applied_brightness = 0",
                          "idle_screen.grace_started_ms = milliseconds()")
        self.assert_order(self.loop, "if (!sleep_phase && reset_idle_screen_tick(",
                          "if (idle_screen.screen_off && applied_brightness != 0)",
                          "if (sleep_phase == 1)", "reset_feed_pause_and_wait(0)",
                          "reset_wifi_suspend()")
        preparation = block_after(self.loop, "if (sleep_phase == 1)")
        self.assertIn("reset_feed_pause_and_wait(0)", preparation)
        self.assertIn("reset_wifi_suspend()", preparation)
        self.assertEqual(self.loop.count("reset_feed_pause_and_wait(0)"), 1)
        self.assertEqual(self.loop.count("reset_wifi_suspend()"), 1)
        grace = block_after(self.idle, "if (!screen->grace_active || now_ms < screen->grace_started_ms)")
        self.assertIn("screen->screen_off = true;", grace)
        self.assertIn("screen->grace_started_ms = now_ms;", grace)
        self.assertIn("return false;", grace)
        self.assertIn("now_ms - screen->grace_started_ms >= RESET_IDLE_GRACE_MS", self.idle)
        self.assertNotRegex(self.idle, r"\b(?:bsp_|esp_|reset_wifi_|reset_feed_|reset_power_)")

    def test_dark_screen_wins_over_every_brightness_override(self) -> None:
        brightness = self.loop[self.loop.index("int backlight = reset_idle_brightness("):]
        self.assertRegex(brightness, r"if\s*\(!idle_screen\.screen_off\s*&&\s*"
                         r"\(wifi\.provisioning\s*\|\|\s*sleep_phase\s*\|\|\s*hold\.active\)\)"
                         r"\s*backlight = settings\.brightness;")
        self.assertIn("if (screen && screen->screen_off) return 0;", self.idle)
        self.assertNotIn("last_input", self.main)
        self.assertNotIn("120000", self.main)

    def test_first_wake_gesture_is_consumed_before_navigation(self) -> None:
        self.assertIn("bool wake_only = applied_brightness == 0;", self.loop)
        wake = block_after(self.loop, "if (wake_only)")
        for token in ("cancel_sleep(", "automatic_sleep = false;", "rtc_dark_restart = 0;",
                      "idle_screen = (reset_idle_screen_t){0};",
                      "direction[0].pressed = direction[1].pressed = false;",
                      "reset_hold_cancel(&hold)", "atomic_store(&input_ready, false)",
                      "input_gate.release_seen = false;", "xQueueReset(input_queue)"):
            self.assertIn(token, wake)
        self.assertNotIn("reset_controls_handle", wake)
        self.assertNotIn("reset_hold_press", wake)
        self.assertRegex(self.loop, r"if \(wake_only\)\s*\{[\s\S]+?\}\s*else if \(sleep_phase\)")
        # A callback enqueued while the gate is closed cannot become a tap when
        # this same iteration observes stable release and reopens the gate.
        gate = block_after(self.loop, "if (!input_was_ready)")
        self.assertIn("got_input = false;", gate)
        self.assertIn("got_input = input_was_ready && atomic_load(&input_ready)", self.loop)

    def test_late_input_is_fenced_after_the_button_producer_stops(self) -> None:
        terminal = block_after(self.loop, "if (sleep_phase == 3)")
        self.assert_order(terminal, "reset_power_prepare_deep_sleep(2000)",
                          "uxQueueMessagesWaiting(input_queue)",
                          "atomic_exchange(&input_overflow, false)",
                          "automatic_sleep && bsp_button_deep_sleep_had_activity()",
                          "atomic_store(&input_ready, false)",
                          "if (late_input)", "reset_feed_export_rtc(&rtc_feed)",
                          "reset_power_enter_deep_sleep()")
        self.assertRegex(terminal, r"if\s*\(prepare == ESP_OK\s*&&\s*!late_input\s*&&")
        late = block_after(terminal, "if (late_input)")
        self.assertIn("user_activity = true;", late)
        self.assertIn("reset_idle_observe(&idle_policy, milliseconds(), true, true)", late)
        self.assertIn("idle_screen = (reset_idle_screen_t){0};", late)
        self.assertIn("rtc_dark_restart = 0;", late)
        # enter() has reversible key-level checks after prepare. A detected key
        # there must restore the screen, rather than disappear in release gating.
        returned = terminal[terminal.index("reset_power_enter_deep_sleep()") :]
        wake = block_after(returned, "if (automatic_sleep && bsp_button_deep_sleep_had_activity())")
        self.assertIn("reset_idle_observe(&idle_policy, milliseconds(), true, true)", wake)
        self.assertIn("idle_screen = (reset_idle_screen_t){0};", wake)
        self.assertIn("rtc_dark_restart = 0;", wake)
        self.assert_order(returned, "bsp_button_deep_sleep_had_activity()", "cancel_sleep(")

    def test_release_wait_and_final_gpio_checks_keep_activity_latched(self) -> None:
        wait = block_after(self.buttons, "static esp_err_t wait_released(")
        pressed = block_after(wait, "if (!released)")
        self.assertIn("s_sleep_had_activity = true;", pressed)
        level = block_after(self.buttons, "int bsp_button_deep_sleep_level(")
        self.assertIn("if (level == 0) s_sleep_had_activity = true;", level)
        self.assertEqual(self.buttons.count("s_sleep_had_activity = false;"), 1)
        prepare = block_after(self.buttons, "esp_err_t bsp_button_prepare_deep_sleep(")
        self.assertIn("s_sleep_had_activity = false;", prepare)

    def test_reversible_auto_failure_keeps_page_and_dark_screen(self) -> None:
        cancel = block_after(self.main, "static void cancel_sleep(")
        self.assertIn("reset_idle_defer_after_cancel_or_failure(idle, milliseconds())", cancel)
        self.assertIn("reset_wifi_resume()", cancel)
        self.assertIn("reset_feed_resume(false)", cancel)
        manual = block_after(cancel, "if (!automatic)")
        self.assertIn("controls->page = RESET_UI_SETTINGS;", manual)
        self.assertIn("controls->editing = true;", manual)
        self.assertEqual(cancel.count("controls->page ="), 1)
        self.assertNotIn("bsp_display_backlight", cancel)
        failure = block_after(self.loop, "if (automatic_sleep && !sleep_phase)")
        self.assertIn("reset_idle_screen_failed(&idle_screen)", failure)
        self.assertIn("automatic_sleep = false;", failure)
        self.assertIn("rtc_dark_restart = 0;", failure)
        self.assertNotIn("screen_off = false", failure)
        failed_policy = block_after(self.idle, "void reset_idle_screen_failed(")
        self.assertIn("screen->grace_active = false;", failed_policy)
        self.assertNotIn("screen_off", failed_policy)

    def test_screen_off_suppresses_ui_redraw(self) -> None:
        render = block_after(self.loop, "if (!idle_screen.screen_off && bsp_lvgl_lock(100))")
        self.assertIn("reset_ui_update(&model)", render)
        self.assertEqual(self.loop.count("reset_ui_update(&model)"), 1)

    def test_auto_off_is_passed_to_policy_without_disabling_dimming(self) -> None:
        self.assertRegex(self.loop, r"reset_idle_screen_tick\(&idle_screen, &idle_policy,\s*"
                         r"settings\.sleep_minutes, now_ms, user_activity\)")
        off = block_after(self.idle, "if (!minutes || activity)")
        self.assertIn("*screen = (reset_idle_screen_t){0};", off)
        self.assertIn("return false;", off)
        brightness = block_after(self.idle, "int reset_idle_brightness(")
        self.assertIn("RESET_IDLE_DIM_MS", brightness)
        self.assertNotIn("minutes", brightness)

    def test_controlled_auto_restart_keeps_backlight_dark(self) -> None:
        self.assertRegex(self.main, r"RTC_NOINIT_ATTR\s+uint32_t\s+rtc_dark_restart;")
        self.assertRegex(self.main, r"bool dark_restart = esp_reset_reason\(\) == ESP_RST_SW\s*&&\s*"
                         r"rtc_dark_restart == RTC_DARK_MAGIC;")
        startup = self.main[self.main.index("bool dark_restart ="):self.main.index("for (;;) {")]
        self.assert_order(startup, "rtc_dark_restart = 0;",
                          "bsp_display_backlight(dark_restart ? 0 : settings.brightness)")
        self.assertIn("reset_idle_screen_t idle_screen = {.screen_off = dark_restart};", startup)
        self.assertIn("if (dark_restart) reset_idle_defer_after_cancel_or_failure", startup)
        self.assertEqual(self.main.count("rtc_dark_restart = RTC_DARK_MAGIC;"), 1)
        ledc = block_after(self.display, "static esp_err_t backlight_init(")
        self.assertRegex(ledc, r"\.duty\s*=\s*0\s*,")

    def test_existing_terminal_hardware_shutdown_order_is_preserved(self) -> None:
        terminal = block_after(self.power, "esp_err_t reset_power_enter_deep_sleep(")
        self.assert_order(terminal, "bsp_battery_sleep()", "bsp_audio_sleep()",
                          "bsp_audio_prepare_deep_sleep()", "bsp_i2c_prepare_deep_sleep()",
                          "bsp_display_prepare_deep_sleep()", "esp_deep_sleep_start()")


if __name__ == "__main__":
    unittest.main()
