#include <pebble.h>

#define FACE_R          (PBL_DISPLAY_WIDTH / 2)
#define CX              (PBL_DISPLAY_WIDTH / 2)
#define CY              (PBL_DISPLAY_HEIGHT / 2)

#define S(x)            ((x) * FACE_R / 130)

#define MIN_TIP_LEN     S(112)
#define MIN_TAIL_LEN    S(20)
#define HOUR_INNER      S(67)
#define HOUR_OUTER      S(112)
#define SEC_ORBIT       S(89)
#define SEC_DOT_R       S(21)
#define BAT_DOT_R       S(6)
// Battery dot sits straight below the centre, just inside the seconds dot's
// path, so the two never touch when the seconds dot passes the 30 mark.
#define BAT_DOT_GAP     S(6)
#define BAT_DOT_DIST    (SEC_ORBIT - SEC_DOT_R - BAT_DOT_R - BAT_DOT_GAP)
#define HAND_FADE       S(6)
#define HAND_SOLID_HW   S(9)
#define HOUR_SOLID_HW   S(7)
#define MIN_HW          S(12)
#define HOUR_HW         S(10)
#define ICLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

// Uncomment to always show the battery dot regardless of charge level (demo).
// #define DEMO_BATTERY

// Uncomment to freeze the clock at 10:08:30.450 with the battery dot shown,
// for repeatable screenshots.
// #define SCREENSHOT_MODE
#ifdef SCREENSHOT_MODE
#define DEMO_BATTERY
#endif

static Window   *s_window;
static Layer    *s_canvas;
static AppTimer *s_timer;
static GColor    s_bg_color;
static int32_t   s_ms_accum = 0;  // ms within current minute, for dot position

#define PERSIST_KEY_BG_COLOR      1
#define PERSIST_KEY_SEC_INTERVAL  2
#define PERSIST_KEY_BAT_THRESHOLD 3

// 0 = smooth (100 ms), 1 = half-second (500 ms), 2 = ticking (1 s),
// 3 = auto (smooth while the watch is tilted toward the wearer, ticking otherwise)
#define SEC_MODE_SMOOTH   0
#define SEC_MODE_HALF     1
#define SEC_MODE_TICKING  2
#define SEC_MODE_AUTO     3
#define SEC_MODE_DEFAULT  SEC_MODE_TICKING
// Auto mode: the accelerometer is sampled at 10 Hz and the face's tilt toward
// the wearer is checked once per batch. A tilt between LOOK_MIN_DEG and
// LOOK_MAX_DEG counts as "looking at the watch". Smooth mode is held for
// AUTO_SMOOTH_MS after the last batch that looked like a glance.
#define LOOK_MIN_DEG       20
#define LOOK_MAX_DEG       60
#define ACCEL_BATCH        5      // samples per callback (5 at 10 Hz = 0.5 s)
#define AUTO_SMOOTH_MS     1500
static int s_sec_interval;
static bool      s_auto_smooth;          // Auto mode: currently in smooth burst
static AppTimer *s_auto_off_timer;
static bool      s_tick_subscribed;
static bool      s_accel_subscribed;
// 0 = 5%, 1 = 10%, 2 = 25%, 3 = off
static int s_bat_threshold;
static BatteryChargeState s_battery;

// Hand cache: a copy of the frame buffer with background and hands drawn.
// In smooth / half-second modes the hands only move once per second, so the
// frames in between copy this instead of redrawing the hands.
static uint8_t *s_cache;
static size_t   s_cache_size;
static bool     s_cache_valid;
static int32_t  s_cache_min_angle;
static int32_t  s_cache_hour_angle;

// #define SIM_MODE
#ifdef SIM_MODE
#define SIM_SPEED     144u
#define SIM_PERIOD_MS (12u * 60u * 60u * 1000u)
static uint32_t s_sim_ms;
static void sim_advance(uint32_t real_ms) {
  s_sim_ms = (s_sim_ms + real_ms * SIM_SPEED) % SIM_PERIOD_MS;
}
static void sim_get(int *hour, int *min, int *sec, int *ms_out) {
  uint32_t t = s_sim_ms;
  *ms_out = (int)(t % 1000); t /= 1000;
  *sec    = (int)(t % 60);   t /= 60;
  *min    = (int)(t % 60);   t /= 60;
  *hour   = (int)(t % 12);
}
#endif

