#include <pebble.h>

// --- config ---
#define DEFAULT_TARGET_BPM 100
#define DEFAULT_SUSTAIN_SECS 10
#define DEFAULT_GRACE_SECS 20
#define GRACE_BUZZ_INTERVAL_SECS 4  // buzz every N seconds during grace, not every tick
#define CALIBRATION_MAX_SECS 90  // safeguard cap if the watch never gets a fresh HR sample (e.g. not worn)
#define HR_SAMPLE_PERIOD_SEC 1
#define TICK_MS 1000
#define ALARM_VOLUME 100
#define MAX_ALARMS 20  // generous ceiling for the persisted array, not a user-facing limit

typedef enum {
  AlarmPhaseGrace,        // a few buzzes to rouse the wearer before we start reading HR
  AlarmPhaseCalibrating,  // waiting for a fresh HR sample + minimum settle time
  AlarmPhaseActive,       // normal threshold/sustain logic
} AlarmPhase;

// Repeat: fires every selected weekday, forever. Once: fires next, then
// disables itself (paused, not removed). Temporary: fires next, then is
// deleted outright. None of this applies to a manual "Test now" run.
typedef enum { AlarmRepeat, AlarmOnce, AlarmTemporary } AlarmType;

// one persisted alarm slot. days_mask bit i corresponds to struct tm's
// tm_wday (0=Sunday .. 6=Saturday); at least one bit is always set (enforced
// at edit time - an alarm with no days selected can never fire). HR
// target/sustain/grace are per-alarm, not global, so each alarm can have its
// own wake-up-difficulty profile.
typedef struct {
  bool enabled;
  uint8_t hour;
  uint8_t minute;
  uint8_t days_mask;
  int32_t wakeup_id;  // -1 if nothing currently scheduled
  int16_t target_bpm;
  int16_t sustain_secs;
  int16_t grace_secs;
  int8_t alarm_type;  // AlarmType
} Alarm;

static Alarm s_alarms[MAX_ALARMS];
static int s_alarm_count = 0;  // number of alarms actually in use (s_alarms[0..count-1])
static int s_active_alarm_index = 0;  // which alarm is running the current alarm session
// true when the current alarm session was started via "Test now" rather than
// a real trigger (wakeup or resumed session) - once/temporary side effects
// and any other "real trigger only" behavior must check this first
static bool s_alarm_is_test = false;

#define LIST_WINDOW_ROWS 3

static Window *s_list_window;
static TextLayer *s_list_title_layer;
static TextLayer *s_list_row_layers[LIST_WINDOW_ROWS];
static TextLayer *s_list_type_layers[LIST_WINDOW_ROWS];
static Layer *s_list_day_layers[LIST_WINDOW_ROWS];
// column widths/geometry computed once in list_window_load, reused in
// list_update_text to widen row 0's text layer to the full row width (its
// "+ Add alarm" label has nowhere near a real alarm's other two columns)
static int16_t s_list_row_w, s_list_time_w;
static TextLayer *s_list_hint_layer;
static int s_list_cursor = 0;  // 0 = "+ Add alarm" row, 1..count = alarm rows
static int s_list_scroll = 0;  // index of the item shown in the topmost visible row

// EFEnabled/day fields are toggles; EFHour/EFMinute/EFTargetBpm/EFSustainSecs/
// EFGraceSecs are numeric (SELECT enters adjust mode, see s_edit_adjusting);
// EFTestNow/EFDelete are one-shot actions.
typedef enum {
  EFEnabled, EFAlarmType, EFHour, EFMinute, EFTargetBpm, EFSustainSecs, EFGraceSecs,
  EFSun, EFMon, EFTue, EFWed, EFThu, EFFri, EFSat, EFTestNow, EFDelete, EFCount,
} EditField;

#define EDIT_VISIBLE_ROWS 4

