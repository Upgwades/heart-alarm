#include <pebble.h>

// --- config ---
#define DEFAULT_TARGET_BPM 100
#define DEFAULT_SUSTAIN_SECS 10
#define DEFAULT_GRACE_SECS 20
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

#define LIST_TOTAL_ROWS (MAX_ALARMS + 1)
#define LIST_WINDOW_ROWS 3

static Window *s_list_window;
static TextLayer *s_list_title_layer;
static TextLayer *s_list_row_layers[LIST_WINDOW_ROWS];
static TextLayer *s_list_hint_layer;
static int s_list_cursor = 0;  // 0 = "HR settings" row, 1..MAX_ALARMS = alarm rows
static int s_list_scroll = 0;  // index of the item shown in the topmost visible row

typedef enum {
  EFEnabled, EFHour, EFMinute, EFSun, EFMon, EFTue, EFWed, EFThu, EFFri, EFSat, EFCount,
} EditField;

#define EDIT_VISIBLE_ROWS 4

static Window *s_edit_window;
static TextLayer *s_edit_field_layers[EDIT_VISIBLE_ROWS];
static TextLayer *s_edit_hint_layer;
static int s_editing_alarm_index = -1;
static EditField s_edit_field = EFEnabled;
static int s_edit_scroll = 0;  // index of the field shown in the topmost visible row

typedef enum {
  FieldTargetBpm, FieldSustainSecs, FieldGraceSecs, FieldCount,
} SettingField;

static Window *s_settings_window;
static TextLayer *s_settings_field_layers[FieldCount];
static TextLayer *s_settings_hint_layer;
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
static int s_grace_secs = DEFAULT_GRACE_SECS;

static int s_sustained_seconds = 0;
static int s_elapsed_seconds = 0;
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
  PKEY_GRACE_SECS = 105,
  PKEY_ALARMS = 110,
};

