#include <pebble.h>

// --- config ---
#define DEFAULT_TARGET_BPM 100
#define DEFAULT_SUSTAIN_SECS 10
#define DEFAULT_CALIBRATION_MIN_SECS 5
#define GRACE_SECS 20               // testing default; bumped up from a few seconds
#define GRACE_BUZZ_INTERVAL_SECS 4  // buzz every N seconds during grace, not every tick
#define HR_SAMPLE_PERIOD_SEC 1
#define TICK_MS 1000
#define ALARM_VOLUME 100
#define MAX_ALARMS 5

typedef enum {
  AlarmPhaseGrace,        // a few buzzes to rouse the wearer before we start reading HR
  AlarmPhaseCalibrating,  // waiting for a fresh HR sample + minimum settle time
  AlarmPhaseActive,       // normal threshold/sustain logic
} AlarmPhase;

// one persisted alarm slot. days_mask bit i corresponds to struct tm's
// tm_wday (0=Sunday .. 6=Saturday); a mask of 0 means "next occurrence only,
// don't repeat".
typedef struct {
  bool enabled;
  uint8_t hour;
  uint8_t minute;
  uint8_t days_mask;
  int32_t wakeup_id;  // -1 if nothing currently scheduled
} Alarm;

static Alarm s_alarms[MAX_ALARMS];
static int s_active_alarm_index = -1;  // which alarm fired the current alarm session, -1 = manual test

static Window *s_list_window;
static TextLayer *s_list_title_layer;
static TextLayer *s_list_body_layer;
static TextLayer *s_list_hint_layer;
static int s_list_cursor = 0;  // 0..MAX_ALARMS-1 = alarm rows, MAX_ALARMS = "HR settings" row

static Window *s_edit_window;
static TextLayer *s_edit_big_layer;
static TextLayer *s_edit_summary_layer;
static TextLayer *s_edit_hint_layer;
static int s_editing_alarm_index = -1;

typedef enum {
  EFEnabled, EFHour, EFMinute, EFSun, EFMon, EFTue, EFWed, EFThu, EFFri, EFSat, EFCount,
} EditField;
static EditField s_edit_field = EFEnabled;

static Window *s_settings_window;
static TextLayer *s_settings_big_layer;
static TextLayer *s_settings_summary_layer;
static TextLayer *s_settings_hint_layer;

typedef enum {
  FieldTargetBpm, FieldSustainSecs, FieldCalibrationMinSecs, FieldCount,
} SettingField;
static SettingField s_active_field = FieldTargetBpm;

static Window *s_alarm_window;
static TextLayer *s_alarm_bpm_layer;
static TextLayer *s_alarm_status_layer;
static TextLayer *s_alarm_progress_layer;
static Layer *s_alarm_progress_bar_layer;

static float s_progress_fraction = 0.f;
static GColor s_progress_color;

static int s_target_bpm = DEFAULT_TARGET_BPM;
static int s_sustain_secs = DEFAULT_SUSTAIN_SECS;
static int s_calibration_min_secs = DEFAULT_CALIBRATION_MIN_SECS;

static int s_sustained_seconds = 0;
static int s_elapsed_seconds = 0;
static bool s_unlocked = false;
static AppTimer *s_alarm_timer;

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
  PKEY_CALIBRATION_MIN_SECS = 104,
  PKEY_ALARMS = 110,
};

static void load_settings(void) {
  if (persist_exists(PKEY_TARGET_BPM)) s_target_bpm = persist_read_int(PKEY_TARGET_BPM);
  if (persist_exists(PKEY_SUSTAIN_SECS)) s_sustain_secs = persist_read_int(PKEY_SUSTAIN_SECS);
  if (persist_exists(PKEY_CALIBRATION_MIN_SECS)) s_calibration_min_secs = persist_read_int(PKEY_CALIBRATION_MIN_SECS);

  if (persist_exists(PKEY_ALARMS)) {
    persist_read_data(PKEY_ALARMS, s_alarms, sizeof(s_alarms));
  } else {
    for (int i = 0; i < MAX_ALARMS; i++) {
      s_alarms[i] = (Alarm){ .enabled = false, .hour = 7, .minute = 0, .days_mask = 0, .wakeup_id = -1 };
    }
  }
}

static void save_settings(void) {
  persist_write_int(PKEY_TARGET_BPM, s_target_bpm);
  persist_write_int(PKEY_SUSTAIN_SECS, s_sustain_secs);
  persist_write_int(PKEY_CALIBRATION_MIN_SECS, s_calibration_min_secs);
}

static void save_alarms(void) {
  persist_write_data(PKEY_ALARMS, s_alarms, sizeof(s_alarms));
}