static Window *s_edit_window;
static TextLayer *s_edit_field_layers[EDIT_VISIBLE_ROWS];
static TextLayer *s_edit_hint_layer;
static int s_editing_alarm_index = -1;
static EditField s_edit_field = EFEnabled;
static int s_edit_scroll = 0;  // index of the field shown in the topmost visible row
// UP/DOWN always move the cursor between rows, EXCEPT while adjusting a
// numeric field's value (entered/exited via SELECT) - one consistent model
// instead of mixing "SELECT tabs / UP+DOWN adjusts" with action rows
static bool s_edit_adjusting = false;

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
  PKEY_ALARM_COUNT = 100,
  PKEY_ALARMS = 110,
  PKEY_SESSION_ACTIVE = 111,
  PKEY_SESSION_ALARM_IDX = 112,
  PKEY_SESSION_IS_TEST = 113,
};

// long-pressing BACK is a system-level "quit app" gesture that no watchapp
// can intercept, so instead of trying to block the escape, we make it
// pointless: persist that an alarm session is in progress, and on the next
// launch (from the launcher, not just a wakeup event) resume straight back
// into the locked alarm screen rather than the main list
static void persist_session_active(int alarm_index, bool is_test) {
  persist_write_int(PKEY_SESSION_ACTIVE, 1);
  persist_write_int(PKEY_SESSION_ALARM_IDX, alarm_index);
  persist_write_int(PKEY_SESSION_IS_TEST, is_test ? 1 : 0);
  // the worker polls this same flag and force-relaunches the app onto the
  // locked alarm screen if it ever gets quit (e.g. the unblockable long-BACK
  // gesture) - vibration would otherwise die the instant the app closes
  app_worker_launch();
}

static void persist_session_cleared(void) {
  persist_write_int(PKEY_SESSION_ACTIVE, 0);
  if (app_worker_is_running()) {
    app_worker_kill();
  }
}

static void load_settings(void) {
  if (!persist_exists(PKEY_ALARM_COUNT)) {
    s_alarm_count = 0;
    return;
  }
  s_alarm_count = persist_read_int(PKEY_ALARM_COUNT);
  if (s_alarm_count < 0) s_alarm_count = 0;
  if (s_alarm_count > MAX_ALARMS) s_alarm_count = MAX_ALARMS;
  if (s_alarm_count > 0 && persist_exists(PKEY_ALARMS)) {
    persist_read_data(PKEY_ALARMS, s_alarms, s_alarm_count * sizeof(Alarm));
  }
}

static void save_alarms(void) {
  persist_write_int(PKEY_ALARM_COUNT, s_alarm_count);
  if (s_alarm_count > 0) {
    persist_write_data(PKEY_ALARMS, s_alarms, s_alarm_count * sizeof(Alarm));
  }
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
  for (int i = 0; i < s_alarm_count; i++) {
    Alarm *a = &s_alarms[i];
    if (a->enabled && (a->wakeup_id < 0 || !wakeup_query(a->wakeup_id, NULL))) {
      schedule_alarm_index(i);
    }
  }
}