static void load_settings(void) {
  if (persist_exists(PKEY_TARGET_BPM)) s_target_bpm = persist_read_int(PKEY_TARGET_BPM);
  if (persist_exists(PKEY_SUSTAIN_SECS)) s_sustain_secs = persist_read_int(PKEY_SUSTAIN_SECS);
  if (persist_exists(PKEY_GRACE_SECS)) s_grace_secs = persist_read_int(PKEY_GRACE_SECS);

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
  persist_write_int(PKEY_GRACE_SECS, s_grace_secs);
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

// ---------- pixel heart (unlocked/success celebration) ----------
// hand-authored bitmap + an integer pulse-size table: deliberately zero
// floating-point/libm calls (no sinf, no curve math) after those were the
// prime suspect in a hard fault the first time this screen ever rendered
#define HEART_GRID_W 17
#define HEART_GRID_H 15
#define HEART_OUTLINE_THICKNESS 2

// per-row filled column ranges (inclusive); a second span lets the top two
// rows show the heart's twin lobes before they merge. {-1,-1} = unused.
typedef struct { int8_t l1, r1, l2, r2; } HeartRowSpans;
static const HeartRowSpans kHeartRows[HEART_GRID_H] = {
  { 2,  5, 11, 14},  // row 0: two separate lobe tops
  { 1,  6, 10, 15},  // row 1
  { 0,  7,  9, 16},  // row 2: lobes almost touching
  { 0, 16, -1, -1},  // row 3: lobes merged, widest point
  { 0, 16, -1, -1},  // row 4
  { 0, 16, -1, -1},  // row 5
  { 1, 15, -1, -1},  // row 6
  { 1, 15, -1, -1},  // row 7
  { 2, 14, -1, -1},  // row 8
  { 3, 13, -1, -1},  // row 9
  { 4, 12, -1, -1},  // row 10
  { 5, 11, -1, -1},  // row 11
  { 6, 10, -1, -1},  // row 12
  { 7,  9, -1, -1},  // row 13
  { 8,  8, -1, -1},  // row 14: bottom point
};

static bool s_heart_outline[HEART_GRID_H][HEART_GRID_W];
static bool s_heart_computed = false;
static Layer *s_heart_layer;
static AppTimer *s_heart_timer;

static bool heart_shape_filled(int row, int col) {
  if (row < 0 || row >= HEART_GRID_H) return false;
  const HeartRowSpans *s = &kHeartRows[row];
  if (col >= s->l1 && col <= s->r1) return true;
  if (s->r2 >= s->l2 && col >= s->l2 && col <= s->r2) return true;
  return false;
}

// a cell is "deep interior" (and thus left uncolored) if every cell within
// HEART_OUTLINE_THICKNESS is also filled; the remaining shell is the outline
static bool heart_deep_interior(int row, int col) {
  for (int dr = -HEART_OUTLINE_THICKNESS; dr <= HEART_OUTLINE_THICKNESS; dr++) {
    for (int dc = -HEART_OUTLINE_THICKNESS; dc <= HEART_OUTLINE_THICKNESS; dc++) {
      int c = col + dc;
      if (c < 0 || c >= HEART_GRID_W || !heart_shape_filled(row + dr, c)) {
        return false;
      }
    }
  }
  return true;
}

static void heart_generate_outline(void) {
  for (int r = 0; r < HEART_GRID_H; r++) {
    for (int c = 0; c < HEART_GRID_W; c++) {
      s_heart_outline[r][c] = heart_shape_filled(r, c) && !heart_deep_interior(r, c);
    }
  }
  s_heart_computed = true;
}

// integer pixel sizes cycling through one pulse (grow then shrink); no floats
static const int8_t kHeartPulseSizes[] = {4, 5, 6, 7, 7, 6, 5, 4};
#define HEART_PULSE_STEPS (int)(sizeof(kHeartPulseSizes) / sizeof(kHeartPulseSizes[0]))
static int s_heart_pulse_step = 0;

static void heart_draw_proc(Layer *layer, GContext *ctx) {
  if (!s_heart_computed) heart_generate_outline();

  GRect bounds = layer_get_bounds(layer);
  int16_t px = kHeartPulseSizes[s_heart_pulse_step % HEART_PULSE_STEPS];
  int16_t grid_w = px * HEART_GRID_W;
  int16_t grid_h = px * HEART_GRID_H;
  int16_t ox = bounds.origin.x + (bounds.size.w - grid_w) / 2;
  int16_t oy = bounds.origin.y + (bounds.size.h - grid_h) / 2;

  // varied deep-red/maroon palette per pixel, mosaic-style like the reference
  // image; no pink/orange tones, just dark blood-red shades; B&W displays
  // have no red at all, so they fall back to solid black
  static const GColor kOutlinePalette[] = {
    GColorBulgarianRose, GColorDarkCandyAppleRed, GColorDarkCandyAppleRed,
    GColorBulgarianRose, GColorDarkCandyAppleRed, GColorBulgarianRose,
    GColorFolly,
  };
  #define HEART_OUTLINE_PALETTE_SIZE (int)(sizeof(kOutlinePalette) / sizeof(kOutlinePalette[0]))

  for (int r = 0; r < HEART_GRID_H; r++) {
    for (int c = 0; c < HEART_GRID_W; c++) {
      if (heart_shape_filled(r, c)) {
        // multiplicative hash of (r,c) for a scrambled, non-diagonal-looking
        // dither pattern rather than a simple repeating stripe
        unsigned hash = (unsigned)(r * 2654435761u) ^ (unsigned)(c * 40503u);
        GColor fill = PBL_IF_COLOR_ELSE(kOutlinePalette[hash % HEART_OUTLINE_PALETTE_SIZE], GColorBlack);
        graphics_context_set_fill_color(ctx, fill);
        graphics_fill_rect(ctx, GRect(ox + c * px, oy + r * px, px, px), 0, GCornerNone);
      }
    }
  }
}

static int s_heart_pulses_remaining = 0;
static void (*s_heart_pulse_done_cb)(void) = NULL;

static void heart_pulse_tick(void *data) {
  s_heart_pulse_step++;
  if (s_heart_pulse_step % HEART_PULSE_STEPS == 0 && s_heart_pulses_remaining > 0
      && --s_heart_pulses_remaining == 0) {
    void (*done_cb)(void) = s_heart_pulse_done_cb;
    s_heart_timer = NULL;
    s_heart_pulse_done_cb = NULL;
    if (s_heart_layer) {
      layer_mark_dirty(s_heart_layer);
    }
    if (done_cb) {
      done_cb();
    }
    return;
  }
  if (s_heart_layer) {
    layer_mark_dirty(s_heart_layer);
  }
  s_heart_timer = app_timer_register(180, heart_pulse_tick, NULL);
}

// pulses the heart layer `pulse_count` full cycles, then calls done_cb (may
// be NULL for an indefinite pulse the caller stops manually via heart_pulse_stop)
static void heart_pulse_start(int pulse_count, void (*done_cb)(void)) {
  s_heart_pulse_step = 0;
  s_heart_pulses_remaining = pulse_count;
  s_heart_pulse_done_cb = done_cb;
  if (!s_heart_timer) {
    s_heart_timer = app_timer_register(180, heart_pulse_tick, NULL);
  }
}

static void heart_pulse_stop(void) {
  if (s_heart_timer) {
    app_timer_cancel(s_heart_timer);
    s_heart_timer = NULL;
  }
  s_heart_pulse_done_cb = NULL;
}

// ---------- celebration window (own screen, auto-dismisses) ----------
#define CELEBRATION_PULSE_COUNT 3

static Window *s_celebration_window;
static TextLayer *s_celebration_status_layer;

// ends the just-finished alarm session's bookkeeping: stop timers/sound/HR
// sampling and line up the alarm's next occurrence. Called once, right when
// the sustain target is hit, before handing off to the celebration screen.
static void finish_alarm_session(void) {
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
}

static void celebration_finish(void) {
  heart_pulse_stop();
  window_stack_pop(true);
}

static void celebration_click_handler(ClickRecognizerRef recognizer, void *context) {
  celebration_finish();  // let the user skip the wait
}

static void celebration_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, celebration_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, celebration_click_handler);
}