static int valid_sec_mode(int v) {
  return (v >= SEC_MODE_SMOOTH && v <= SEC_MODE_AUTO) ? v : SEC_MODE_DEFAULT;
}

static void load_sec_interval(void) {
  s_sec_interval = persist_exists(PERSIST_KEY_SEC_INTERVAL)
                     ? valid_sec_mode(persist_read_int(PERSIST_KEY_SEC_INTERVAL))
                     : SEC_MODE_DEFAULT;
}

static void load_bat_threshold(void) {
  s_bat_threshold = ICLAMP(persist_read_int(PERSIST_KEY_BAT_THRESHOLD), 0, 3);
}

static void battery_handler(BatteryChargeState state) {
  s_battery = state;
  layer_mark_dirty(s_canvas);
}

static void timer_callback(void *data);
static void tick_handler(struct tm *tick_time, TimeUnits units_changed);
static void accel_handler(AccelData *data, uint32_t num_samples);

// Redraw period in ms for the current mode; 0 means "once per second,
// aligned to the clock via the tick timer service".
static uint32_t current_period_ms(void) {
  switch (s_sec_interval) {
    case SEC_MODE_SMOOTH: return 100;
    case SEC_MODE_HALF:   return 500;
    case SEC_MODE_AUTO:   return s_auto_smooth ? 100 : 0;
    default:              return 0;
  }
}

static void apply_sec_interval(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  uint32_t ms = current_period_ms();
  if (ms == 0) {
    if (!s_tick_subscribed) {
      tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
      s_tick_subscribed = true;
    }
  } else {
    if (s_tick_subscribed) {
      tick_timer_service_unsubscribe();
      s_tick_subscribed = false;
    }
    s_timer = app_timer_register(ms, timer_callback, NULL);
  }

  bool want_accel = (s_sec_interval == SEC_MODE_AUTO);
  if (want_accel && !s_accel_subscribed) {
    accel_data_service_subscribe(ACCEL_BATCH, accel_handler);
    accel_service_set_sampling_rate(ACCEL_SAMPLING_10HZ);
    s_accel_subscribed = true;
  } else if (!want_accel && s_accel_subscribed) {
    accel_data_service_unsubscribe();
    s_accel_subscribed = false;
  }
  if (!want_accel && s_auto_off_timer) {
    app_timer_cancel(s_auto_off_timer);
    s_auto_off_timer = NULL;
    s_auto_smooth = false;
  }
}

static void auto_off_callback(void *data) {
  s_auto_off_timer = NULL;
  s_auto_smooth = false;
  apply_sec_interval();
}

// Start (or extend) a smooth burst in Auto mode.
static void auto_start_smooth(void) {
  if (s_sec_interval != SEC_MODE_AUTO) return;
  if (s_auto_off_timer) {
    app_timer_reschedule(s_auto_off_timer, AUTO_SMOOTH_MS);
  } else {
    s_auto_off_timer = app_timer_register(AUTO_SMOOTH_MS, auto_off_callback, NULL);
  }
  if (!s_auto_smooth) {
    s_auto_smooth = true;
    apply_sec_interval();
  }
}

// Averages a batch of samples and checks whether the face is tilted toward
// the wearer. Axes are in milli-g: flat face-up reads z = -1000, and lifting
// the 12 o'clock edge toward you drives y negative. The tilt angle is
// atan2(-y, -z): 0 deg flat, 90 deg upright, > 90 deg past vertical/face down.
static void accel_handler(AccelData *data, uint32_t num_samples) {
  int32_t sy = 0, sz = 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < num_samples; i++) {
    if (data[i].did_vibrate) continue;  // motor shake corrupts readings
    sy += data[i].y;
    sz += data[i].z;
    n++;
  }
  if (n == 0) return;
  int32_t y = sy / (int32_t)n;
  int32_t z = sz / (int32_t)n;

  int32_t angle = atan2_lookup((int16_t)(-y), (int16_t)(-z));
  int32_t lo = TRIG_MAX_ANGLE * LOOK_MIN_DEG / 360;
  int32_t hi = TRIG_MAX_ANGLE * LOOK_MAX_DEG / 360;
  if (angle >= lo && angle <= hi) {
    auto_start_smooth();
  }
}