// removes alarm idx from the array outright, cancels its pending wakeup, and
// re-schedules every alarm that shifted down a slot (wakeup_schedule() bakes
// the array index in as its "reason", so a shift leaves other alarms'
// already-scheduled wakeups pointing at a stale index otherwise)
static void delete_alarm_at(int idx) {
  Alarm *a = &s_alarms[idx];
  if (a->wakeup_id >= 0 && wakeup_query(a->wakeup_id, NULL)) {
    wakeup_cancel(a->wakeup_id);
  }
  for (int i = idx; i < s_alarm_count - 1; i++) {
    s_alarms[i] = s_alarms[i + 1];
  }
  s_alarm_count--;
  save_alarms();
  for (int i = idx; i < s_alarm_count; i++) {
    schedule_alarm_index(i);
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
  s_heart_timer = app_timer_register(80, heart_pulse_tick, NULL);
}

// pulses the heart layer `pulse_count` full cycles, then calls done_cb (may
// be NULL for an indefinite pulse the caller stops manually via heart_pulse_stop)
static void heart_pulse_start(int pulse_count, void (*done_cb)(void)) {
  s_heart_pulse_step = 0;
  s_heart_pulses_remaining = pulse_count;
  s_heart_pulse_done_cb = done_cb;
  if (!s_heart_timer) {
    s_heart_timer = app_timer_register(80, heart_pulse_tick, NULL);
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
#define CELEBRATION_PULSE_COUNT 5

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
  persist_session_cleared();

  // once/temporary side effects only apply to a real trigger, never to a
  // manual "Test now" run
  if (!s_alarm_is_test) {
    Alarm *a = &s_alarms[s_active_alarm_index];
    if (a->alarm_type == AlarmTemporary) {
      delete_alarm_at(s_active_alarm_index);
      return;
    } else if (a->alarm_type == AlarmOnce) {
      a->enabled = false;
      save_alarms();
      schedule_alarm_index(s_active_alarm_index);  // cancels the wakeup since now disabled
      return;
    }
  }
  schedule_alarm_index(s_active_alarm_index);
}

static void celebration_finish(void) {
  heart_pulse_stop();
  light_enable(false);  // screen stays lit through the celebration too, only now released
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
      // the grace buzz pattern can still be mid-playback when this phase
      // ends; cut it off immediately instead of letting it bleed into
      // calibrating
      vibes_cancel();
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
    // of time, so there's no fixed delay that reliably guarantees a real one.
    // CALIBRATION_MAX_SECS is just a safeguard against never getting one at
    // all (e.g. the watch isn't being worn) - not a configurable setting.
    if (s_fresh_hr_events >= 1 || s_calibration_elapsed_secs >= CALIBRATION_MAX_SECS) {
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
  // holding BACK is a system-level "quit app" gesture that bypasses normal
  // click handling entirely unless a long-click handler for BACK is
  // subscribed to override it; without this, a held BACK could still escape
  // the alarm even with the single-click handler above swallowing short presses
  window_long_click_subscribe(BUTTON_ID_BACK, 0, alarm_back_click_handler, NULL);
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

  // snapshot this specific alarm's HR settings for the duration of the
  // session - s_target_bpm/s_sustain_secs/s_grace_secs below are a
  // session-scoped cache, not global settings
  Alarm *active = &s_alarms[s_active_alarm_index];
  s_target_bpm = active->target_bpm;
  s_sustain_secs = active->sustain_secs;
  s_grace_secs = active->grace_secs;

  vibes_cancel();
  speaker_stop();
  light_enable(true);  // keep the screen lit for the whole alarm phase
  health_service_set_heart_rate_sample_period(HR_SAMPLE_PERIOD_SEC);
  s_sustained_seconds = 0;
  s_elapsed_seconds = 0;
  s_fresh_hr_events = 0;
  s_alarm_active = true;
  persist_session_active(s_active_alarm_index, s_alarm_is_test);
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
  // deliberately NOT calling light_enable(false) or clearing the persisted
  // session flag here: this handler also runs on the normal success path
  // (window popped to enter the celebration screen), where the backlight
  // needs to stay on (celebration_finish() releases it) and the resume flag
  // must survive (it also runs during an app quit via the unblockable
  // long-BACK gesture). Both are only cleared on their true end-of-session
  // paths: celebration_finish() and finish_alarm_session() respectively.
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

// ---------- per-alarm edit window (fields + inline HR settings + actions) ----------
static const char *const kEditFieldLabels[EFCount] = {
  "Enabled", "Type", "Hour", "Minute", "Target", "Sustain", "Grace",
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
  "Test now", "Delete alarm",
};

static const char *const kAlarmTypeLabels[3] = { "Repeat", "Once", "Temporary" };

static bool edit_field_is_numeric(EditField f) {
  return f == EFHour || f == EFMinute || f == EFTargetBpm || f == EFSustainSecs || f == EFGraceSecs;
}

static void edit_field_text(int i, char *out, size_t out_len) {
  Alarm *a = &s_alarms[s_editing_alarm_index];
  const char *prefix = (i == s_edit_field) ? "> " : "";
  if (i == EFHour) {
    snprintf(out, out_len, "%s%s: %02d", prefix, kEditFieldLabels[i], a->hour);
  } else if (i == EFMinute) {
    snprintf(out, out_len, "%s%s: %02d", prefix, kEditFieldLabels[i], a->minute);
  } else if (i == EFTargetBpm) {
    snprintf(out, out_len, "%s%s: %d bpm", prefix, kEditFieldLabels[i], a->target_bpm);
  } else if (i == EFSustainSecs) {
    snprintf(out, out_len, "%s%s: %d sec", prefix, kEditFieldLabels[i], a->sustain_secs);
  } else if (i == EFGraceSecs) {
    snprintf(out, out_len, "%s%s: %d sec", prefix, kEditFieldLabels[i], a->grace_secs);
  } else if (i == EFAlarmType) {
    snprintf(out, out_len, "%s%s: %s", prefix, kEditFieldLabels[i], kAlarmTypeLabels[a->alarm_type]);
  } else if (i == EFEnabled) {
    snprintf(out, out_len, "%s%s: %s", prefix, kEditFieldLabels[i], a->enabled ? "On" : "Off");
  } else if (i >= EFSun && i <= EFSat) {
    int day = i - EFSun;
    snprintf(out, out_len, "%s%s: %s", prefix, kEditFieldLabels[i],
             (a->days_mask & (1 << day)) ? "On" : "Off");
  } else {
    // action rows (Test now/Delete alarm): just the label, no value
    snprintf(out, out_len, "%s%s", prefix, kEditFieldLabels[i]);
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

  if (s_edit_adjusting) {
    text_layer_set_text(s_edit_hint_layer, "UP/DOWN: change value\nSELECT: done");
  } else {
    text_layer_set_text(s_edit_hint_layer, "UP/DOWN: move\nSELECT: choose\nhold SELECT: save & exit");
  }
}

static void edit_apply_and_reschedule(void) {
  save_alarms();
  schedule_alarm_index(s_editing_alarm_index);
  edit_update_text();
}

// UP/DOWN always move the cursor between rows, except while s_edit_adjusting
// is set (a numeric field's value is being changed instead)
static void edit_move_cursor(int delta) {
  s_edit_field = (EditField)(((int)s_edit_field + delta + EFCount) % EFCount);
  if (s_edit_field < s_edit_scroll) {
    s_edit_scroll = s_edit_field;
  } else if (s_edit_field >= s_edit_scroll + EDIT_VISIBLE_ROWS) {
    s_edit_scroll = s_edit_field - EDIT_VISIBLE_ROWS + 1;
  }
  edit_update_text();
}

static void edit_adjust_value(int delta) {
  Alarm *a = &s_alarms[s_editing_alarm_index];
  switch (s_edit_field) {
    case EFHour: a->hour = (a->hour + 24 + delta) % 24; break;
    case EFMinute: a->minute = (a->minute + 60 + delta) % 60; break;
    case EFTargetBpm:
      a->target_bpm += delta * 5;
      if (a->target_bpm < 40) a->target_bpm = 40;
      if (a->target_bpm > 220) a->target_bpm = 220;  // well past any real max heart rate
      break;
    case EFSustainSecs:
      a->sustain_secs += delta * 5;
      if (a->sustain_secs < 5) a->sustain_secs = 5;
      break;
    case EFGraceSecs:
      a->grace_secs += delta * 5;
      if (a->grace_secs < 5) a->grace_secs = 5;
      break;
    default: return;
  }
  edit_apply_and_reschedule();
}

static void edit_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_edit_adjusting) {
    edit_adjust_value(1);
  } else {
    edit_move_cursor(-1);
  }
}

static void edit_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_edit_adjusting) {
    edit_adjust_value(-1);
  } else {
    edit_move_cursor(1);
  }
}

// removes the alarm being edited from the list entirely (not just disables
// it), cancels any pending wakeup for it, and returns to the alarm list
static void delete_current_alarm(void) {
  delete_alarm_at(s_editing_alarm_index);
  if (s_list_cursor > s_alarm_count) {
    s_list_cursor = s_alarm_count;
  }
  window_stack_pop(true);
}

static void edit_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_edit_adjusting) {
    s_edit_adjusting = false;  // commit and return to navigation
    edit_update_text();
    return;
  }
  if (s_edit_field == EFTestNow) {
    s_active_alarm_index = s_editing_alarm_index;
    s_alarm_is_test = true;
    enter_alarm_mode();
    return;
  }
  if (s_edit_field == EFDelete) {
    delete_current_alarm();
    return;
  }
  if (s_edit_field == EFAlarmType) {
    Alarm *a = &s_alarms[s_editing_alarm_index];
    a->alarm_type = (a->alarm_type + 1) % 3;
    edit_apply_and_reschedule();
    return;
  }
  if (edit_field_is_numeric(s_edit_field)) {
    s_edit_adjusting = true;  // UP/DOWN now change this field's value instead of moving the cursor
    edit_update_text();
    return;
  }
  // toggle fields (enabled/days): a plain SELECT flips it right away, no
  // separate adjust mode needed for a binary value
  Alarm *a = &s_alarms[s_editing_alarm_index];
  if (s_edit_field == EFEnabled) {
    a->enabled = !a->enabled;
  } else {
    int day = s_edit_field - EFSun;
    bool day_on = a->days_mask & (1 << day);
    int days_selected = 0;
    for (int d = 0; d < 7; d++) {
      if (a->days_mask & (1 << d)) days_selected++;
    }
    // at least one day must always stay selected, otherwise the alarm could
    // never fire
    if (!(day_on && days_selected <= 1)) {
      a->days_mask ^= (1 << day);
    }
  }
  edit_apply_and_reschedule();
}