static void celebration_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int16_t h = bounds.size.h;

  window_set_background_color(window, GColorWhite);

  s_celebration_status_layer = text_layer_create(GRect(0, h * 8 / 100, bounds.size.w, h * 16 / 100));
  text_layer_set_background_color(s_celebration_status_layer, GColorClear);
  text_layer_set_text_color(s_celebration_status_layer, GColorBlack);
  text_layer_set_font(s_celebration_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_celebration_status_layer, GTextAlignmentCenter);
  text_layer_set_text(s_celebration_status_layer, "Great job!");
  layer_add_child(window_layer, text_layer_get_layer(s_celebration_status_layer));

  s_heart_layer = layer_create(GRect(0, h * 26 / 100, bounds.size.w, h * 60 / 100));
  layer_set_update_proc(s_heart_layer, heart_draw_proc);
  layer_add_child(window_layer, s_heart_layer);

  heart_pulse_start(CELEBRATION_PULSE_COUNT, celebration_finish);
}

static void celebration_window_unload(Window *window) {
  heart_pulse_stop();
  layer_destroy(s_heart_layer);
  text_layer_destroy(s_celebration_status_layer);
}

static void enter_celebration_mode(void) {
  s_celebration_window = window_create();
  window_set_click_config_provider(s_celebration_window, celebration_click_config_provider);
  window_set_window_handlers(s_celebration_window, (WindowHandlers) {
    .load = celebration_window_load,
    .unload = celebration_window_unload,
  });
  window_stack_push(s_celebration_window, true);
}

