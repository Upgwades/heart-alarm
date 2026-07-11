#include <pebble.h>

// --- config ---
#define DEFAULT_TARGET_BPM 100
#define DEFAULT_SUSTAIN_SECS 10
#define DEFAULT_DELAY_MINUTES 1
#define DEFAULT_CALIBRATION_MIN_SECS 5
#define GRACE_SECS 20               // testing default; bumped up from a few seconds
#define GRACE_BUZZ_INTERVAL_SECS 4  // buzz every N seconds during grace, not every tick
#define HR_SAMPLE_PERIOD_SEC 1
#define TICK_MS 1000
#define WAKEUP_REASON_ALARM 0
#define ALARM_VOLUME 100

typedef enum {
  AlarmPhaseGrace,        // a few buzzes to rouse the wearer before we start reading HR
  AlarmPhaseCalibrating,  // waiting for a fresh HR sample + minimum settle time
  AlarmPhaseActive,       // normal threshold/sustain logic
} AlarmPhase;

static Window *s_main_window;
static TextLayer *s_time_layer;
static TextLayer *s_config_layer;
static TextLayer *s_hint_layer;

static Window *s_alarm_window;
static TextLayer *s_alarm_bpm_layer;
static TextLayer *s_alarm_status_layer;
static TextLayer *s_alarm_progress_layer;
static Layer *s_alarm_progress_bar_layer;

static float s_progress_fraction = 0.f;
static GColor s_progress_color;

static int s_target_bpm = DEFAULT_TARGET_BPM;
static int s_sustain_secs = DEFAULT_SUSTAIN_SECS;
static int s_delay_minutes = DEFAULT_DELAY_MINUTES;
static int s_calibration_min_secs = DEFAULT_CALIBRATION_MIN_SECS;

static int s_sustained_seconds = 0;
static int s_elapsed_seconds = 0;
static bool s_unlocked = false;
static AppTimer *s_alarm_timer;
static WakeupId s_wakeup_id;

static AlarmPhase s_phase = AlarmPhaseGrace;
static int s_grace_elapsed_secs = 0;
static int s_calibration_elapsed_secs = 0;

// gates on an actual HealthEventHeartRateUpdate rather than a fixed delay,
// since peek_current_value can keep returning a stale pre-alarm reading
// for an unpredictable amount of time until the sensor produces a real sample
static bool s_alarm_active = false;
static int s_fresh_hr_events = 0;

static void health_event_handler(HealthEventType event, void *context) {
  if (s_alarm_active && event == HealthEventHeartRateUpdate) {
    s_fresh_hr_events++;
  }
}

// ---------- persistence ----------
enum {
  PKEY_TARGET_BPM = 100,
  PKEY_SUSTAIN_SECS = 101,
  PKEY_DELAY_MIN = 102,
  PKEY_WAKEUP_ID = 103,
  PKEY_CALIBRATION_MIN_SECS = 104,
};

static void load_settings(void) {
  if (persist_exists(PKEY_TARGET_BPM)) s_target_bpm = persist_read_int(PKEY_TARGET_BPM);
  if (persist_exists(PKEY_SUSTAIN_SECS)) s_sustain_secs = persist_read_int(PKEY_SUSTAIN_SECS);
  if (persist_exists(PKEY_DELAY_MIN)) s_delay_minutes = persist_read_int(PKEY_DELAY_MIN);
  if (persist_exists(PKEY_CALIBRATION_MIN_SECS)) s_calibration_min_secs = persist_read_int(PKEY_CALIBRATION_MIN_SECS);
}

static void save_settings(void) {
  persist_write_int(PKEY_TARGET_BPM, s_target_bpm);
  persist_write_int(PKEY_SUSTAIN_SECS, s_sustain_secs);
  persist_write_int(PKEY_DELAY_MIN, s_delay_minutes);
  persist_write_int(PKEY_CALIBRATION_MIN_SECS, s_calibration_min_secs);
}

// ---------- alarm window ----------
static void start_hr_alarm_vibe(void) {
  // near-continuous hard buzzing: long pulses, almost no gap
  static const uint32_t segments[] = {700, 50, 700, 50, 700, 50, 700, 50};
  VibePattern pattern = {
    .durations = segments,
    .num_segments = ARRAY_LENGTH(segments),
  };
  vibes_enqueue_custom_pattern(pattern);
}