static void edit_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  // long-press SELECT: done editing this alarm, save and go back to the alarm list
  save_alarms();
  schedule_alarm_index(s_editing_alarm_index);
  window_stack_pop(true);
}

static void edit_click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, edit_up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, edit_down_click_handler);
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
  s_edit_adjusting = false;
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
  // the cursor row is already indicated by bold+accent color (see
  // list_update_text), so no "> " text prefix is needed here - that also
  // frees up column width for the type indicator below
  if (i == 0) {
    snprintf(out, out_len, "+ Add alarm");
    return;
  }
  if (i > s_alarm_count) {
    out[0] = '\0';  // beyond the real list (visible-window padding row): blank
    return;
  }
  Alarm *a = &s_alarms[i - 1];
  if (a->enabled) {
    snprintf(out, out_len, "%02d:%02d", a->hour, a->minute);
  } else {
    snprintf(out, out_len, "Alarm off");
  }
}

// draws the day-of-week grid for one alarm row: each of the 7 day letters
// gets its own evenly-spaced cell (real spacing, not just adjacent glyphs),
// and a selected day is bold + the accent color while an unselected one is
// plain + dim - a real visual on/off distinction, not just letter case
static void list_day_grid_draw_proc(Layer *layer, GContext *ctx) {
  int row = *(int *)layer_get_data(layer);
  int i = s_list_scroll + row;
  if (i == 0 || i > s_alarm_count) return;

  Alarm *a = &s_alarms[i - 1];
  if (!a->enabled) return;

  GRect bounds = layer_get_bounds(layer);
  GFont font_on = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont font_off = fonts_get_system_font(FONT_KEY_GOTHIC_24);
  GColor on_color = PBL_IF_COLOR_ELSE(GColorRed, GColorBlack);
  GColor off_color = GColorFromRGB(64, 64, 64);

  // always show the letter grid - a selected day is bold+accent, an
  // unselected one plain+dim, regardless of how many days are picked
  static const char *const letters[7] = { "S", "M", "T", "W", "T", "F", "S" };
  int16_t pitch = bounds.size.w / 7;
  for (int d = 0; d < 7; d++) {
    bool on = a->days_mask & (1 << d);
    graphics_context_set_text_color(ctx, on ? on_color : off_color);
    GRect cell = GRect(bounds.origin.x + d * pitch, bounds.origin.y, pitch, bounds.size.h);
    graphics_draw_text(ctx, letters[d], on ? font_on : font_off, cell,
        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

// repeat/once/temporary indicator text for one row - kept as its own small
// TextLayer (not part of the day-letter grid) so it reads as a distinct
// piece of info rather than an 8th day
static void list_type_text(int i, char *out, size_t out_len) {
  if (i == 0 || i > s_alarm_count) {
    out[0] = '\0';
    return;
  }
  Alarm *a = &s_alarms[i - 1];
  if (!a->enabled) {
    out[0] = '\0';
    return;
  }
  static const char *const type_letters[3] = { "R", "1", "T" };
  snprintf(out, out_len, "%s", type_letters[a->alarm_type]);
}

static void list_update_text(void) {
  static char row_text[LIST_WINDOW_ROWS][48];
  static char type_text[LIST_WINDOW_ROWS][4];

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

    // "+ Add alarm" has no type/day columns to share the row with - widen
    // its text layer to the full row width instead of leaving it clipped to
    // the narrow time column
    GRect frame = layer_get_frame(text_layer_get_layer(s_list_row_layers[row]));
    frame.size.w = (i == 0) ? s_list_row_w : s_list_time_w;
    layer_set_frame(text_layer_get_layer(s_list_row_layers[row]), frame);

    list_type_text(i, type_text[row], sizeof(type_text[row]));
    text_layer_set_text(s_list_type_layers[row], type_text[row]);

    layer_mark_dirty(s_list_day_layers[row]);
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

// creates a new default alarm right under the "+ Add alarm" row (index 0)
// and jumps straight into editing it
static void add_new_alarm(void) {
  if (s_alarm_count >= MAX_ALARMS) return;  // reached the ceiling, nothing to do
  for (int i = s_alarm_count; i > 0; i--) {
    s_alarms[i] = s_alarms[i - 1];
  }
  time_t now = time(NULL);
  struct tm *now_tm = localtime(&now);
  s_alarms[0] = (Alarm){
    .enabled = true, .hour = now_tm->tm_hour, .minute = now_tm->tm_min,
    .days_mask = (uint8_t)(1 << now_tm->tm_wday),  // default: today, so it fires by itself once as expected
    .wakeup_id = -1,
    .target_bpm = DEFAULT_TARGET_BPM, .sustain_secs = DEFAULT_SUSTAIN_SECS, .grace_secs = DEFAULT_GRACE_SECS,
    .alarm_type = AlarmOnce,
  };
  s_alarm_count++;
  save_alarms();
  // every existing alarm just shifted down one slot. wakeup_schedule() bakes
  // the array index in as its "reason", so a previously-scheduled wakeup now
  // has a stale index baked into it (it would wake into the WRONG alarm's
  // slot) - re-schedule everything so each one's reason matches its new spot
  for (int i = 0; i < s_alarm_count; i++) {
    schedule_alarm_index(i);
  }
  enter_edit_screen(0);
}

static void list_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_list_cursor = (s_list_cursor + s_alarm_count) % (s_alarm_count + 1);
  list_scroll_to_cursor();
  list_update_text();
}

static void list_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_list_cursor = (s_list_cursor + 1) % (s_alarm_count + 1);
  list_scroll_to_cursor();
  list_update_text();
}

static void list_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_list_cursor == 0) {
    add_new_alarm();
  } else {
    enter_edit_screen(s_list_cursor - 1);
  }
}