// ---------- alarm scheduling ----------
static time_t compute_next_occurrence(Alarm *a, time_t now) {
  struct tm *now_tm = localtime(&now);

  if (a->days_mask == 0) {
    struct tm c = *now_tm;
    c.tm_hour = a->hour;
    c.tm_min = a->minute;
    c.tm_sec = 0;
    time_t t = mktime(&c);
    if (t <= now) t += 24 * 60 * 60;
    return t;
  }

  for (int i = 0; i < 8; i++) {
    struct tm c = *now_tm;
    c.tm_mday += i;
    c.tm_hour = a->hour;
    c.tm_min = a->minute;
    c.tm_sec = 0;
    time_t t = mktime(&c);
    struct tm *norm = localtime(&t);
    if ((a->days_mask & (1 << norm->tm_wday)) && t > now) {
      return t;
    }
  }
  return now + 24 * 60 * 60;  // shouldn't happen, mask covers a full week
}

static void schedule_alarm_index(int idx) {
  Alarm *a = &s_alarms[idx];
  if (a->wakeup_id >= 0 && wakeup_query(a->wakeup_id, NULL)) {
    wakeup_cancel(a->wakeup_id);
  }
  if (!a->enabled) {
    a->wakeup_id = -1;
  } else {
    time_t next = compute_next_occurrence(a, time(NULL));
    a->wakeup_id = wakeup_schedule(next, idx, true);
  }
  save_alarms();
}

static void reschedule_all_enabled(void) {
  for (int i = 0; i < MAX_ALARMS; i++) {
    Alarm *a = &s_alarms[i];
    if (a->enabled && (a->wakeup_id < 0 || !wakeup_query(a->wakeup_id, NULL))) {
      schedule_alarm_index(i);
    }
  }
}

static void format_days_summary(uint8_t mask, char *buf, size_t n) {
  static const char *const names[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
  if (mask == 0) {
    snprintf(buf, n, "Once");
  } else if (mask == 0x7F) {
    snprintf(buf, n, "Daily");
  } else {
    buf[0] = '\0';
    for (int i = 0; i < 7; i++) {
      if (mask & (1 << i)) {
        if (buf[0] != '\0') strncat(buf, " ", n - strlen(buf) - 1);
        strncat(buf, names[i], n - strlen(buf) - 1);
      }
    }
  }
}

// ---------- alarm window ----------
static void start_grace_vibe(void) {
  // stronger than a single short pulse: three firm buzzes back to back
  static const uint32_t segments[] = {400, 100, 400, 100, 400};
  VibePattern pattern = {
    .durations = segments,
    .num_segments = ARRAY_LENGTH(segments),
  };
  vibes_enqueue_custom_pattern(pattern);
}

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
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d seconds", s_grace_elapsed_secs, GRACE_SECS);
  } else if (s_phase == AlarmPhaseCalibrating) {
    snprintf(status_buf, sizeof(status_buf), "Calibrating...");
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d seconds", s_calibration_elapsed_secs, s_calibration_min_secs);
  } else if (bpm <= 0) {
    snprintf(status_buf, sizeof(status_buf), "Measuring...");
    snprintf(progress_buf, sizeof(progress_buf), "target %d bpm", s_target_bpm);
  } else if (bpm >= s_target_bpm) {
    snprintf(status_buf, sizeof(status_buf), "Keep it up!");
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d seconds", s_sustained_seconds, s_sustain_secs);
  } else {
    snprintf(status_buf, sizeof(status_buf), "Get moving!\nneed %d bpm", s_target_bpm);
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d seconds", s_sustained_seconds, s_sustain_secs);
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
      start_grace_vibe();
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

  if (s_active_alarm_index >= 0) {
    // this alarm was a real scheduled one, not a manual test: line up its
    // next occurrence per its day-of-week repeat mask
    schedule_alarm_index(s_active_alarm_index);
    s_active_alarm_index = -1;
  }

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

// ---------- HR settings window (shared across all alarms) ----------
static void settings_update_text(void) {
  static char big[16];
  static char summary[96];
  static char hint[100];

  switch (s_active_field) {
    case FieldTargetBpm: snprintf(big, sizeof(big), "%d", s_target_bpm); break;
    case FieldSustainSecs: snprintf(big, sizeof(big), "%d", s_sustain_secs); break;
    default: snprintf(big, sizeof(big), "%d", s_calibration_min_secs); break;
  }
  text_layer_set_text(s_settings_big_layer, big);

  snprintf(summary, sizeof(summary), "target %d bpm\nsustain %d seconds\ncalibration minimum %d seconds",
           s_target_bpm, s_sustain_secs, s_calibration_min_secs);
  text_layer_set_text(s_settings_summary_layer, summary);

  const char *field_name;
  switch (s_active_field) {
    case FieldTargetBpm: field_name = "target heart rate"; break;
    case FieldSustainSecs: field_name = "sustain duration"; break;
    default: field_name = "calibration minimum"; break;
  }
  snprintf(hint, sizeof(hint), "editing: %s\nUP/DOWN: change\nSELECT: next field\nhold SELECT: save & exit", field_name);
  text_layer_set_text(s_settings_hint_layer, hint);
}

static void settings_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_active_field) {
    case FieldTargetBpm: s_target_bpm += 5; break;
    case FieldSustainSecs: s_sustain_secs += 5; break;
    default: s_calibration_min_secs += 1; break;
  }
  save_settings();
  settings_update_text();
}

