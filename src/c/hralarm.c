#include <pebble.h>

// --- config ---
#define DEFAULT_TARGET_BPM 120
#define DEFAULT_SUSTAIN_SECS 30
#define DEFAULT_DELAY_MINUTES 1
#define HR_SAMPLE_PERIOD_SEC 1
#define TICK_MS 1000
#define WAKEUP_REASON_ALARM 0

static Window *s_main_window;
static TextLayer *s_time_layer;
static TextLayer *s_config_layer;
static TextLayer *s_hint_layer;

static Window *s_alarm_window;
static TextLayer *s_alarm_bpm_layer;
static TextLayer *s_alarm_status_layer;
static TextLayer *s_alarm_progress_layer;

static int s_target_bpm = DEFAULT_TARGET_BPM;
static int s_sustain_secs = DEFAULT_SUSTAIN_SECS;
static int s_delay_minutes = DEFAULT_DELAY_MINUTES;

static int s_sustained_seconds = 0;
static bool s_unlocked = false;
static AppTimer *s_alarm_timer;
static WakeupId s_wakeup_id;

// ---------- persistence ----------
enum { PKEY_TARGET_BPM = 100, PKEY_SUSTAIN_SECS = 101, PKEY_DELAY_MIN = 102, PKEY_WAKEUP_ID = 103 };

static void load_settings(void) {
  if (persist_exists(PKEY_TARGET_BPM)) s_target_bpm = persist_read_int(PKEY_TARGET_BPM);
  if (persist_exists(PKEY_SUSTAIN_SECS)) s_sustain_secs = persist_read_int(PKEY_SUSTAIN_SECS);
  if (persist_exists(PKEY_DELAY_MIN)) s_delay_minutes = persist_read_int(PKEY_DELAY_MIN);
}

static void save_settings(void) {
  persist_write_int(PKEY_TARGET_BPM, s_target_bpm);
  persist_write_int(PKEY_SUSTAIN_SECS, s_sustain_secs);
  persist_write_int(PKEY_DELAY_MIN, s_delay_minutes);
}

// ---------- alarm window ----------
static void start_hr_alarm_vibe(void) {
  static const uint32_t segments[] = {200, 200, 200, 200, 200, 600};
  VibePattern pattern = {
    .durations = segments,
    .num_segments = ARRAY_LENGTH(segments),
  };
  vibes_enqueue_custom_pattern(pattern);
}

static void update_alarm_ui(int bpm) {
  static char bpm_buf[32];
  static char status_buf[48];
  static char progress_buf[24];

  if (bpm <= 0) {
    snprintf(bpm_buf, sizeof(bpm_buf), "-- BPM");
  } else {
    snprintf(bpm_buf, sizeof(bpm_buf), "%d BPM", bpm);
  }
  text_layer_set_text(s_alarm_bpm_layer, bpm_buf);

  if (s_unlocked) {
    snprintf(status_buf, sizeof(status_buf), "Unlocked!\nPress SELECT to stop");
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d s", s_sustain_secs, s_sustain_secs);
  } else if (bpm <= 0) {
    snprintf(status_buf, sizeof(status_buf), "Measuring...");
    snprintf(progress_buf, sizeof(progress_buf), "target %d bpm", s_target_bpm);
  } else if (bpm >= s_target_bpm) {
    snprintf(status_buf, sizeof(status_buf), "Keep it up!");
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d s", s_sustained_seconds, s_sustain_secs);
  } else {
    snprintf(status_buf, sizeof(status_buf), "Get moving!\nneed %d bpm", s_target_bpm);
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d s", s_sustained_seconds, s_sustain_secs);
  }
  text_layer_set_text(s_alarm_status_layer, status_buf);
  text_layer_set_text(s_alarm_progress_layer, progress_buf);
}

static void alarm_tick(void *data) {
  if (!s_unlocked) {
    start_hr_alarm_vibe();
  }

  HealthValue raw = health_service_peek_current_value(HealthMetricHeartRateRawBPM);
  int bpm = (int)raw;

  if (!s_unlocked) {
    if (bpm >= s_target_bpm) {
      s_sustained_seconds++;
      if (s_sustained_seconds >= s_sustain_secs) {
        s_unlocked = true;
      }
    } else {
      s_sustained_seconds = 0;
    }
  }

  update_alarm_ui(bpm);

  s_alarm_timer = app_timer_register(TICK_MS, alarm_tick, NULL);
}