static void list_click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, list_up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, list_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, list_select_click_handler);
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

  int16_t row_w = bounds.size.w - 12;
  int16_t time_w = row_w * 8 / 20;   // ~40%: plenty for unprefixed "HH:MM"
  int16_t type_w = row_w * 3 / 20;   // ~15%: just enough for one GOTHIC_24 glyph - do NOT shrink
                                      // day_w below this to make more room here, the day grid
                                      // visibly breaks (mangled/overlapping letters) once its
                                      // per-letter pitch drops much below ~60px total width
  int16_t day_w = row_w - time_w - type_w;  // ~45%: keep this at its known-good width
  s_list_row_w = row_w;
  s_list_time_w = time_w;
  for (int i = 0; i < LIST_WINDOW_ROWS; i++) {
    int16_t row_y = h * (16 + i * 16) / 100;
    int16_t row_h = h * 16 / 100;

    s_list_row_layers[i] = text_layer_create(GRect(6, row_y, time_w, row_h));
    text_layer_set_background_color(s_list_row_layers[i], GColorClear);
    text_layer_set_text_alignment(s_list_row_layers[i], GTextAlignmentLeft);
    layer_add_child(window_layer, text_layer_get_layer(s_list_row_layers[i]));

    // same font size as the day grid (just not bold) so the glyph itself
    // reads clearly, but its own isolated column/layer keeps it visually a
    // distinct piece of info rather than an 8th day
    s_list_type_layers[i] = text_layer_create(GRect(6 + time_w, row_y, type_w, row_h));
    text_layer_set_background_color(s_list_type_layers[i], GColorClear);
    text_layer_set_text_color(s_list_type_layers[i], GColorFromRGB(64, 64, 64));
    text_layer_set_font(s_list_type_layers[i], fonts_get_system_font(FONT_KEY_GOTHIC_24));
    text_layer_set_text_alignment(s_list_type_layers[i], GTextAlignmentCenter);
    layer_add_child(window_layer, text_layer_get_layer(s_list_type_layers[i]));

    s_list_day_layers[i] = layer_create_with_data(
        GRect(6 + time_w + type_w, row_y, day_w, row_h), sizeof(int));
    *(int *)layer_get_data(s_list_day_layers[i]) = i;
    layer_set_update_proc(s_list_day_layers[i], list_day_grid_draw_proc);
    layer_add_child(window_layer, s_list_day_layers[i]);
  }

  s_list_hint_layer = text_layer_create(GRect(0, h * 72 / 100, bounds.size.w, h * 28 / 100));
  text_layer_set_background_color(s_list_hint_layer, GColorClear);
  text_layer_set_text_color(s_list_hint_layer, GColorFromRGB(64, 64, 64));
  text_layer_set_font(s_list_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_list_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_list_hint_layer, "UP/DOWN: move cursor\nSELECT: open");
  layer_add_child(window_layer, text_layer_get_layer(s_list_hint_layer));

  list_update_text();
}