static void load_bg_color(void) {
  int val = persist_read_int(PERSIST_KEY_BG_COLOR);
  switch (val) {
    case 1:  s_bg_color = GColorLightGray;     break;
    case 2:  s_bg_color = GColorPastelYellow;  break;
    default: s_bg_color = GColorWhite;         break;
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_BG_COLOR);
  if (t) {
    persist_write_int(PERSIST_KEY_BG_COLOR, (int)t->value->int32);
    load_bg_color();
    window_set_background_color(s_window, s_bg_color);
    s_cache_valid = false;
    layer_mark_dirty(s_canvas);
  }

  Tuple *ti = dict_find(iter, MESSAGE_KEY_SEC_INTERVAL);
  if (ti) {
    s_sec_interval = valid_sec_mode((int)ti->value->int32);
    persist_write_int(PERSIST_KEY_SEC_INTERVAL, s_sec_interval);
    apply_sec_interval();
    auto_start_smooth();
  }

  Tuple *tb = dict_find(iter, MESSAGE_KEY_BAT_THRESHOLD);
  if (tb) {
    s_bat_threshold = ICLAMP((int)tb->value->int32, 0, 3);
    persist_write_int(PERSIST_KEY_BAT_THRESHOLD, s_bat_threshold);
    layer_mark_dirty(s_canvas);
  }
}

// ---------------------------------------------------------------------------
// Pixel output. Pixels are written straight into the frame buffer (one byte
// per pixel on colour screens, one bit on black-and-white ones), which avoids
// two SDK calls per pixel. If the frame buffer cannot be captured or has an
// unexpected format, it falls back to graphics_draw_pixel.

typedef struct {
  GContext          *ctx;
  GBitmap           *fb;       // NULL => fallback via graphics_draw_pixel
  bool               one_bit;
  GBitmapDataRowInfo row;      // current row, set by cv_row()
} Canvas;

static void cv_begin(Canvas *cv, GContext *ctx) {
  cv->ctx = ctx;
  cv->one_bit = false;
  cv->fb = graphics_capture_frame_buffer(ctx);
  if (cv->fb) {
    GBitmapFormat fmt = gbitmap_get_format(cv->fb);
    if (fmt == GBitmapFormat1Bit) {
      cv->one_bit = true;
    } else if (fmt != GBitmapFormat8Bit && fmt != GBitmapFormat8BitCircular) {
      graphics_release_frame_buffer(ctx, cv->fb);
      cv->fb = NULL;
    }
  }
  if (!cv->fb) graphics_context_set_stroke_width(ctx, 1);
}

static void cv_end(Canvas *cv) {
  if (cv->fb) graphics_release_frame_buffer(cv->ctx, cv->fb);
}

static inline void cv_row(Canvas *cv, int y) {
  if (cv->fb) cv->row = gbitmap_get_data_row_info(cv->fb, y);
}

// Black-and-white screens: which colours end up as a white pixel.
static inline bool bw_is_white(GColor c) {
  return gcolor_equal(c, GColorWhite) || gcolor_equal(c, GColorLightGray);
}

static inline void cv_put(Canvas *cv, int x, int y, GColor c) {
  if (!cv->fb) {
    graphics_context_set_stroke_color(cv->ctx, c);
    graphics_draw_pixel(cv->ctx, GPoint(x, y));
    return;
  }
  if (x < cv->row.min_x || x > cv->row.max_x) return;
  if (cv->one_bit) {
    uint8_t mask = (uint8_t)(1u << (x & 7));
    if (bw_is_white(c)) cv->row.data[x >> 3] |= mask;
    else                cv->row.data[x >> 3] &= (uint8_t)~mask;
  } else {
    cv->row.data[x] = c.argb;
  }
}

// Bytes of frame buffer row y and where they start (round screens store only
// the visible part of each row).
static size_t cv_row_bytes(Canvas *cv, int y, uint8_t **start) {
  GBitmapDataRowInfo r = gbitmap_get_data_row_info(cv->fb, y);
  if (cv->one_bit) {
    *start = r.data;
    return gbitmap_get_bytes_per_row(cv->fb);
  }
  if (r.max_x < r.min_x) { *start = r.data; return 0; }
  *start = r.data + r.min_x;
  return (size_t)(r.max_x - r.min_x + 1);
}