static void start_hr_alarm_siren(void) {
  // blaring two-tone siren, one cycle per tick (matches TICK_MS)
  static const SpeakerNote notes[] = {
    { .midi_note = 91, .waveform = SpeakerWaveformSquare, .duration_ms = 250, .velocity = 127 },
    { .midi_note = 86, .waveform = SpeakerWaveformSquare, .duration_ms = 250, .velocity = 127 },
    { .midi_note = 91, .waveform = SpeakerWaveformSquare, .duration_ms = 250, .velocity = 127 },
    { .midi_note = 86, .waveform = SpeakerWaveformSquare, .duration_ms = 250, .velocity = 127 },
  };
  speaker_play_notes(notes, ARRAY_LENGTH(notes), ALARM_VOLUME);
}

static void progress_bar_draw_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, bounds, 6);

  GRect fill = GRect(bounds.origin.x + 3, bounds.origin.y + 3,
                      (int16_t)((bounds.size.w - 6) * s_progress_fraction), bounds.size.h - 6);
  if (fill.size.w > 0) {
    graphics_context_set_fill_color(ctx, s_progress_color);
    graphics_fill_rect(ctx, fill, 4, GCornersAll);
  }
}

// applies the background/text colors and progress bar for the current
// phase/state, so the whole screen communicates urgency at a glance
static void apply_alarm_theme(int bpm) {
  GColor bg, fg;

  if (s_unlocked) {
    bg = PBL_IF_COLOR_ELSE(GColorGreen, GColorWhite);
    fg = GColorBlack;
    s_progress_color = GColorBlack;
    s_progress_fraction = 1.f;
  } else if (s_phase == AlarmPhaseGrace) {
    bg = PBL_IF_COLOR_ELSE(GColorOrange, GColorWhite);
    fg = GColorBlack;
    s_progress_color = GColorBlack;
    s_progress_fraction = (float)s_grace_elapsed_secs / GRACE_SECS;
  } else if (s_phase == AlarmPhaseCalibrating) {
    bg = PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite);
    fg = GColorBlack;
    s_progress_color = GColorBlack;
    s_progress_fraction = (float)s_calibration_elapsed_secs / s_calibration_min_secs;
  } else {
    // active phase: red while below threshold (flashing for urgency),
    // shifting toward green as sustain progress builds
    bool below = bpm < s_target_bpm;
    if (below) {
      bool flash_on = (s_elapsed_seconds % 2) == 0;
      bg = PBL_IF_COLOR_ELSE(flash_on ? GColorRed : GColorDarkCandyAppleRed,
                              flash_on ? GColorWhite : GColorBlack);
      fg = flash_on ? GColorWhite : PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack);
    } else {
      bg = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite);
      fg = GColorBlack;
    }
    s_progress_color = PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack);
    s_progress_fraction = s_sustain_secs > 0 ? (float)s_sustained_seconds / s_sustain_secs : 0.f;
  }

  window_set_background_color(s_alarm_window, bg);
  text_layer_set_text_color(s_alarm_bpm_layer, fg);
  text_layer_set_text_color(s_alarm_status_layer, fg);
  text_layer_set_text_color(s_alarm_progress_layer, fg);
  if (s_alarm_progress_bar_layer) {
    layer_mark_dirty(s_alarm_progress_bar_layer);
  }
}