static void list_window_unload(Window *window) {
  text_layer_destroy(s_list_title_layer);
  for (int i = 0; i < LIST_WINDOW_ROWS; i++) {
    text_layer_destroy(s_list_row_layers[i]);
    text_layer_destroy(s_list_type_layers[i]);
    layer_destroy(s_list_day_layers[i]);
  }
  text_layer_destroy(s_list_hint_layer);
}

static void list_window_appear(Window *window) {
  // refresh immediately when returning from the edit/settings screens,
  // rather than waiting for the next UP/DOWN press
  list_update_title();
  // the alarm count may have shrunk (an alarm was just deleted): keep the
  // cursor and scroll window in bounds
  if (s_list_cursor > s_alarm_count) {
    s_list_cursor = s_alarm_count;
  }
  list_scroll_to_cursor();
  list_update_text();
}

static void wakeup_handler(WakeupId id, int32_t reason) {
  s_active_alarm_index = (int)reason;
  s_alarm_is_test = false;
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
      s_alarm_is_test = false;
      enter_alarm_mode();
    }
  } else if (persist_exists(PKEY_SESSION_ACTIVE) && persist_read_int(PKEY_SESSION_ACTIVE)) {
    // an alarm session was in progress when the app last quit (e.g. via the
    // system's long-BACK quit gesture, which no app can block) - resume
    // straight back into the locked alarm screen instead of the main list
    s_active_alarm_index = persist_exists(PKEY_SESSION_ALARM_IDX)
      ? persist_read_int(PKEY_SESSION_ALARM_IDX) : -1;
    s_alarm_is_test = persist_exists(PKEY_SESSION_IS_TEST) && persist_read_int(PKEY_SESSION_IS_TEST);
    enter_alarm_mode();
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