// ---------- alarm window ----------
static void start_grace_vibe(void) {
  // near-continuous hard buzzing to actually wake the wearer, matching the
  // intensity of the main HR alarm vibe rather than a few gentle taps
  static const uint32_t segments[] = {700, 50, 700, 50, 700, 50, 700, 50};
  VibePattern pattern = {
    .durations = segments,
    .num_segments = ARRAY_LENGTH(segments),
  };
  vibes_enqueue_custom_pattern(pattern);
}

static void start_success_vibe(void) {
  // short celebratory triple-tap, distinct from the urgent alarm buzz
  static const uint32_t segments[] = {100, 80, 100, 80, 200};
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

  if (s_phase == AlarmPhaseGrace) {
    bg = PBL_IF_COLOR_ELSE(GColorOrange, GColorWhite);
    fg = GColorBlack;
    s_progress_color = GColorBlack;
    s_progress_fraction = (float)s_grace_elapsed_secs / s_grace_secs;
  } else if (s_phase == AlarmPhaseCalibrating) {
    bg = PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite);
    fg = GColorBlack;
    s_progress_color = GColorBlack;
    // no fixed duration any more - this phase ends purely on a clean HR
    // sample, so just pulse the bar to show activity rather than progress
    s_progress_fraction = (s_calibration_elapsed_secs % 2 == 0) ? 0.3f : 0.7f;
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
  static char progress_buf[32];

  if (s_phase != AlarmPhaseActive) {
    // not actively monitoring HR yet (still in grace/calibration): don't
    // show a counter at all, not even a placeholder
    bpm_buf[0] = '\0';
  } else if (bpm <= 0) {
    // actively monitoring, but no fresh sample yet
    snprintf(bpm_buf, sizeof(bpm_buf), "-- BPM");
  } else {
    snprintf(bpm_buf, sizeof(bpm_buf), "%d BPM", bpm);
  }
  text_layer_set_text(s_alarm_bpm_layer, bpm_buf);

  if (s_phase == AlarmPhaseGrace) {
    snprintf(status_buf, sizeof(status_buf), "WAKE UP");
    snprintf(progress_buf, sizeof(progress_buf), "%d/%d seconds", s_grace_elapsed_secs, s_grace_secs);
  } else if (s_phase == AlarmPhaseCalibrating) {
    snprintf(status_buf, sizeof(status_buf), "Calibrating...");
    snprintf(progress_buf, sizeof(progress_buf), "waiting for clean reading");
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
    if (s_grace_elapsed_secs >= s_grace_secs) {
      s_phase = AlarmPhaseCalibrating;
      s_calibration_elapsed_secs = 0;
    }
    update_alarm_ui(bpm);
    s_alarm_timer = app_timer_register(TICK_MS, alarm_tick, NULL);
    return;
  }

  if (s_phase == AlarmPhaseCalibrating) {
    s_calibration_elapsed_secs++;
    // gated purely on a confirmed-fresh HR sample: peek_current_value can
    // keep returning a stale pre-alarm reading for an unpredictable amount
    // of time, so there's no fixed delay that reliably guarantees a real one
    if (s_fresh_hr_events >= 1) {
      s_phase = AlarmPhaseActive;
    }
  }

  bool calibrating = s_phase != AlarmPhaseActive;

  if (!calibrating) {
    if (bpm >= s_target_bpm) {
      s_sustained_seconds++;
      if (s_sustained_seconds >= s_sustain_secs) {
        // sustain target hit: finish this alarm session and hand off to the
        // celebration screen, rather than lingering here
        start_success_vibe();
        finish_alarm_session();
        window_stack_pop(true);
        enter_celebration_mode();
        return;
      }
    } else {
      s_sustained_seconds = 0;
    }
  }
  s_elapsed_seconds++;

  // sound + vibration only run while HR is below the target threshold.
  // During calibration the reading may still be the stale pre-alarm value,
  // so stay silent until it's had time to settle.
  bool below_threshold = !calibrating && bpm < s_target_bpm;
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
  // this screen is never manually dismissible: reaching the sustain target
  // automatically advances to the celebration screen instead
}