static void settings_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_active_field) {
    case FieldTargetBpm: if (s_target_bpm > 40) s_target_bpm -= 5; break;
    case FieldSustainSecs: if (s_sustain_secs > 5) s_sustain_secs -= 5; break;
    default: if (s_calibration_min_secs > 0) s_calibration_min_secs -= 1; break;
  }
  save_settings();
  settings_update_text();
}

static void settings_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_active_field = (s_active_field + 1) % FieldCount;
  settings_update_text();
}

static void settings_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  // long-press SELECT: done editing, save and go back to the alarm list
  save_settings();
  window_stack_pop(true);
}

static void settings_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, settings_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, settings_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, settings_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, settings_select_long_click_handler, NULL);
}

static void settings_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int16_t h = bounds.size.h;

  window_set_background_color(window, PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorWhite));

  s_settings_big_layer = text_layer_create(GRect(0, h * 8 / 100, bounds.size.w, h * 26 / 100));
  text_layer_set_background_color(s_settings_big_layer, GColorClear);
  text_layer_set_text_color(s_settings_big_layer, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  text_layer_set_font(s_settings_big_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_settings_big_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_settings_big_layer));

  s_settings_summary_layer = text_layer_create(GRect(0, h * 36 / 100, bounds.size.w, h * 26 / 100));
  text_layer_set_background_color(s_settings_summary_layer, GColorClear);
  text_layer_set_text_color(s_settings_summary_layer, PBL_IF_COLOR_ELSE(GColorElectricBlue, GColorBlack));
  text_layer_set_font(s_settings_summary_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_settings_summary_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_settings_summary_layer));

  s_settings_hint_layer = text_layer_create(GRect(0, h * 64 / 100, bounds.size.w, h * 36 / 100));
  text_layer_set_background_color(s_settings_hint_layer, GColorClear);
  text_layer_set_text_color(s_settings_hint_layer, PBL_IF_COLOR_ELSE(GColorLightGray, GColorDarkGray));
  text_layer_set_font(s_settings_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_settings_hint_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_settings_hint_layer));

  settings_update_text();
}

static void settings_window_unload(Window *window) {
  text_layer_destroy(s_settings_big_layer);
  text_layer_destroy(s_settings_summary_layer);
  text_layer_destroy(s_settings_hint_layer);
}

static void enter_settings_screen(void) {
  s_active_field = FieldTargetBpm;
  s_settings_window = window_create();
  window_set_click_config_provider(s_settings_window, settings_click_config_provider);
  window_set_window_handlers(s_settings_window, (WindowHandlers) {
    .load = settings_window_load,
    .unload = settings_window_unload,
  });
  window_stack_push(s_settings_window, true);
}

// ---------- per-alarm edit window ----------
static void edit_update_text(void) {
  Alarm *a = &s_alarms[s_editing_alarm_index];
  static char big[16];
  static char summary[64];
  static char hint[100];
  static char days_buf[32];

  switch (s_edit_field) {
    case EFEnabled: snprintf(big, sizeof(big), "%s", a->enabled ? "ON" : "OFF"); break;
    case EFHour: snprintf(big, sizeof(big), "%02d", a->hour); break;
    case EFMinute: snprintf(big, sizeof(big), "%02d", a->minute); break;
    default: {
      int day = s_edit_field - EFSun;
      static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
      snprintf(big, sizeof(big), "%s %s", names[day], (a->days_mask & (1 << day)) ? "ON" : "OFF");
      break;
    }
  }
  text_layer_set_text(s_edit_big_layer, big);

  format_days_summary(a->days_mask, days_buf, sizeof(days_buf));
  snprintf(summary, sizeof(summary), "%02d:%02d  %s\n%s", a->hour, a->minute, days_buf,
           a->enabled ? "enabled" : "disabled");
  text_layer_set_text(s_edit_summary_layer, summary);

  const char *field_name;
  switch (s_edit_field) {
    case EFEnabled: field_name = "on or off"; break;
    case EFHour: field_name = "hour"; break;
    case EFMinute: field_name = "minute"; break;
    default: field_name = "day toggle"; break;
  }
  snprintf(hint, sizeof(hint), "editing: %s\nUP/DOWN: change\nSELECT: next field\nhold SELECT: save & exit", field_name);
  text_layer_set_text(s_edit_hint_layer, hint);
}

