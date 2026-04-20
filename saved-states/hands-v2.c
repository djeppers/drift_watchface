#include <pebble.h>

#define SCREEN_RADIUS   130
#define MIN_TIP_LEN     112
#define MIN_TAIL_LEN     20
#define HOUR_INNER       67
#define HOUR_OUTER      112
#define SEC_ORBIT        89
#define SEC_DOT_R        21
#define HAND_FADE        10   // px of gradient at each hand end
#define HAND_SOLID_HW     4   // perpendicular px that stay fully solid

static Window   *s_window;
static Layer    *s_canvas;
static AppTimer *s_timer;

// 4×4 Bayer ordered-dither matrix (values 0–15).
// Pixel is drawn when bayer[y%4][x%4] < threshold (0=transparent, 16=opaque).
static const uint8_t s_bayer[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 },
};

static GPoint point_at(int32_t angle, int radius) {
  GPoint c = GPoint(SCREEN_RADIUS, SCREEN_RADIUS);
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
// Threshold is derived from two distances:
//   side  : perpendicular distance from the hand axis
//   end   : distance from the nearest tip or tail
//
// Both distances are mapped to a 0–16 threshold that drives the Bayer matrix.
// 16 = fully solid, 0 = fully transparent.
//
// from_r : signed distance from center to inner end (negative = behind center)
// to_r   : signed distance from center to outer end
// hw     : half-width in pixels
static void draw_hand(GContext *ctx, int32_t angle, int from_r, int to_r, int hw) {
  GPoint c = GPoint(SCREEN_RADIUS, SCREEN_RADIUS);
  int32_t perp = angle + TRIG_MAX_ANGLE / 4;

  int sin_a = sin_lookup(angle);
  int cos_a = cos_lookup(angle);
  int sin_p = sin_lookup(perp);
  int cos_p = cos_lookup(perp);

  int fx = c.x + sin_a * from_r / TRIG_MAX_RATIO;
  int fy = c.y - cos_a * from_r / TRIG_MAX_RATIO;
  int tx = c.x + sin_a * to_r   / TRIG_MAX_RATIO;
  int ty = c.y - cos_a * to_r   / TRIG_MAX_RATIO;

#define ICLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
  int bx0 = ICLAMP((fx < tx ? fx : tx) - hw - 1, 0, SCREEN_RADIUS * 2 - 1);
  int bx1 = ICLAMP((fx > tx ? fx : tx) + hw + 1, 0, SCREEN_RADIUS * 2 - 1);
  int by0 = ICLAMP((fy < ty ? fy : ty) - hw - 1, 0, SCREEN_RADIUS * 2 - 1);
  int by1 = ICLAMP((fy > ty ? fy : ty) + hw + 1, 0, SCREEN_RADIUS * 2 - 1);

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

      // Bayer threshold: take the minimum of side and end contributions
      int threshold = 16;

      if (ad_perp > HAND_SOLID_HW && side_zone > 0) {
        int t = 16 - (ad_perp - HAND_SOLID_HW) * 16 / side_zone;
        if (t < threshold) threshold = t;
      }

      if (end_dist < HAND_FADE) {
        int t = end_dist * 16 / HAND_FADE;
        if (t < threshold) threshold = t;
      }

      if (threshold <= 0) continue;
      if (threshold < 16 && s_bayer[y & 3][x & 3] >= threshold) continue;

      GColor color = grain_at(x, y) ? GColorLightGray : GColorDarkGray;
      graphics_context_set_stroke_color(ctx, color);
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  time_t now_t;
  uint16_t ms;
  time_ms(&now_t, &ms);
  struct tm *now = localtime(&now_t);

  // Background
  graphics_context_set_fill_color(ctx, GColorPastelYellow);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  // Minute hand
  int32_t min_angle =
    (TRIG_MAX_ANGLE * (now->tm_min * 60 + now->tm_sec)) / 3600;
  draw_hand(ctx, min_angle, -MIN_TAIL_LEN, MIN_TIP_LEN, 15);

  // Hour hand
  int32_t hour_angle =
    (TRIG_MAX_ANGLE * ((now->tm_hour % 12) * 3600 +
                        now->tm_min * 60 + now->tm_sec)) / (12 * 3600);
  draw_hand(ctx, hour_angle, HOUR_INNER, HOUR_OUTER, 13);

  // Seconds dot
  int32_t ms_in_min = now->tm_sec * 1000 + ms;
  int32_t sec_angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * ms_in_min / 60000);
  GPoint sec_pos = point_at(sec_angle, SEC_ORBIT);
  graphics_context_set_fill_color(ctx, GColorRed);
  graphics_fill_circle(ctx, sec_pos, SEC_DOT_R);

  // Grain on the seconds dot
  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  for (int gy = -SEC_DOT_R; gy <= SEC_DOT_R; gy++) {
    for (int gx = -SEC_DOT_R; gx <= SEC_DOT_R; gx++) {
      if (gx * gx + gy * gy > SEC_DOT_R * SEC_DOT_R) continue;
      int px = sec_pos.x + gx;
      int py = sec_pos.y + gy;
      if (grain_at(px, py)) {
        graphics_draw_pixel(ctx, GPoint(px, py));
      }
    }
  }
}

static void timer_callback(void *data) {
  layer_mark_dirty(s_canvas);
  s_timer = app_timer_register(300, timer_callback, NULL);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorPastelYellow);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, false);

  s_timer = app_timer_register(300, timer_callback, NULL);
}

static void prv_deinit(void) {
  app_timer_cancel(s_timer);
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