static void alarm_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  // swallow BACK so it can't fall through to the default window-pop
  // behavior (that gap previously left a dangling timer and crashed)
}

static void alarm_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, alarm_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, alarm_back_click_handler);
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
  s_phase = AlarmPhaseGrace;
  s_grace_elapsed_secs = 0;
  s_calibration_elapsed_secs = 0;
  update_alarm_ui(0);

  s_alarm_timer = app_timer_register(TICK_MS, alarm_tick, NULL);
}

static void alarm_window_unload(Window *window) {
  // defensive cleanup in case this window is ever torn down through a path
  // other than the finish_alarm_session() call in alarm_tick (e.g. the OS
  // force-closing the app) - finish_alarm_session() already made these calls
  // in the normal path, so these are idempotent no-ops there
  if (s_alarm_timer) {
    app_timer_cancel(s_alarm_timer);
    s_alarm_timer = NULL;
  }
  vibes_cancel();
  speaker_stop();
  s_alarm_active = false;
  health_service_set_heart_rate_sample_period(0);
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
  static char field_text[FieldCount][40];

  snprintf(field_text[FieldTargetBpm], sizeof(field_text[FieldTargetBpm]),
           "%sTarget: %d bpm", s_active_field == FieldTargetBpm ? "> " : "", s_target_bpm);
  snprintf(field_text[FieldSustainSecs], sizeof(field_text[FieldSustainSecs]),
           "%sSustain: %d seconds", s_active_field == FieldSustainSecs ? "> " : "", s_sustain_secs);
  snprintf(field_text[FieldGraceSecs], sizeof(field_text[FieldGraceSecs]),
           "%sGrace: %d seconds", s_active_field == FieldGraceSecs ? "> " : "", s_grace_secs);

  for (int i = 0; i < FieldCount; i++) {
    bool active = (i == s_active_field);
    text_layer_set_text(s_settings_field_layers[i], field_text[i]);
    text_layer_set_font(s_settings_field_layers[i], fonts_get_system_font(
        active ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_24));
    text_layer_set_text_color(s_settings_field_layers[i], active
        ? PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
        : GColorFromRGB(64, 64, 64));
  }

  static char hint[80];
  snprintf(hint, sizeof(hint), "UP/DOWN: change\nSELECT: next field\nhold SELECT: save & exit");
  text_layer_set_text(s_settings_hint_layer, hint);
}

static void settings_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_active_field) {
    case FieldTargetBpm: s_target_bpm += 5; break;
    case FieldSustainSecs: s_sustain_secs += 5; break;
    default: s_grace_secs += 5; break;
  }
  save_settings();
  settings_update_text();
}

static void settings_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_active_field) {
    case FieldTargetBpm: if (s_target_bpm > 40) s_target_bpm -= 5; break;
    case FieldSustainSecs: if (s_sustain_secs > 5) s_sustain_secs -= 5; break;
    default: if (s_grace_secs > 5) s_grace_secs -= 5; break;
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

  window_set_background_color(window, GColorWhite);

  for (int i = 0; i < FieldCount; i++) {
    s_settings_field_layers[i] = text_layer_create(
        GRect(6, h * (6 + i * 16) / 100, bounds.size.w - 12, h * 16 / 100));
    text_layer_set_background_color(s_settings_field_layers[i], GColorClear);
    text_layer_set_text_alignment(s_settings_field_layers[i], GTextAlignmentLeft);
    layer_add_child(window_layer, text_layer_get_layer(s_settings_field_layers[i]));
  }

  s_settings_hint_layer = text_layer_create(GRect(0, h * 72 / 100, bounds.size.w, h * 28 / 100));
  text_layer_set_background_color(s_settings_hint_layer, GColorClear);
  text_layer_set_text_color(s_settings_hint_layer, GColorFromRGB(64, 64, 64));
  text_layer_set_font(s_settings_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_settings_hint_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_settings_hint_layer));

  settings_update_text();
}

