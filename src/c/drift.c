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
#define HAND_FADE       S(6)
#define HAND_SOLID_HW   S(9)
#define HOUR_SOLID_HW   S(7)
#define MIN_HW          S(12)
#define HOUR_HW         S(10)
#define ICLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

// Uncomment to always show the battery dot regardless of charge level (demo).
// #define DEMO_BATTERY

static Window   *s_window;
static Layer    *s_canvas;
static AppTimer *s_timer;
static GColor    s_bg_color;
static int32_t   s_ms_accum = 0;  // ms within current minute, for dot position
static int       s_sub_ms   = 0;  // sub-second accumulator (0–999)
static int       s_prev_sec = -1;

#define PERSIST_KEY_BG_COLOR      1
#define PERSIST_KEY_SEC_INTERVAL  2
#define PERSIST_KEY_BAT_THRESHOLD 3

// 0 = smooth (100 ms), 1 = half-second (500 ms), 2 = ticking (1 s)
static int s_sec_interval;
// 0 = 5%, 1 = 10%, 2 = 25%
static int s_bat_threshold;
static BatteryChargeState s_battery;

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

static void load_sec_interval(void) {
  s_sec_interval = persist_read_int(PERSIST_KEY_SEC_INTERVAL);
}

static void load_bat_threshold(void) {
  s_bat_threshold = persist_read_int(PERSIST_KEY_BAT_THRESHOLD);
}

static void battery_handler(BatteryChargeState state) {
  s_battery = state;
  layer_mark_dirty(s_canvas);
}

static void timer_callback(void *data);

static void apply_sec_interval(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  uint32_t ms = (s_sec_interval == 2) ? 1000 :
                (s_sec_interval == 1) ?  500 : 100;
  s_timer = app_timer_register(ms, timer_callback, NULL);
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
    layer_mark_dirty(s_canvas);
  }

  Tuple *ti = dict_find(iter, MESSAGE_KEY_SEC_INTERVAL);
  if (ti) {
    s_sec_interval = (int)ti->value->int32;
    persist_write_int(PERSIST_KEY_SEC_INTERVAL, s_sec_interval);
    apply_sec_interval();
  }

  Tuple *tb = dict_find(iter, MESSAGE_KEY_BAT_THRESHOLD);
  if (tb) {
    s_bat_threshold = (int)tb->value->int32;
    persist_write_int(PERSIST_KEY_BAT_THRESHOLD, s_bat_threshold);
    layer_mark_dirty(s_canvas);
  }
}

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