static void edit_apply_and_reschedule(void) {
  save_alarms();
  schedule_alarm_index(s_editing_alarm_index);
  edit_update_text();
}

static void edit_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  Alarm *a = &s_alarms[s_editing_alarm_index];
  switch (s_edit_field) {
    case EFEnabled: a->enabled = !a->enabled; break;
    case EFHour: a->hour = (a->hour + 1) % 24; break;
    case EFMinute: a->minute = (a->minute + 5) % 60; break;
    default: a->days_mask ^= (1 << (s_edit_field - EFSun)); break;
  }
  edit_apply_and_reschedule();
}

static void edit_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  Alarm *a = &s_alarms[s_editing_alarm_index];
  switch (s_edit_field) {
    case EFEnabled: a->enabled = !a->enabled; break;
    case EFHour: a->hour = (a->hour + 23) % 24; break;
    case EFMinute: a->minute = (a->minute + 55) % 60; break;
    default: a->days_mask ^= (1 << (s_edit_field - EFSun)); break;
  }
  edit_apply_and_reschedule();
}

static void edit_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_edit_field = (s_edit_field + 1) % EFCount;
  edit_update_text();
}

static void edit_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  // long-press SELECT: done editing this alarm, save and go back to the alarm list
  save_alarms();
  schedule_alarm_index(s_editing_alarm_index);
  window_stack_pop(true);
}

static void edit_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, edit_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, edit_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, edit_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, edit_select_long_click_handler, NULL);
}

static void edit_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int16_t h = bounds.size.h;

  window_set_background_color(window, PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorWhite));

  s_edit_big_layer = text_layer_create(GRect(0, h * 8 / 100, bounds.size.w, h * 26 / 100));
  text_layer_set_background_color(s_edit_big_layer, GColorClear);
  text_layer_set_text_color(s_edit_big_layer, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  text_layer_set_font(s_edit_big_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_edit_big_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_edit_big_layer));

  s_edit_summary_layer = text_layer_create(GRect(0, h * 36 / 100, bounds.size.w, h * 26 / 100));
  text_layer_set_background_color(s_edit_summary_layer, GColorClear);
  text_layer_set_text_color(s_edit_summary_layer, PBL_IF_COLOR_ELSE(GColorElectricBlue, GColorBlack));
  text_layer_set_font(s_edit_summary_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_edit_summary_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_edit_summary_layer));

  s_edit_hint_layer = text_layer_create(GRect(0, h * 64 / 100, bounds.size.w, h * 36 / 100));
  text_layer_set_background_color(s_edit_hint_layer, GColorClear);
  text_layer_set_text_color(s_edit_hint_layer, PBL_IF_COLOR_ELSE(GColorLightGray, GColorDarkGray));
  text_layer_set_font(s_edit_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_edit_hint_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_edit_hint_layer));

  s_edit_field = EFEnabled;
  edit_update_text();
}

static void edit_window_unload(Window *window) {
  text_layer_destroy(s_edit_big_layer);
  text_layer_destroy(s_edit_summary_layer);
  text_layer_destroy(s_edit_hint_layer);
}

static void enter_edit_screen(int alarm_index) {
  s_editing_alarm_index = alarm_index;
  s_edit_window = window_create();
  window_set_click_config_provider(s_edit_window, edit_click_config_provider);
  window_set_window_handlers(s_edit_window, (WindowHandlers) {
    .load = edit_window_load,
    .unload = edit_window_unload,
  });
  window_stack_push(s_edit_window, true);
}

// ---------- alarm list window (app entry point) ----------
static void list_update_title(void) {
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  static char buf[24];
  strftime(buf, sizeof(buf), clock_is_24h_style() ? "Alarms  %H:%M" : "Alarms  %I:%M", tick_time);
  text_layer_set_text(s_list_title_layer, buf);
}

static void list_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  list_update_title();
}

