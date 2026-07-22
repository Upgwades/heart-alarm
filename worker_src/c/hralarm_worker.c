#include <pebble_worker.h>

// keep in sync with PKEY_SESSION_ACTIVE in src/c/hralarm.c
#define PKEY_SESSION_ACTIVE 111

// long-pressing BACK is a system-level "quit app" gesture that no watchapp
// can intercept, and once the foreground app is gone, vibration stops with
// it - a real alarm clock can't rely on the wearer being awake enough to
// reopen the app themselves. This worker runs independently of the
// foreground app for as long as an alarm session is in progress (started/
// stopped by the app itself around persist_session_active/cleared) and
// simply keeps relaunching the app onto its locked alarm screen, defeating
// the escape hatch instead of just tolerating it.
#define POLL_INTERVAL_MS 3000

static AppTimer *s_poll_timer;

static void poll_tick(void *data) {
  if (persist_exists(PKEY_SESSION_ACTIVE) && persist_read_int(PKEY_SESSION_ACTIVE)) {
    worker_launch_app();
    s_poll_timer = app_timer_register(POLL_INTERVAL_MS, poll_tick, NULL);
  }
  // if the session is no longer active, just let the timer chain stop; the
  // app kills this worker outright once the alarm is dismissed anyway
}

static void worker_init(void) {
  s_poll_timer = app_timer_register(POLL_INTERVAL_MS, poll_tick, NULL);
}

static void worker_deinit(void) {
  if (s_poll_timer) {
    app_timer_cancel(s_poll_timer);
    s_poll_timer = NULL;
  }
}

int main(void) {
  worker_init();
  worker_event_loop();
  worker_deinit();
}