static void draw_hand(GContext *ctx, int32_t angle, int from_r, int to_r, int hw, int solid_hw) {
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

  int32_t from_s = (int32_t)from_r * TRIG_MAX_RATIO;
  int32_t to_s   = (int32_t)to_r   * TRIG_MAX_RATIO;
  int side_zone  = hw - solid_hw;

  graphics_context_set_stroke_width(ctx, 1);

  for (int y = by0; y <= by1; y++) {
    int dy = y - CY;
    for (int x = bx0; x <= bx1; x++) {
      int dx = x - CX;

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

      GColor color = grain_at(x, y) ? GColorLightGray : GColorDarkGray;
      graphics_context_set_stroke_color(ctx, color);
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
}

static void draw_dot(GContext *ctx, GPoint center, int radius) {
  const int dot_fade = radius >= 8 ? 2 : 1;
  const int inner_r  = radius - dot_fade;
  const int inner_r2 = inner_r * inner_r;
  const int outer_r2 = radius * radius;

  graphics_context_set_stroke_width(ctx, 1);
  for (int gy = -radius; gy <= radius; gy++) {
    for (int gx = -radius; gx <= radius; gx++) {
      int d2 = gx * gx + gy * gy;
      if (d2 > outer_r2) continue;

      int px = center.x + gx;
      int py = center.y + gy;
      if (px < 0 || px >= PBL_DISPLAY_WIDTH || py < 0 || py >= PBL_DISPLAY_HEIGHT) continue;

      int threshold = dot_threshold(d2, inner_r2, outer_r2);
      if (threshold <= 0) continue;
      if (threshold < 16 && s_bayer[py & 3][px & 3] >= threshold) continue;

      GColor color = grain_at(px, py) ? GColorLightGray : GColorRed;
      graphics_context_set_stroke_color(ctx, color);
      graphics_draw_pixel(ctx, GPoint(px, py));
    }
  }
}

// Single update proc draws background, hands, and dot in one pass.
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, s_bg_color);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

#ifdef SIM_MODE
  int sim_h, sim_m, sim_s, sim_ms;
  sim_get(&sim_h, &sim_m, &sim_s, &sim_ms);
  int32_t min_angle =
    (TRIG_MAX_ANGLE * (sim_m * 60 + sim_s)) / 3600;
  draw_hand(ctx, min_angle, -MIN_TAIL_LEN, MIN_TIP_LEN, MIN_HW, HAND_SOLID_HW);
  int32_t hour_angle = (int32_t)(
    (int64_t)TRIG_MAX_ANGLE * (sim_h * 3600 + sim_m * 60 + sim_s) / (12 * 3600));
  draw_hand(ctx, hour_angle, HOUR_INNER, HOUR_OUTER, HOUR_HW, HOUR_SOLID_HW);
  int32_t ms_in_min = sim_s * 1000 + sim_ms;
#else
  time_t now_t = time(NULL);
  struct tm *now = localtime(&now_t);
  int32_t min_angle =
    (TRIG_MAX_ANGLE * (now->tm_min * 60 + now->tm_sec)) / 3600;
  draw_hand(ctx, min_angle, -MIN_TAIL_LEN, MIN_TIP_LEN, MIN_HW, HAND_SOLID_HW);
  int32_t hour_angle = (int32_t)(
    (int64_t)TRIG_MAX_ANGLE * ((now->tm_hour % 12) * 3600 +
                                now->tm_min * 60 + now->tm_sec) / (12 * 3600));
  draw_hand(ctx, hour_angle, HOUR_INNER, HOUR_OUTER, HOUR_HW, HOUR_SOLID_HW);
#endif

  int32_t sec_angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * s_ms_accum / 60000);
  draw_dot(ctx, point_at(sec_angle, SEC_ORBIT), SEC_DOT_R);

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
    GPoint bat_pos = GPoint(CX, (CY + PBL_DISPLAY_HEIGHT) / 2);
    draw_dot(ctx, bat_pos, BAT_DOT_R);
  }
}

// Fires at the configured interval — updates the dot accumulator and redraws.
static void timer_callback(void *data) {
  uint32_t next_ms = (s_sec_interval == 2) ? 1000 :
                     (s_sec_interval == 1) ?  500 : 100;

#ifdef SIM_MODE
  sim_advance((int)next_ms);
  int sim_h2, sim_m2, sim_s2, sim_ms2;
  sim_get(&sim_h2, &sim_m2, &sim_s2, &sim_ms2);
  (void)sim_h2; (void)sim_m2;
  s_ms_accum = sim_s2 * 1000 + sim_ms2;
#else
  // Use time(NULL) for the second — it is an atomic integer read with no race
  // condition. Sub-second position comes from the timer accumulator, which only
  // ever increments, so it can never bounce backward at the second boundary.
  time_t now_t = time(NULL);
  struct tm *now = localtime(&now_t);
  if (now->tm_sec != s_prev_sec) {
    s_prev_sec = now->tm_sec;
    s_sub_ms   = 0;
  } else {
    s_sub_ms += (int)next_ms;
    if (s_sub_ms > 999) s_sub_ms = 999;
  }
  s_ms_accum = now->tm_sec * 1000 + s_sub_ms;
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

  // Seed the dot accumulator so it starts at the correct second position.
  time_t seed_t = time(NULL);
  struct tm *seed = localtime(&seed_t);
  s_prev_sec = seed->tm_sec;
  s_sub_ms   = 0;
  s_ms_accum = seed->tm_sec * 1000;

  load_bat_threshold();
  battery_state_service_subscribe(battery_handler);
  s_battery = battery_state_service_peek();

  load_sec_interval();
  apply_sec_interval();
}

static void prv_deinit(void) {
  battery_state_service_unsubscribe();
  app_timer_cancel(s_timer);
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