static void settings_window_unload(Window *window) {
  for (int i = 0; i < FieldCount; i++) {
    text_layer_destroy(s_settings_field_layers[i]);
  }
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
static const char *const kEditFieldLabels[EFCount] = {
  "Enabled", "Hour", "Minute", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static void edit_field_text(int i, char *out, size_t out_len) {
  Alarm *a = &s_alarms[s_editing_alarm_index];
  const char *prefix = (i == s_edit_field) ? "> " : "";
  if (i == EFHour) {
    snprintf(out, out_len, "%s%s: %02d", prefix, kEditFieldLabels[i], a->hour);
  } else if (i == EFMinute) {
    snprintf(out, out_len, "%s%s: %02d", prefix, kEditFieldLabels[i], a->minute);
  } else if (i == EFEnabled) {
    snprintf(out, out_len, "%s%s: %s", prefix, kEditFieldLabels[i], a->enabled ? "On" : "Off");
  } else {
    int day = i - EFSun;
    snprintf(out, out_len, "%s%s: %s", prefix, kEditFieldLabels[i],
             (a->days_mask & (1 << day)) ? "On" : "Off");
  }
}

static void edit_update_text(void) {
  static char field_text[EDIT_VISIBLE_ROWS][32];

  for (int row = 0; row < EDIT_VISIBLE_ROWS; row++) {
    int i = s_edit_scroll + row;
    bool active = (i == s_edit_field);
    edit_field_text(i, field_text[row], sizeof(field_text[row]));
    text_layer_set_text(s_edit_field_layers[row], field_text[row]);
    text_layer_set_font(s_edit_field_layers[row], fonts_get_system_font(
        active ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_24));
    text_layer_set_text_color(s_edit_field_layers[row], active
        ? PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
        : GColorFromRGB(64, 64, 64));
  }

  text_layer_set_text(s_edit_hint_layer, "UP/DOWN: change\nSELECT: next field\nhold SELECT: save & exit");
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
  if (s_edit_field == 0) {
    s_edit_scroll = 0;  // wrapped back to the first field: scroll back to top
  } else if (s_edit_field >= s_edit_scroll + EDIT_VISIBLE_ROWS) {
    s_edit_scroll = s_edit_field - EDIT_VISIBLE_ROWS + 1;
  }
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

  window_set_background_color(window, GColorWhite);

  for (int i = 0; i < EDIT_VISIBLE_ROWS; i++) {
    s_edit_field_layers[i] = text_layer_create(
        GRect(6, h * (6 + i * 16) / 100, bounds.size.w - 12, h * 16 / 100));
    text_layer_set_background_color(s_edit_field_layers[i], GColorClear);
    text_layer_set_text_alignment(s_edit_field_layers[i], GTextAlignmentLeft);
    layer_add_child(window_layer, text_layer_get_layer(s_edit_field_layers[i]));
  }

  s_edit_hint_layer = text_layer_create(GRect(0, h * 72 / 100, bounds.size.w, h * 28 / 100));
  text_layer_set_background_color(s_edit_hint_layer, GColorClear);
  text_layer_set_text_color(s_edit_hint_layer, GColorFromRGB(64, 64, 64));
  text_layer_set_font(s_edit_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_edit_hint_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_edit_hint_layer));

  s_edit_field = EFEnabled;
  s_edit_scroll = 0;
  edit_update_text();
}

static void edit_window_unload(Window *window) {
  for (int i = 0; i < EDIT_VISIBLE_ROWS; i++) {
    text_layer_destroy(s_edit_field_layers[i]);
  }
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

static void list_row_text(int i, char *out, size_t out_len) {
  const char *prefix = (s_list_cursor == i) ? "> " : "";
  if (i == 0) {
    snprintf(out, out_len, "%sHR settings", prefix);
    return;
  }
  Alarm *a = &s_alarms[i - 1];
  if (a->enabled) {
    char days_buf[32];
    format_days_summary(a->days_mask, days_buf, sizeof(days_buf));
    snprintf(out, out_len, "%s%02d:%02d %s", prefix, a->hour, a->minute, days_buf);
  } else {
    snprintf(out, out_len, "%sAlarm off", prefix);
  }
}

static void list_update_text(void) {
  static char row_text[LIST_WINDOW_ROWS][48];

  for (int row = 0; row < LIST_WINDOW_ROWS; row++) {
    int i = s_list_scroll + row;
    bool active = (i == s_list_cursor);
    list_row_text(i, row_text[row], sizeof(row_text[row]));
    text_layer_set_text(s_list_row_layers[row], row_text[row]);
    text_layer_set_font(s_list_row_layers[row], fonts_get_system_font(
        active ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_24));
    text_layer_set_text_color(s_list_row_layers[row], active
        ? PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
        : GColorFromRGB(64, 64, 64));
  }
}

// keeps the cursor's row inside the visible window, scrolling as needed;
// works for movement in either direction, including wraparound
static void list_scroll_to_cursor(void) {
  if (s_list_cursor < s_list_scroll) {
    s_list_scroll = s_list_cursor;
  } else if (s_list_cursor >= s_list_scroll + LIST_WINDOW_ROWS) {
    s_list_scroll = s_list_cursor - LIST_WINDOW_ROWS + 1;
  }
}

static void list_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_list_cursor = (s_list_cursor + MAX_ALARMS) % LIST_TOTAL_ROWS;
  list_scroll_to_cursor();
  list_update_text();
}

static void list_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_list_cursor = (s_list_cursor + 1) % LIST_TOTAL_ROWS;
  list_scroll_to_cursor();
  list_update_text();
}

