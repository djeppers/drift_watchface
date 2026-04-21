#include <pebble.h>

// Face geometry — derived from the display at compile time.
// FACE_R : radius of the clock circle (half the display width).
// CX/CY  : center of the clock face in screen coordinates.
#define FACE_R          (PBL_DISPLAY_WIDTH / 2)
#define CX              (PBL_DISPLAY_WIDTH / 2)
#define CY              (PBL_DISPLAY_HEIGHT / 2)

// Scale a design-space value (original design radius 130) to this display.
#define S(x)            ((x) * FACE_R / 130)

#define MIN_TIP_LEN     S(112)
#define MIN_TAIL_LEN    S(20)
#define HOUR_INNER      S(67)
#define HOUR_OUTER      S(112)
#define SEC_ORBIT       S(89)
#define SEC_DOT_R       S(21)
#define HAND_FADE       S(6)
#define HAND_SOLID_HW   S(9)
#define MIN_HW          S(12)
#define HOUR_HW         S(10)
#define ICLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static Window   *s_window;
static Layer    *s_canvas;
static Layer    *s_dot_layer;
static AppTimer *s_timer;
static GColor    s_bg_color;

#define PERSIST_KEY_BG_COLOR   1
#define PERSIST_KEY_SEC_INTERVAL 2

// 0 = smooth (100 ms), 1 = half-second (500 ms), 2 = ticking (1 s)
static int s_sec_interval;

static void load_sec_interval(void) {
  s_sec_interval = persist_read_int(PERSIST_KEY_SEC_INTERVAL);
}

static void timer_callback(void *data);  // forward declaration

// Restart the seconds-dot timer at the interval matching s_sec_interval.
// Safe to call at any time; cancels any existing timer first.
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
}

// 4×4 Bayer ordered-dither matrix (values 0–15).
// Pixel is drawn when bayer[y%4][x%4] < threshold (0=transparent, 16=opaque).
static const uint8_t s_bayer[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 },
};

// Returns Bayer threshold (0–16) for a hand pixel.
// side_zone: width of the side-fade band (hw - HAND_SOLID_HW).
static int hand_threshold(int ad_perp, int end_dist, int side_zone) {
  int threshold = 16;
  if (ad_perp > HAND_SOLID_HW && side_zone > 0) {
    int t = 16 - (ad_perp - HAND_SOLID_HW) * 16 / side_zone;
    if (t < threshold) threshold = t;
  }
  if (end_dist < HAND_FADE) {
    int t = end_dist * 16 / HAND_FADE;
    if (t < threshold) threshold = t;
  }
  return threshold;
}

// Returns Bayer threshold (0–16) for a dot pixel based on squared distances.
static int dot_threshold(int d2, int inner_r2, int outer_r2) {
  if (d2 <= inner_r2) return 16;
  return 16 - (d2 - inner_r2) * 16 / (outer_r2 - inner_r2);
}

static GPoint point_at(int32_t angle, int radius) {
  GPoint c = GPoint(CX, CY);
  return GPoint(
    c.x + sin_lookup(angle) * radius / TRIG_MAX_RATIO,
    c.y - cos_lookup(angle) * radius / TRIG_MAX_RATIO
  );
}

// Stable per-pixel grain — ~3% of pixels become LightGray.
static bool grain_at(int x, int y) {
  uint32_t h = (uint32_t)x * 1664525u + (uint32_t)y * 22695477u;
  h ^= h >> 13;
  return (h & 0xFF) < 8;
}