static void alarm_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_unlocked) {
    // locked: ignore, can't dismiss until sustained HR target hit
    return;
  }
  if (s_alarm_timer) {
    app_timer_cancel(s_alarm_timer);
    s_alarm_timer = NULL;
  }
  health_service_set_heart_rate_sample_period(0);
  window_stack_pop(true);
}

static void alarm_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, alarm_select_click_handler);
}

static void alarm_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_alarm_bpm_layer = text_layer_create(GRect(0, 20, bounds.size.w, 50));
  text_layer_set_font(s_alarm_bpm_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_alarm_bpm_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_alarm_bpm_layer));

  s_alarm_status_layer = text_layer_create(GRect(0, 75, bounds.size.w, 60));
  text_layer_set_font(s_alarm_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_alarm_status_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_alarm_status_layer));

  s_alarm_progress_layer = text_layer_create(GRect(0, 140, bounds.size.w, 30));
  text_layer_set_font(s_alarm_progress_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_alarm_progress_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_alarm_progress_layer));

  health_service_set_heart_rate_sample_period(HR_SAMPLE_PERIOD_SEC);
  s_sustained_seconds = 0;
  s_unlocked = false;
  update_alarm_ui(0);

  s_alarm_timer = app_timer_register(TICK_MS, alarm_tick, NULL);
}

static void alarm_window_unload(Window *window) {
  text_layer_destroy(s_alarm_bpm_layer);
  text_layer_destroy(s_alarm_status_layer);
  text_layer_destroy(s_alarm_progress_layer);
}

static void enter_alarm_mode(void) {
  s_alarm_window = window_create();
  window_set_click_config_provider(s_alarm_window, alarm_click_config_provider);
  window_set_window_handlers(s_alarm_window, (WindowHandlers) {
    .load = alarm_window_load,
    .unload = alarm_window_unload,
  });
  window_stack_push(s_alarm_window, true);
}

// ---------- main window ----------
static void update_time(void) {
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  static char buf[16];
  strftime(buf, sizeof(buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
  text_layer_set_text(s_time_layer, buf);
}

static void update_config_text(void) {
  static char buf[64];
  snprintf(buf, sizeof(buf), "%d bpm x %ds\nin %d min", s_target_bpm, s_sustain_secs, s_delay_minutes);
  text_layer_set_text(s_config_layer, buf);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();
}

static void schedule_alarm(void) {
  time_t future_time = time(NULL) + (s_delay_minutes * 60);
  if (wakeup_query(s_wakeup_id, NULL)) {
    wakeup_cancel(s_wakeup_id);
  }
  s_wakeup_id = wakeup_schedule(future_time, WAKEUP_REASON_ALARM, true);
  persist_write_int(PKEY_WAKEUP_ID, s_wakeup_id);
  save_settings();
  text_layer_set_text(s_hint_layer, "Alarm armed");
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_target_bpm += 5;
  update_config_text();
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_target_bpm > 40) s_target_bpm -= 5;
  update_config_text();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  schedule_alarm();
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  // long-press SELECT: jump straight into alarm mode now, for testing without waiting
  enter_alarm_mode();
}

static void main_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_click_handler, NULL);
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_time_layer = text_layer_create(GRect(0, 15, bounds.size.w, 50));
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

  s_config_layer = text_layer_create(GRect(0, 75, bounds.size.w, 50));
  text_layer_set_font(s_config_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_config_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_config_layer));

  s_hint_layer = text_layer_create(GRect(0, 135, bounds.size.w, 45));
  text_layer_set_font(s_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_hint_layer, "UP/DOWN: bpm\nSELECT: arm\nhold SELECT: test now");
  layer_add_child(window_layer, text_layer_get_layer(s_hint_layer));

  update_time();
  update_config_text();
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_config_layer);
  text_layer_destroy(s_hint_layer);
}

static void wakeup_handler(WakeupId id, int32_t reason) {
  enter_alarm_mode();
}

static void init(void) {
  load_settings();

  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, main_click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  wakeup_service_subscribe(wakeup_handler);

  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id = 0;
    int32_t reason = 0;
    if (wakeup_get_launch_event(&id, &reason)) {
      enter_alarm_mode();
    }
  }
}

static void deinit(void) {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