static void update_alarm_ui(int bpm) {
  static char bpm_buf[32];
  static char status_buf[48];
  static char progress_buf[24];

  bool reading_trustworthy = (s_phase == AlarmPhaseActive) || s_unlocked;
  if (bpm <= 0 || !reading_trustworthy) {
    // don't show a number until it's confirmed fresh, to avoid displaying
    // a stale reading left over from before the alarm started
    snprintf(bpm_buf, sizeof(bpm_buf), "-- BPM");
  } else {
    snprintf(bpm_buf, sizeof(bpm_buf), "%d BPM", bpm);
  }
  text_layer_set_text(s_alarm_bpm_layer, bpm_buf);

  if (s_unlocked) {
    snprintf(status_buf, sizeof(status_buf), "UNLOCKED");
    snprintf(progress_buf, sizeof(progress_buf), "press SELECT to stop");
  } else if (s_phase == AlarmPhaseGrace) {
    snprintf(status_buf, sizeof(status_buf), "WAKE UP");
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d s", s_grace_elapsed_secs, GRACE_SECS);
  } else if (s_phase == AlarmPhaseCalibrating) {
    snprintf(status_buf, sizeof(status_buf), "Calibrating...");
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d s", s_calibration_elapsed_secs, s_calibration_min_secs);
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

  apply_alarm_theme(bpm);
}

static void alarm_tick(void *data) {
  HealthValue raw = health_service_peek_current_value(HealthMetricHeartRateRawBPM);
  int bpm = (int)raw;

  if (s_phase == AlarmPhaseGrace) {
    if (s_grace_elapsed_secs % GRACE_BUZZ_INTERVAL_SECS == 0) {
      vibes_short_pulse();
    }
    s_grace_elapsed_secs++;
    if (s_grace_elapsed_secs >= GRACE_SECS) {
      s_phase = AlarmPhaseCalibrating;
      s_calibration_elapsed_secs = 0;
    }
    update_alarm_ui(bpm);
    s_alarm_timer = app_timer_register(TICK_MS, alarm_tick, NULL);
    return;
  }

  if (s_phase == AlarmPhaseCalibrating) {
    s_calibration_elapsed_secs++;
    // needs both the minimum settle time AND at least one confirmed-fresh
    // reading; a fixed delay alone can't guarantee the sensor has produced
    // a real sample yet
    if (s_calibration_elapsed_secs >= s_calibration_min_secs && s_fresh_hr_events >= 1) {
      s_phase = AlarmPhaseActive;
    }
  }

  bool calibrating = s_phase != AlarmPhaseActive;

  if (!s_unlocked && !calibrating) {
    if (bpm >= s_target_bpm) {
      s_sustained_seconds++;
      if (s_sustained_seconds >= s_sustain_secs) {
        s_unlocked = true;
      }
    } else {
      s_sustained_seconds = 0;
    }
  }
  s_elapsed_seconds++;

  // sound + vibration only run while HR is below the target threshold.
  // During calibration the reading may still be the stale pre-alarm value,
  // so stay silent until it's had time to settle.
  bool below_threshold = !s_unlocked && !calibrating && bpm < s_target_bpm;
  if (below_threshold) {
    start_hr_alarm_vibe();
    start_hr_alarm_siren();
  } else {
    speaker_stop();
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
  vibes_cancel();
  speaker_stop();
  s_alarm_active = false;
  health_service_set_heart_rate_sample_period(0);
  window_stack_pop(true);
}

static void alarm_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, alarm_select_click_handler);
}

static void alarm_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int16_t h = bounds.size.h;

  s_alarm_bpm_layer = text_layer_create(GRect(0, h / 10, bounds.size.w, h / 4));
  text_layer_set_background_color(s_alarm_bpm_layer, GColorClear);
  text_layer_set_font(s_alarm_bpm_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_alarm_bpm_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_alarm_bpm_layer));

  s_alarm_status_layer = text_layer_create(GRect(0, h * 38 / 100, bounds.size.w, h / 4));
  text_layer_set_background_color(s_alarm_status_layer, GColorClear);
  text_layer_set_font(s_alarm_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_alarm_status_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_alarm_status_layer));

  s_alarm_progress_bar_layer = layer_create(GRect(20, h * 66 / 100, bounds.size.w - 40, 18));
  layer_set_update_proc(s_alarm_progress_bar_layer, progress_bar_draw_proc);
  layer_add_child(window_layer, s_alarm_progress_bar_layer);

  s_alarm_progress_layer = text_layer_create(GRect(0, h * 66 / 100 + 24, bounds.size.w, 24));
  text_layer_set_background_color(s_alarm_progress_layer, GColorClear);
  text_layer_set_font(s_alarm_progress_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_alarm_progress_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_alarm_progress_layer));

  vibes_cancel();
  speaker_stop();
  health_service_set_heart_rate_sample_period(HR_SAMPLE_PERIOD_SEC);
  s_sustained_seconds = 0;
  s_elapsed_seconds = 0;
  s_fresh_hr_events = 0;
  s_alarm_active = true;
  s_unlocked = false;
  s_phase = AlarmPhaseGrace;
  s_grace_elapsed_secs = 0;
  s_calibration_elapsed_secs = 0;
  update_alarm_ui(0);

  s_alarm_timer = app_timer_register(TICK_MS, alarm_tick, NULL);
}