static void list_update_text(void) {
  static char body[256];
  char days_buf[32];
  size_t off = 0;

  for (int i = 0; i < MAX_ALARMS && off < sizeof(body); i++) {
    Alarm *a = &s_alarms[i];
    const char *cursor = (s_list_cursor == i) ? ">" : " ";
    if (a->enabled) {
      format_days_summary(a->days_mask, days_buf, sizeof(days_buf));
      off += snprintf(body + off, sizeof(body) - off, "%s %02d:%02d %s\n", cursor, a->hour, a->minute, days_buf);
    } else {
      off += snprintf(body + off, sizeof(body) - off, "%s -- off --\n", cursor);
    }
  }
  const char *settings_cursor = (s_list_cursor == MAX_ALARMS) ? ">" : " ";
  snprintf(body + off, sizeof(body) - off, "%s HR settings", settings_cursor);

  text_layer_set_text(s_list_body_layer, body);
}

static void list_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_list_cursor = (s_list_cursor + MAX_ALARMS) % (MAX_ALARMS + 1);
  list_update_text();
}

static void list_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_list_cursor = (s_list_cursor + 1) % (MAX_ALARMS + 1);
  list_update_text();
}

static void list_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_list_cursor == MAX_ALARMS) {
    enter_settings_screen();
  } else {
    enter_edit_screen(s_list_cursor);
  }
}

static void list_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  // long-press SELECT: jump straight into alarm mode now, for testing without waiting
  s_active_alarm_index = -1;
  enter_alarm_mode();
}

static void list_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, list_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, list_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, list_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, list_select_long_click_handler, NULL);
}

static void accent_bar_draw_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorElectricBlue, GColorBlack));
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

static Layer *s_accent_bar_layer;

static void list_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  window_set_background_color(window, PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorWhite));

  s_list_title_layer = text_layer_create(GRect(0, 10, bounds.size.w, 30));
  text_layer_set_background_color(s_list_title_layer, GColorClear);
  text_layer_set_text_color(s_list_title_layer, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  text_layer_set_font(s_list_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_list_title_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_list_title_layer));
  list_update_title();

  s_accent_bar_layer = layer_create(GRect(bounds.size.w / 4, 44, bounds.size.w / 2, 3));
  layer_set_update_proc(s_accent_bar_layer, accent_bar_draw_proc);
  layer_add_child(window_layer, s_accent_bar_layer);

  s_list_body_layer = text_layer_create(GRect(0, 52, bounds.size.w, 110));
  text_layer_set_background_color(s_list_body_layer, GColorClear);
  text_layer_set_text_color(s_list_body_layer, PBL_IF_COLOR_ELSE(GColorElectricBlue, GColorBlack));
  text_layer_set_font(s_list_body_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_list_body_layer, GTextAlignmentLeft);
  layer_add_child(window_layer, text_layer_get_layer(s_list_body_layer));

  s_list_hint_layer = text_layer_create(GRect(0, bounds.size.h - 50, bounds.size.w, 50));
  text_layer_set_background_color(s_list_hint_layer, GColorClear);
  text_layer_set_text_color(s_list_hint_layer, PBL_IF_COLOR_ELSE(GColorLightGray, GColorDarkGray));
  text_layer_set_font(s_list_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_list_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_list_hint_layer, "SELECT: edit  hold SELECT: test now");
  layer_add_child(window_layer, text_layer_get_layer(s_list_hint_layer));

  list_update_text();
}

static void list_window_unload(Window *window) {
  text_layer_destroy(s_list_title_layer);
  layer_destroy(s_accent_bar_layer);
  text_layer_destroy(s_list_body_layer);
  text_layer_destroy(s_list_hint_layer);
}

static void list_window_appear(Window *window) {
  // refresh immediately when returning from the edit/settings screens,
  // rather than waiting for the next UP/DOWN press
  list_update_title();
  list_update_text();
}

static void wakeup_handler(WakeupId id, int32_t reason) {
  s_active_alarm_index = (int)reason;
  enter_alarm_mode();
}

static void init(void) {
  load_settings();
  reschedule_all_enabled();

  s_list_window = window_create();
  window_set_click_config_provider(s_list_window, list_click_config_provider);
  window_set_window_handlers(s_list_window, (WindowHandlers) {
    .load = list_window_load,
    .unload = list_window_unload,
    .appear = list_window_appear,
  });
  window_stack_push(s_list_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, list_tick_handler);
  wakeup_service_subscribe(wakeup_handler);
  health_service_events_subscribe(health_event_handler, NULL);

  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id = 0;
    int32_t reason = 0;
    if (wakeup_get_launch_event(&id, &reason)) {
      s_active_alarm_index = (int)reason;
      enter_alarm_mode();
    }
  }
}

static void deinit(void) {
  window_destroy(s_list_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