// Per-pixel hand renderer with Bayer-dithered gradient.
//
// from_r : signed distance from center to inner end (negative = behind center)
// to_r   : signed distance from center to outer end
// hw     : half-width in pixels
static void draw_hand(GContext *ctx, int32_t angle, int from_r, int to_r, int hw) {
  GPoint c = GPoint(CX, CY);
  int32_t perp = angle + TRIG_MAX_ANGLE / 4;

  int sin_a = sin_lookup(angle);
  int cos_a = cos_lookup(angle);
  int sin_p = sin_lookup(perp);
  int cos_p = cos_lookup(perp);

  int fx = c.x + sin_a * from_r / TRIG_MAX_RATIO;
  int fy = c.y - cos_a * from_r / TRIG_MAX_RATIO;
  int tx = c.x + sin_a * to_r   / TRIG_MAX_RATIO;
  int ty = c.y - cos_a * to_r   / TRIG_MAX_RATIO;
  int bx0 = ICLAMP((fx < tx ? fx : tx) - hw - 1, 0, PBL_DISPLAY_WIDTH  - 1);
  int bx1 = ICLAMP((fx > tx ? fx : tx) + hw + 1, 0, PBL_DISPLAY_WIDTH  - 1);
  int by0 = ICLAMP((fy < ty ? fy : ty) - hw - 1, 0, PBL_DISPLAY_HEIGHT - 1);
  int by1 = ICLAMP((fy > ty ? fy : ty) + hw + 1, 0, PBL_DISPLAY_HEIGHT - 1);

  int32_t from_s = (int32_t)from_r * TRIG_MAX_RATIO;
  int32_t to_s   = (int32_t)to_r   * TRIG_MAX_RATIO;
  int side_zone  = hw - HAND_SOLID_HW;  // width of the side fade band

  graphics_context_set_stroke_width(ctx, 1);

  for (int y = by0; y <= by1; y++) {
    int dy = y - c.y;
    for (int x = bx0; x <= bx1; x++) {
      int dx = x - c.x;

      // Project pixel into hand-local coords (scaled by TRIG_MAX_RATIO)
      int32_t along_s = (int32_t)dx * sin_a - (int32_t)dy * cos_a;
      if (along_s < from_s || along_s > to_s) continue;

      int32_t perp_s = (int32_t)dx * sin_p - (int32_t)dy * cos_p;
      int ad_perp = (int)(perp_s < 0 ? -perp_s : perp_s) / TRIG_MAX_RATIO;
      if (ad_perp > hw) continue;

      int d_tail   = (int)((along_s - from_s) / TRIG_MAX_RATIO);
      int d_tip    = (int)((to_s   - along_s) / TRIG_MAX_RATIO);
      int end_dist = d_tail < d_tip ? d_tail : d_tip;

      int threshold = hand_threshold(ad_perp, end_dist, side_zone);
      if (threshold <= 0) continue;
      if (threshold < 16 && s_bayer[y & 3][x & 3] >= threshold) continue;

      GColor color = grain_at(x, y) ? GColorLightGray : GColorDarkGray;
      graphics_context_set_stroke_color(ctx, color);
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
}

// Draws background and hands only — updated once per second via tick.
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  time_t now_t = time(NULL);
  struct tm *now = localtime(&now_t);

  // Background
  graphics_context_set_fill_color(ctx, s_bg_color);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  // Minute hand
  int32_t min_angle =
    (TRIG_MAX_ANGLE * (now->tm_min * 60 + now->tm_sec)) / 3600;
  draw_hand(ctx, min_angle, -MIN_TAIL_LEN, MIN_TIP_LEN, MIN_HW);

  // Hour hand — use int64 to avoid overflow past ~9:07
  int32_t hour_angle = (int32_t)(
    (int64_t)TRIG_MAX_ANGLE * ((now->tm_hour % 12) * 3600 +
                                now->tm_min * 60 + now->tm_sec) / (12 * 3600));
  draw_hand(ctx, hour_angle, HOUR_INNER, HOUR_OUTER, HOUR_HW);
}

// Draws the seconds dot only — layer is repositioned each 100ms.
static void dot_update_proc(Layer *layer, GContext *ctx) {
  GRect frame = layer_get_frame(layer);
  int ox = frame.origin.x;
  int oy = frame.origin.y;

  // Dot center in local layer coordinates
  const int cx = SEC_DOT_R + 1;
  const int cy = SEC_DOT_R + 1;

  const int dot_fade  = 2;
  const int inner_r   = SEC_DOT_R - dot_fade;
  const int inner_r2  = inner_r * inner_r;
  const int outer_r2  = SEC_DOT_R * SEC_DOT_R;

  graphics_context_set_stroke_width(ctx, 1);
  for (int gy = -SEC_DOT_R; gy <= SEC_DOT_R; gy++) {
    for (int gx = -SEC_DOT_R; gx <= SEC_DOT_R; gx++) {
      int d2 = gx * gx + gy * gy;
      if (d2 > outer_r2) continue;

      int lx = cx + gx;
      int ly = cy + gy;
      // Absolute screen coords for stable Bayer/grain pattern
      int px = ox + lx;
      int py = oy + ly;

      int threshold = dot_threshold(d2, inner_r2, outer_r2);
      if (threshold <= 0) continue;
      if (threshold < 16 && s_bayer[py & 3][px & 3] >= threshold) continue;

      GColor color = grain_at(px, py) ? GColorLightGray : GColorRed;
      graphics_context_set_stroke_color(ctx, color);
      graphics_draw_pixel(ctx, GPoint(lx, ly));
    }
  }
}

// Called once per second — redraws hands.
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  layer_mark_dirty(s_canvas);
}

// Called every 100ms — moves the dot layer to its new position.
static void timer_callback(void *data) {
  time_t now_t;
  uint16_t ms;
  time_ms(&now_t, &ms);
  struct tm *now = localtime(&now_t);

  int32_t ms_in_min = now->tm_sec * 1000 + ms;
  int32_t sec_angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * ms_in_min / 60000);
  GPoint sec_pos = point_at(sec_angle, SEC_ORBIT);

  // Dot layer is (SEC_DOT_R+1) px of padding on each side around the center
  const int pad = SEC_DOT_R + 1;
  const int size = pad * 2;
  layer_set_frame(s_dot_layer, GRect(sec_pos.x - pad, sec_pos.y - pad, size, size));
  layer_mark_dirty(s_dot_layer);

  uint32_t next_ms = (s_sec_interval == 2) ? 1000 :
                     (s_sec_interval == 1) ?  500 : 100;
  s_timer = app_timer_register(next_ms, timer_callback, NULL);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  // Dot layer starts at origin; timer_callback will move it immediately
  const int dot_size = (SEC_DOT_R + 1) * 2;
  s_dot_layer = layer_create(GRect(0, 0, dot_size, dot_size));
  layer_set_update_proc(s_dot_layer, dot_update_proc);
  layer_add_child(root, s_dot_layer);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_dot_layer);
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

  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  load_sec_interval();
  apply_sec_interval();
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  app_timer_cancel(s_timer);
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