static void alarm_window_unload(Window *window) {
  vibes_cancel();
  speaker_stop();
  s_alarm_active = false;
  layer_destroy(s_alarm_progress_bar_layer);
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

typedef enum {
  FieldTargetBpm,
  FieldSustainSecs,
  FieldDelayMinutes,
  FieldCalibrationMinSecs,
  FieldCount,
} SettingField;

static SettingField s_active_field = FieldTargetBpm;

static void update_config_text(void) {
  static char buf[80];
  snprintf(buf, sizeof(buf), "%d bpm x %ds\nin %d min, calib %ds",
           s_target_bpm, s_sustain_secs, s_delay_minutes, s_calibration_min_secs);
  text_layer_set_text(s_config_layer, buf);
}

static void update_hint_text(void) {
  static char buf[80];
  const char *field_name;
  switch (s_active_field) {
    case FieldTargetBpm: field_name = "target bpm"; break;
    case FieldSustainSecs: field_name = "sustain secs"; break;
    case FieldDelayMinutes: field_name = "delay min"; break;
    default: field_name = "calib min secs"; break;
  }
  snprintf(buf, sizeof(buf), "UP/DOWN: %s\nBACK: change field\nhold SELECT: test now", field_name);
  text_layer_set_text(s_hint_layer, buf);
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

static void adjust_active_field(int delta) {
  switch (s_active_field) {
    case FieldTargetBpm:
      s_target_bpm += delta * 5;
      if (s_target_bpm < 40) s_target_bpm = 40;
      break;
    case FieldSustainSecs:
      s_sustain_secs += delta * 5;
      if (s_sustain_secs < 5) s_sustain_secs = 5;
      break;
    case FieldDelayMinutes:
      s_delay_minutes += delta;
      if (s_delay_minutes < 0) s_delay_minutes = 0;
      break;
    case FieldCalibrationMinSecs:
    default:
      s_calibration_min_secs += delta;
      if (s_calibration_min_secs < 0) s_calibration_min_secs = 0;
      break;
  }
  update_config_text();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  adjust_active_field(1);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  adjust_active_field(-1);
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_active_field = (s_active_field + 1) % FieldCount;
  update_hint_text();
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
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_click_handler, NULL);
}

static void accent_bar_draw_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorElectricBlue, GColorBlack));
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

static Layer *s_accent_bar_layer;

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  window_set_background_color(window, PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorWhite));

  s_time_layer = text_layer_create(GRect(0, 15, bounds.size.w, 50));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

  s_accent_bar_layer = layer_create(GRect(bounds.size.w / 4, 62, bounds.size.w / 2, 3));
  layer_set_update_proc(s_accent_bar_layer, accent_bar_draw_proc);
  layer_add_child(window_layer, s_accent_bar_layer);

  s_config_layer = text_layer_create(GRect(0, 72, bounds.size.w, 55));
  text_layer_set_background_color(s_config_layer, GColorClear);
  text_layer_set_text_color(s_config_layer, PBL_IF_COLOR_ELSE(GColorElectricBlue, GColorBlack));
  text_layer_set_font(s_config_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_config_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_config_layer));

  s_hint_layer = text_layer_create(GRect(0, 130, bounds.size.w, 60));
  text_layer_set_background_color(s_hint_layer, GColorClear);
  text_layer_set_text_color(s_hint_layer, PBL_IF_COLOR_ELSE(GColorLightGray, GColorDarkGray));
  text_layer_set_font(s_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_hint_layer));

  update_time();
  update_config_text();
  update_hint_text();
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  layer_destroy(s_accent_bar_layer);
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
  health_service_events_subscribe(health_event_handler, NULL);

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