static void list_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_list_cursor == 0) {
    enter_settings_screen();
  } else {
    enter_edit_screen(s_list_cursor - 1);
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

static void list_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int16_t h = bounds.size.h;

  window_set_background_color(window, GColorWhite);

  s_list_title_layer = text_layer_create(GRect(0, h * 2 / 100, bounds.size.w, h * 14 / 100));
  text_layer_set_background_color(s_list_title_layer, GColorClear);
  text_layer_set_text_color(s_list_title_layer, GColorBlack);
  text_layer_set_font(s_list_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_list_title_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_list_title_layer));
  list_update_title();

  for (int i = 0; i < LIST_WINDOW_ROWS; i++) {
    s_list_row_layers[i] = text_layer_create(
        GRect(6, h * (16 + i * 16) / 100, bounds.size.w - 12, h * 16 / 100));
    text_layer_set_background_color(s_list_row_layers[i], GColorClear);
    text_layer_set_text_alignment(s_list_row_layers[i], GTextAlignmentLeft);
    layer_add_child(window_layer, text_layer_get_layer(s_list_row_layers[i]));
  }

  s_list_hint_layer = text_layer_create(GRect(0, h * 72 / 100, bounds.size.w, h * 28 / 100));
  text_layer_set_background_color(s_list_hint_layer, GColorClear);
  text_layer_set_text_color(s_list_hint_layer, GColorFromRGB(64, 64, 64));
  text_layer_set_font(s_list_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_list_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_list_hint_layer, "UP/DOWN: move cursor\nSELECT: edit\nhold SELECT: test now");
  layer_add_child(window_layer, text_layer_get_layer(s_list_hint_layer));

  list_update_text();
}

static void list_window_unload(Window *window) {
  text_layer_destroy(s_list_title_layer);
  for (int i = 0; i < LIST_WINDOW_ROWS; i++) {
    text_layer_destroy(s_list_row_layers[i]);
  }
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