// Copies the frame buffer into the hand cache (store) or back (!store).
// Allocates the cache on first use. Returns false if it could not.
static bool cache_copy(Canvas *cv, bool store) {
  int h = gbitmap_get_bounds(cv->fb).size.h;
  uint8_t *src;
  if (!s_cache) {
    size_t total = 0;
    for (int y = 0; y < h; y++) total += cv_row_bytes(cv, y, &src);
    s_cache = malloc(total);
    if (!s_cache) return false;
    s_cache_size = total;
  }
  size_t off = 0;
  for (int y = 0; y < h; y++) {
    size_t len = cv_row_bytes(cv, y, &src);
    if (off + len > s_cache_size) return false;
    if (store) memcpy(s_cache + off, src, len);
    else       memcpy(src, s_cache + off, len);
    off += len;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Drawing

// 4×4 Bayer ordered-dither matrix (values 0–15).
static const uint8_t s_bayer[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 },
};

static int hand_threshold(int ad_perp, int end_dist, int solid_hw, int side_zone) {
  int threshold = 16;
  if (ad_perp > solid_hw && side_zone > 0) {
    int t = 16 - (ad_perp - solid_hw) * 16 / side_zone;
    if (t < threshold) threshold = t;
  }
  if (end_dist < HAND_FADE) {
    int t = end_dist * 16 / HAND_FADE;
    if (t < threshold) threshold = t;
  }
  return threshold;
}

static int dot_threshold(int d2, int inner_r2, int outer_r2) {
  if (d2 <= inner_r2) return 16;
  return 16 - (d2 - inner_r2) * 16 / (outer_r2 - inner_r2);
}

static GPoint point_at(int32_t angle, int radius) {
  return GPoint(
    CX + sin_lookup(angle) * radius / TRIG_MAX_RATIO,
    CY - cos_lookup(angle) * radius / TRIG_MAX_RATIO
  );
}

static bool grain_at(int x, int y) {
  uint32_t h = (uint32_t)x * 1664525u + (uint32_t)y * 22695477u;
  h ^= h >> 13;
  return (h & 0xFF) < 8;
}

// Narrows [*x0, *x1] to the dx values where lo <= c + k*dx <= hi, keeping one
// pixel of slack on each side so the exact per-pixel tests still decide every
// edge pixel. Returns false if the range is empty.
static bool narrow_span(int32_t c, int32_t k, int32_t lo, int32_t hi, int *x0, int *x1) {
  if (k == 0) return c >= lo && c <= hi;
  int32_t a = (lo - c) / k;
  int32_t b = (hi - c) / k;
  if (a > b) { int32_t t = a; a = b; b = t; }
  if (a - 1 > *x0) *x0 = (int)(a - 1);
  if (b + 1 < *x1) *x1 = (int)(b + 1);
  return *x0 <= *x1;
}

static void draw_hand(Canvas *cv, int32_t angle, int from_r, int to_r, int hw, int solid_hw) {
  int32_t perp = angle + TRIG_MAX_ANGLE / 4;

  int sin_a = sin_lookup(angle);
  int cos_a = cos_lookup(angle);
  int sin_p = sin_lookup(perp);
  int cos_p = cos_lookup(perp);

  int fx = CX + sin_a * from_r / TRIG_MAX_RATIO;
  int fy = CY - cos_a * from_r / TRIG_MAX_RATIO;
  int tx = CX + sin_a * to_r   / TRIG_MAX_RATIO;
  int ty = CY - cos_a * to_r   / TRIG_MAX_RATIO;
  int bx0 = ICLAMP((fx < tx ? fx : tx) - hw - 1, 0, PBL_DISPLAY_WIDTH  - 1);
  int bx1 = ICLAMP((fx > tx ? fx : tx) + hw + 1, 0, PBL_DISPLAY_WIDTH  - 1);
  int by0 = ICLAMP((fy < ty ? fy : ty) - hw - 1, 0, PBL_DISPLAY_HEIGHT - 1);
  int by1 = ICLAMP((fy > ty ? fy : ty) + hw + 1, 0, PBL_DISPLAY_HEIGHT - 1);

  int32_t from_s   = (int32_t)from_r * TRIG_MAX_RATIO;
  int32_t to_s     = (int32_t)to_r   * TRIG_MAX_RATIO;
  int32_t perp_lim = (int32_t)(hw + 1) * TRIG_MAX_RATIO;
  int side_zone    = hw - solid_hw;

  for (int y = by0; y <= by1; y++) {
    int dy = y - CY;

    // Only visit the part of this row the hand can cover.
    int x0 = bx0 - CX, x1 = bx1 - CX;
    if (!narrow_span(-(int32_t)dy * cos_a, sin_a, from_s, to_s, &x0, &x1)) continue;
    if (!narrow_span(-(int32_t)dy * cos_p, sin_p, -perp_lim, perp_lim, &x0, &x1)) continue;
    cv_row(cv, y);

    for (int dx = x0; dx <= x1; dx++) {
      int x = dx + CX;

      int32_t along_s = (int32_t)dx * sin_a - (int32_t)dy * cos_a;
      if (along_s < from_s || along_s > to_s) continue;

      int32_t perp_s = (int32_t)dx * sin_p - (int32_t)dy * cos_p;
      int ad_perp = (int)(perp_s < 0 ? -perp_s : perp_s) / TRIG_MAX_RATIO;
      if (ad_perp > hw) continue;

      int d_tail   = (int)((along_s - from_s) / TRIG_MAX_RATIO);
      int d_tip    = (int)((to_s   - along_s) / TRIG_MAX_RATIO);
      int end_dist = d_tail < d_tip ? d_tail : d_tip;

      int threshold = hand_threshold(ad_perp, end_dist, solid_hw, side_zone);
      if (threshold <= 0) continue;
      if (threshold < 16 && s_bayer[y & 3][x & 3] >= threshold) continue;

      cv_put(cv, x, y, grain_at(x, y) ? GColorLightGray : GColorDarkGray);
    }
  }
}

static void draw_dot(Canvas *cv, GPoint center, int radius) {
  const int dot_fade = radius >= 8 ? 2 : 1;
  const int inner_r  = radius - dot_fade;
  const int inner_r2 = inner_r * inner_r;
  const int outer_r2 = radius * radius;

  for (int gy = -radius; gy <= radius; gy++) {
    int py = center.y + gy;
    if (py < 0 || py >= PBL_DISPLAY_HEIGHT) continue;
    cv_row(cv, py);
    for (int gx = -radius; gx <= radius; gx++) {
      int d2 = gx * gx + gy * gy;
      if (d2 > outer_r2) continue;

      int px = center.x + gx;
      if (px < 0 || px >= PBL_DISPLAY_WIDTH) continue;

      int threshold = dot_threshold(d2, inner_r2, outer_r2);
      if (threshold <= 0) continue;
      if (threshold < 16 && s_bayer[py & 3][px & 3] >= threshold) continue;

      cv_put(cv, px, py, grain_at(px, py) ? GColorLightGray : GColorRed);
    }
  }
}

// Draws hands and dots. The background is already filled by the window's
// root layer (window_set_background_color), so it is not filled again here.
static void canvas_update_proc(Layer *layer, GContext *ctx) {
#ifdef SIM_MODE
  int hh, mm, ss, sim_ms;
  sim_get(&hh, &mm, &ss, &sim_ms);
  (void)sim_ms;
#else
  time_t now_t = time(NULL);
  struct tm *now = localtime(&now_t);
  int hh = now->tm_hour % 12, mm = now->tm_min, ss = now->tm_sec;
#endif
#ifdef SCREENSHOT_MODE
  hh = 10; mm = 8; ss = 30;
  s_ms_accum = 30450;
#endif
  int32_t min_angle = (TRIG_MAX_ANGLE * (mm * 60 + ss)) / 3600;
  int32_t hour_angle = (int32_t)(
    (int64_t)TRIG_MAX_ANGLE * (hh * 3600 + mm * 60 + ss) / (12 * 3600));

  Canvas cv;
  cv_begin(&cv, ctx);

  // The cache only pays off when several frames share one hand position.
  bool use_cache = cv.fb != NULL && current_period_ms() != 0;
  if (use_cache && s_cache_valid &&
      min_angle == s_cache_min_angle && hour_angle == s_cache_hour_angle &&
      cache_copy(&cv, false)) {
    // Background and hands restored from the cache.
  } else {
    draw_hand(&cv, min_angle, -MIN_TAIL_LEN, MIN_TIP_LEN, MIN_HW, HAND_SOLID_HW);
    draw_hand(&cv, hour_angle, HOUR_INNER, HOUR_OUTER, HOUR_HW, HOUR_SOLID_HW);
    s_cache_valid = false;
    if (use_cache && cache_copy(&cv, true)) {
      s_cache_valid      = true;
      s_cache_min_angle  = min_angle;
      s_cache_hour_angle = hour_angle;
    }
  }

  int32_t sec_angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * s_ms_accum / 60000);
  draw_dot(&cv, point_at(sec_angle, SEC_ORBIT), SEC_DOT_R);

#ifdef DEMO_BATTERY
  bool show_bat = true;
#else
  static const int bat_pct[] = { 5, 10, 25 };
  bool show_bat = s_bat_threshold < 3 &&
                  !s_battery.is_charging &&
                  (int)s_battery.charge_percent <=
                      bat_pct[ICLAMP(s_bat_threshold, 0, 2)];
#endif
  if (show_bat) {
    GPoint bat_pos = GPoint(CX, CY + BAT_DOT_DIST);
    draw_dot(&cv, bat_pos, BAT_DOT_R);
  }

  cv_end(&cv);
}

// Sets the dot position, never stepping backward by less than a second (clock
// jitter around a second boundary). A larger drop is the minute wrapping
// (59.9 s -> 0.1 s) and is allowed.
static void set_ms_accum(int32_t accum) {
  if (accum < s_ms_accum && s_ms_accum - accum < 1000) return;
  s_ms_accum = accum;
}

// Ticking: fires exactly on each new second, so no seconds are skipped or
// repeated.
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
#ifdef SIM_MODE
  sim_advance(1000);
  int sim_h2, sim_m2, sim_s2, sim_ms2;
  sim_get(&sim_h2, &sim_m2, &sim_s2, &sim_ms2);
  (void)sim_h2; (void)sim_m2; (void)sim_ms2;
  s_ms_accum = sim_s2 * 1000;
#else
  set_ms_accum(tick_time->tm_sec * 1000);
#endif
  layer_mark_dirty(s_canvas);
}

// Smooth / half-second: fires at the configured interval, updates the dot
// position from the real clock and redraws.
static void timer_callback(void *data) {
  s_timer = NULL;
  uint32_t next_ms = current_period_ms();
  if (next_ms == 0) {  // mode changed to ticking under us
    apply_sec_interval();
    return;
  }

#ifdef SIM_MODE
  sim_advance((int)next_ms);
  int sim_h2, sim_m2, sim_s2, sim_ms2;
  sim_get(&sim_h2, &sim_m2, &sim_s2, &sim_ms2);
  (void)sim_h2; (void)sim_m2;
  s_ms_accum = sim_s2 * 1000 + sim_ms2;
#else
  // Seconds and milliseconds come from one time_ms() call, so they always
  // belong to the same instant.
  time_t now_t;
  uint16_t now_ms;
  time_ms(&now_t, &now_ms);
  struct tm *now = localtime(&now_t);
  set_ms_accum(now->tm_sec * 1000 + now_ms);
#endif

  layer_mark_dirty(s_canvas);
  s_timer = app_timer_register(next_ms, timer_callback, NULL);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void prv_init(void) {
  load_bg_color();

  app_message_register_inbox_received(inbox_received);
  app_message_open(64, 64);

  s_window = window_create();
  window_set_background_color(s_window, s_bg_color);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, false);

  // Seed the dot so it starts at the correct second position.
  time_t seed_t = time(NULL);
  struct tm *seed = localtime(&seed_t);
  s_ms_accum = seed->tm_sec * 1000;

  load_bat_threshold();
  battery_state_service_subscribe(battery_handler);
  s_battery = battery_state_service_peek();

  load_sec_interval();
  apply_sec_interval();
  // The face just appeared, so the wearer is looking at it.
  auto_start_smooth();
}

static void prv_deinit(void) {
  battery_state_service_unsubscribe();
  if (s_accel_subscribed) accel_data_service_unsubscribe();
  if (s_tick_subscribed)  tick_timer_service_unsubscribe();
  if (s_auto_off_timer)   app_timer_cancel(s_auto_off_timer);
  if (s_timer)            app_timer_cancel(s_timer);
  window_destroy(s_window);
  free(s_cache);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
