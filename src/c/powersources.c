// Power Sources -- Watchface mit dem Leistungsgraphen aus dem
// Home-Assistant-Energie-Dashboard ("Jetzt -> Stromquellen") oben und der
// Uhrzeit in Leco unten.
//
// Die Uhr rechnet nichts ueber Home Assistant: PebbleKit JS holt die
// Leistungsverlaeufe, verdichtet sie auf genau eine Spalte je Pixel und
// schickt drei int16-Reihen (Solar, Speicher, Netz; vorzeichenbehaftet, in
// 10-W-Schritten). Hier werden sie nur noch gestapelt und gezeichnet --
// oberhalb der Nulllinie Solar, Speicher-Entladung, Netzbezug, darunter
// Speicher-Laden und Einspeisung, dazu die Verbrauchslinie wie in HA.

#include <pebble.h>

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
// Uhrzeit fest auf 40 % der Hoehe, der Rest gehoert dem Graphen.
#ifdef PBL_PLATFORM_EMERY
#define SCREEN_W        200
#define SCREEN_H        228
#define TIME_TOP        137          // 228 - 91 (40 %)
#define TIME_BOTTOM     228
#define PLOT_LEFT       32
#define PLOT_RIGHT      194
#define PLOT_TOP        22
#define PLOT_BOTTOM     114
#define PLOT_BOTTOM_SOC 100          // mit Fuellstandsbalken darunter
#define SOC_Y           121
#define YLABEL_RIGHT    29           // rechte Kante der Y-Beschriftung
#define UNIT_X          0
#define UNIT_Y          0
#define UNIT_W          YLABEL_RIGHT
#define UNIT_ALIGN      GTextAlignmentRight
#define XLABEL_MAX_X    199
#else  // gabbro, rund
#define SCREEN_W        260
#define SCREEN_H        260
#define TIME_TOP        156          // 260 - 104 (40 %)
#define TIME_BOTTOM     240          // unten laeuft das Rund zusammen
#define PLOT_LEFT       64
#define PLOT_RIGHT      224
#define PLOT_TOP        46
#define PLOT_BOTTOM     136
#define PLOT_BOTTOM_SOC 122          // mit Fuellstandsbalken darunter
#define SOC_Y           143
#define YLABEL_RIGHT    61
#define UNIT_X          100
#define UNIT_Y          24
#define UNIT_W          60
#define UNIT_ALIGN      GTextAlignmentCenter
#define XLABEL_MAX_X    236          // Rund auf Hoehe der Zeitachse
#endif

#define PLOT_W          (PLOT_RIGHT - PLOT_LEFT)
// Die Unterkante haengt davon ab, ob ein Fuellstandsbalken Platz braucht.
#define PLOT_H          (s_plot_bottom - PLOT_TOP)
#define XLABEL_Y        (s_plot_bottom + 1)
#define SOC_H           8            // Balken zwischen Graph und Uhrzeit
#define SOC_NONE        (-1)
#define MAX_COLS        180
#define MAX_TICKS       6            // Rasterabschnitte auf der Y-Achse (min. 15 px)
#define XLABEL_W        40
#define XLABEL_EDGE     6            // Marken dichter am Achsbeginn weglassen
#define XTICK_LEN       3
#define USAGE_WIDTH     2
#define TEXT_NUDGE      9            // Gothic 14: Oberkante bis Schriftmitte
#define TIME_NUDGE      14           // Leco 60: Leerraum ueber den Ziffern

#define FONT_TIME       FONT_KEY_LECO_60_NUMBERS_AM_PM
#define FONT_AXIS       FONT_KEY_GOTHIC_14
#define FONT_STATUS     FONT_KEY_GOTHIC_18_BOLD

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define REFRESH_MIN     5            // HA-Statistik liegt in 5-Minuten-Schritten vor
#define STALE_SEC       (30 * 60)    // aelter: Hinweis statt Einheit

// ---------------------------------------------------------------------------
// Farben
// ---------------------------------------------------------------------------
// Reihenfarben angelehnt an HA (Solar orange, Speicher gruen, Netz blau),
// aber mit mehr Abstand in Farbton und Helligkeit: Gabbro entsaettigt,
// und Tuerkis neben Hellblau war dort kaum zu unterscheiden.
#define COLOR_SOLAR     GColorChromeYellow
#define COLOR_BATTERY   GColorMalachite
#define COLOR_GRID      GColorBlueMoon
#define COLOR_GRID_DARK GColorDarkGray   // Rasterlinien auf dunklem Grund
#define COLOR_GRID_LITE GColorLightGray  // Rasterlinien auf hellem Grund

#define DEFAULT_DAY_BG   GColorBlack
#define DEFAULT_DAY_FG   GColorWhite
#define DEFAULT_NIGHT_BG GColorWhite
#define DEFAULT_NIGHT_FG GColorBlack

// ---------------------------------------------------------------------------
// Persistenz
// ---------------------------------------------------------------------------
#define KEY_SETTINGS     1
#define KEY_GRAPH_BASE   10          // 10..14, je 256 Byte
#define PERSIST_CHUNK    256
#define SETTINGS_VERSION 1           // neue Felder nur hinten anfuegen

// Fehlercodes aus PebbleKit JS
enum {
  ERR_NONE = 0,
  ERR_CONFIG,
  ERR_AUTH,
  ERR_LINK,
  ERR_NO_SENSORS,
  ERR_NO_WEBSOCKET,
  ERR_HA,
};

enum { SERIES_SOLAR = 0, SERIES_BATTERY, SERIES_GRID, SERIES_COUNT };

typedef struct {
  uint8_t version;
  uint8_t night_mode;
  uint8_t day_bg, day_fg, night_bg, night_fg;   // GColor8.argb
  int16_t night_start;                          // Minuten ab Mitternacht
  int16_t day_start;
} Settings;

typedef struct {
  int32_t end_time;                             // Zeitpunkt der letzten Spalte
  int16_t range_h;
  int16_t cols;
  int16_t v[SERIES_COUNT][MAX_COLS];            // 10-W-Einheiten, signed
  int16_t soc;                                  // 0..100, SOC_NONE = kein Balken
} GraphData;

static Window *s_window;
static Layer *s_canvas;
static Settings s_settings;
static GraphData s_graph;
static int s_error = ERR_NONE;
static int s_plot_bottom = PLOT_BOTTOM;
static GFont s_font_time, s_font_axis, s_font_status;

static const GColor SERIES_COLORS[SERIES_COUNT] = {
  COLOR_SOLAR, COLOR_BATTERY, COLOR_GRID,
};

// ---------------------------------------------------------------------------
// Persistenz
// ---------------------------------------------------------------------------
static void settings_defaults(void) {
  s_settings = (Settings){
    .version = SETTINGS_VERSION,
    .night_mode = 0,
    .day_bg = DEFAULT_DAY_BG.argb,
    .day_fg = DEFAULT_DAY_FG.argb,
    .night_bg = DEFAULT_NIGHT_BG.argb,
    .night_fg = DEFAULT_NIGHT_FG.argb,
    .night_start = 22 * 60,
    .day_start = 7 * 60,
  };
}

static void settings_load(void) {
  settings_defaults();
  if (persist_exists(KEY_SETTINGS)) {
    Settings stored;
    int n = persist_read_data(KEY_SETTINGS, &stored, sizeof(stored));
    if (n == (int)sizeof(stored) && stored.version == SETTINGS_VERSION) {
      s_settings = stored;
    }
  }
}

static void settings_save(void) {
  persist_write_data(KEY_SETTINGS, &s_settings, sizeof(s_settings));
}

// persist_write_data nimmt hoechstens 256 Byte je Schluessel.
static void graph_save(void) {
  const uint8_t *p = (const uint8_t *)&s_graph;
  int left = sizeof(s_graph);
  for (uint32_t key = KEY_GRAPH_BASE; left > 0; key++) {
    int n = left > PERSIST_CHUNK ? PERSIST_CHUNK : left;
    persist_write_data(key, p, n);
    p += n;
    left -= n;
  }
}

static void graph_load(void) {
  memset(&s_graph, 0, sizeof(s_graph));
  uint8_t *p = (uint8_t *)&s_graph;
  int left = sizeof(s_graph);
  for (uint32_t key = KEY_GRAPH_BASE; left > 0; key++) {
    int n = left > PERSIST_CHUNK ? PERSIST_CHUNK : left;
    if (persist_read_data(key, p, n) != n) {
      memset(&s_graph, 0, sizeof(s_graph));
      return;
    }
    p += n;
    left -= n;
  }
  if (s_graph.cols < 0 || s_graph.cols > MAX_COLS || s_graph.range_h <= 0) {
    memset(&s_graph, 0, sizeof(s_graph));
  }
  if (s_graph.cols == 0 || s_graph.soc < 0 || s_graph.soc > 100) s_graph.soc = SOC_NONE;
}

// ---------------------------------------------------------------------------
// Farben
// ---------------------------------------------------------------------------
static bool is_night(const struct tm *t) {
  if (!s_settings.night_mode) return false;
  int now = t->tm_hour * 60 + t->tm_min;
  int a = s_settings.night_start, b = s_settings.day_start;
  if (a == b) return false;
  return a < b ? (now >= a && now < b) : (now >= a || now < b);
}

static bool is_dark(GColor c) {
  // Kanaele je 0..3; Gruen zaehlt fuer die Helligkeit am meisten.
  return (c.r * 2 + c.g * 5 + c.b) < 12;
}

// ---------------------------------------------------------------------------
// Zeichnen
// ---------------------------------------------------------------------------
static int value_to_y(int32_t v, int zero_y, int32_t units_per_plot) {
  return zero_y - (int)(v * PLOT_H / units_per_plot);
}

// Schrittweite in 10-W-Einheiten: 0,1 kW bis 100 kW
static const int16_t STEPS[] = { 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000 };

static void format_kw(char *buf, size_t size, int32_t units, int32_t step) {
  // units in 10 W -> Zehntel-kW = units / 10
  int32_t tenths = units / 10;
  const char *sign = tenths < 0 ? "-" : "";
  int32_t a = tenths < 0 ? -tenths : tenths;
  if (step % 100 == 0) {
    snprintf(buf, size, "%s%d", sign, (int)(a / 10));
  } else {
    snprintf(buf, size, "%s%d.%d", sign, (int)(a / 10), (int)(a % 10));
  }
}

static void draw_status(GContext *ctx, GColor fg, const char *text) {
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, text, s_font_status,
                     GRect(PLOT_LEFT - 20, PLOT_TOP + PLOT_H / 2 - 24, PLOT_W + 20, 50),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static const char *error_text(int err) {
  switch (err) {
    case ERR_CONFIG:       return "Set up Home Assistant in the settings";
    case ERR_AUTH:         return "Home Assistant rejected the token";
    case ERR_LINK:         return "Home Assistant not reachable";
    case ERR_NO_SENSORS:   return "No power sensors in the energy dashboard";
    case ERR_NO_WEBSOCKET: return "Phone app lacks WebSocket support";
    case ERR_HA:           return "Home Assistant returned an error";
    default:               return "Loading...";
  }
}

static void draw_x_labels(GContext *ctx, GColor fg, GColor grid) {
  int range = s_graph.range_h;
  int step_h = range <= 3 ? 1 : range <= 6 ? 2 : range <= 12 ? 3 : 6;
  time_t end = s_graph.end_time;
  time_t start = end - (time_t)range * 3600;

  // Erste volle Rasterstunde nach dem Start, in Ortszeit gerechnet.
  struct tm *lt = localtime(&start);
  int32_t gmtoff = lt->tm_gmtoff;
  int32_t step_s = step_h * 3600;
  int32_t local = start + gmtoff;
  time_t t = (local / step_s + 1) * step_s - gmtoff;

  graphics_context_set_text_color(ctx, fg);
  for (; t <= end; t += step_s) {
    int x = PLOT_LEFT + (int)((int32_t)(t - start) * PLOT_W / (range * 3600));
    graphics_context_set_stroke_color(ctx, grid);
    graphics_draw_line(ctx, GPoint(x, s_plot_bottom + 1), GPoint(x, s_plot_bottom + XTICK_LEN));

    char buf[12];
    struct tm *lt2 = localtime(&t);
    if (clock_is_24h_style()) {
      strftime(buf, sizeof(buf), "%H:%M", lt2);
    } else {
      strftime(buf, sizeof(buf), "%I %p", lt2);
    }
    const char *text = (buf[0] == '0' && !clock_is_24h_style()) ? buf + 1 : buf;

    // Am Rand nach innen schieben statt weglassen: die letzte Marke liegt
    // fast immer direkt am rechten Ende.
    if (x < PLOT_LEFT + XLABEL_EDGE) continue;
    int tw = graphics_text_layout_get_content_size(text, s_font_axis,
               GRect(0, 0, XLABEL_W, 16), GTextOverflowModeFill, GTextAlignmentLeft).w;
    int lx = x - tw / 2;
    if (lx + tw > XLABEL_MAX_X) lx = XLABEL_MAX_X - tw;
    graphics_draw_text(ctx, text, s_font_axis, GRect(lx, XLABEL_Y, tw + 2, 16),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
}

static void draw_graph(GContext *ctx, GColor fg, GColor grid, time_t now) {
  int cols = s_graph.cols;
  if (cols == 0) {
    draw_status(ctx, fg, error_text(s_error));
    return;
  }

  // Wertebereich aus den gestapelten Reihen
  int32_t max_pos = 0, min_neg = 0;
  for (int i = 0; i < cols; i++) {
    int32_t pos = 0, neg = 0;
    for (int s = 0; s < SERIES_COUNT; s++) {
      int32_t v = s_graph.v[s][i];
      if (v > 0) pos += v;
      else if (s != SERIES_SOLAR) neg += v;
    }
    if (pos > max_pos) max_pos = pos;
    if (neg < min_neg) min_neg = neg;
  }

  int32_t step = STEPS[ARRAY_LENGTH(STEPS) - 1];
  int ticks_pos = 1, ticks_neg = 0;
  for (unsigned k = 0; k < ARRAY_LENGTH(STEPS); k++) {
    int tp = (max_pos + STEPS[k] - 1) / STEPS[k];
    int tn = (-min_neg + STEPS[k] - 1) / STEPS[k];
    if (tp < 1) tp = 1;
    if (tp + tn <= MAX_TICKS) {
      step = STEPS[k];
      ticks_pos = tp;
      ticks_neg = tn;
      break;
    }
  }
  int ticks = ticks_pos + ticks_neg;
  int32_t units_per_plot = step * ticks;
  int zero_y = PLOT_TOP + ticks_pos * PLOT_H / ticks;

  // Raster und Y-Beschriftung
  graphics_context_set_text_color(ctx, fg);
  for (int k = -ticks_neg; k <= ticks_pos; k++) {
    int y = value_to_y(k * step, zero_y, units_per_plot);
    graphics_context_set_stroke_color(ctx, k == 0 ? fg : grid);
    if (k == 0) {
      graphics_draw_line(ctx, GPoint(PLOT_LEFT, y), GPoint(PLOT_RIGHT - 1, y));
    } else {
      for (int x = PLOT_LEFT; x < PLOT_RIGHT; x += 3) {
        graphics_draw_pixel(ctx, GPoint(x, y));
      }
    }
    char buf[12];
    format_kw(buf, sizeof(buf), k * step, step);
    graphics_draw_text(ctx, buf, s_font_axis,
                       GRect(YLABEL_RIGHT - 30, y - TEXT_NUDGE, 30, 16),
                       GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }

  // Gestapelte Flaechen, eine Pixelspalte je Wert
  for (int i = 0; i < cols; i++) {
    int x = PLOT_LEFT + i * PLOT_W / cols;
    int32_t pos = 0, neg = 0;
    for (int s = 0; s < SERIES_COUNT; s++) {
      int32_t v = s_graph.v[s][i];
      int32_t from, to;
      if (v > 0) {
        from = pos; to = pos + v; pos = to;
      } else if (v < 0 && s != SERIES_SOLAR) {
        from = neg; to = neg + v; neg = to;
      } else {
        continue;
      }
      int y1 = value_to_y(from, zero_y, units_per_plot);
      int y2 = value_to_y(to, zero_y, units_per_plot);
      if (y1 == y2) continue;
      int top = y1 < y2 ? y1 : y2;
      int h = y1 < y2 ? y2 - y1 : y1 - y2;
      graphics_context_set_fill_color(ctx, SERIES_COLORS[s]);
      graphics_fill_rect(ctx, GRect(x, top, 1, h), 0, GCornerNone);
    }
  }

  // Verbrauchslinie: Summe aller Quellen, nie unter null (wie in HA)
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_stroke_width(ctx, USAGE_WIDTH);
  GPoint prev = GPointZero;
  for (int i = 0; i < cols; i++) {
    int32_t u = 0;
    for (int s = 0; s < SERIES_COUNT; s++) u += s_graph.v[s][i];
    if (u < 0) u = 0;
    GPoint p = GPoint(PLOT_LEFT + i * PLOT_W / cols, value_to_y(u, zero_y, units_per_plot));
    if (i > 0) graphics_draw_line(ctx, prev, p);
    prev = p;
  }
  graphics_context_set_stroke_width(ctx, 1);

  draw_x_labels(ctx, fg, grid);

  // Einheit, bei veralteten Daten stattdessen ein Hinweis
  const char *unit = "kW";
  if (s_error != ERR_NONE && now - s_graph.end_time > STALE_SEC) unit = "offline";
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, unit, s_font_axis, GRect(UNIT_X, UNIT_Y, UNIT_W, 16),
                     GTextOverflowModeFill, UNIT_ALIGN, NULL);
}

static void draw_time(GContext *ctx, GColor fg, const struct tm *t) {
  static char buf[8];
  strftime(buf, sizeof(buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  const char *text = (!clock_is_24h_style() && buf[0] == '0') ? buf + 1 : buf;
  graphics_context_set_text_color(ctx, fg);
  int h = TIME_BOTTOM - TIME_TOP;
  graphics_draw_text(ctx, text, s_font_time,
                     GRect(0, TIME_TOP + h / 2 - 30 - TIME_NUDGE, SCREEN_W, 70),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Fuellstand aller Speicher (stat_soc aus dem Energie-Dashboard), buendig
// unter dem Graphen, die Prozentzahl in der Spalte der Y-Beschriftung.
static void draw_soc(GContext *ctx, GColor fg) {
  if (s_graph.soc == SOC_NONE) return;
  GRect frame = GRect(PLOT_LEFT, SOC_Y, PLOT_W, SOC_H);
  int fill = (PLOT_W - 2) * s_graph.soc / 100;
  if (fill > 0) {
    graphics_context_set_fill_color(ctx, COLOR_BATTERY);
    graphics_fill_rect(ctx, GRect(PLOT_LEFT + 1, SOC_Y + 1, fill, SOC_H - 2), 0, GCornerNone);
  }
  graphics_context_set_stroke_color(ctx, fg);
  graphics_draw_rect(ctx, frame);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (int)s_graph.soc);
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, buf, s_font_axis,
                     GRect(YLABEL_RIGHT - 30, SOC_Y + SOC_H / 2 - TEXT_NUDGE, 30, 16),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  s_plot_bottom = s_graph.soc == SOC_NONE ? PLOT_BOTTOM : PLOT_BOTTOM_SOC;

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  struct tm tcopy = *t;
  bool night = is_night(&tcopy);

  GColor bg = (GColor){ .argb = night ? s_settings.night_bg : s_settings.day_bg };
  GColor fg = (GColor){ .argb = night ? s_settings.night_fg : s_settings.day_fg };
  if (gcolor_equal(bg, fg)) fg = is_dark(bg) ? GColorWhite : GColorBlack;
  GColor grid = is_dark(bg) ? COLOR_GRID_DARK : COLOR_GRID_LITE;

  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  draw_graph(ctx, fg, grid, now);
  draw_soc(ctx, fg);
  draw_time(ctx, fg, &tcopy);
}

// ---------------------------------------------------------------------------
// Kommunikation
// ---------------------------------------------------------------------------
static void request_data(void) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_int32(it, MESSAGE_KEY_COLS, PLOT_W);
  app_message_outbox_send();
}

static int32_t tuple_int(Tuple *t) {
  return t->type == TUPLE_CSTRING ? atoi(t->value->cstring) : t->value->int32;
}

static void read_series(DictionaryIterator *iter, uint32_t key, int s, int *cols) {
  memset(s_graph.v[s], 0, sizeof(s_graph.v[s]));
  Tuple *t = dict_find(iter, key);
  if (!t || t->type != TUPLE_BYTE_ARRAY) return;
  int n = t->length / 2;
  if (n > MAX_COLS) n = MAX_COLS;
  const uint8_t *d = (const uint8_t *)t->value;   // little endian, je 2 Byte
  for (int i = 0; i < n; i++) {
    s_graph.v[s][i] = (int16_t)(d[2 * i] | (d[2 * i + 1] << 8));
  }
  if (n > *cols) *cols = n;
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  bool dirty = false;

  // Einstellungen
  bool settings_changed = false;
  if ((t = dict_find(iter, MESSAGE_KEY_DAY_BG)))      { s_settings.day_bg = GColorFromHEX(tuple_int(t)).argb; settings_changed = true; }
  if ((t = dict_find(iter, MESSAGE_KEY_DAY_FG)))      { s_settings.day_fg = GColorFromHEX(tuple_int(t)).argb; settings_changed = true; }
  if ((t = dict_find(iter, MESSAGE_KEY_NIGHT_BG)))    { s_settings.night_bg = GColorFromHEX(tuple_int(t)).argb; settings_changed = true; }
  if ((t = dict_find(iter, MESSAGE_KEY_NIGHT_FG)))    { s_settings.night_fg = GColorFromHEX(tuple_int(t)).argb; settings_changed = true; }
  if ((t = dict_find(iter, MESSAGE_KEY_NIGHT_MODE)))  { s_settings.night_mode = tuple_int(t) ? 1 : 0; settings_changed = true; }
  if ((t = dict_find(iter, MESSAGE_KEY_NIGHT_START))) { s_settings.night_start = tuple_int(t); settings_changed = true; }
  if ((t = dict_find(iter, MESSAGE_KEY_DAY_START)))   { s_settings.day_start = tuple_int(t); settings_changed = true; }
  if (settings_changed) {
    settings_save();
    dirty = true;
  }

  if ((t = dict_find(iter, MESSAGE_KEY_ERROR))) {
    s_error = tuple_int(t);
    dirty = true;
  }

  // Graph
  if ((t = dict_find(iter, MESSAGE_KEY_END_TIME))) {
    s_graph.end_time = tuple_int(t);
    Tuple *r = dict_find(iter, MESSAGE_KEY_RANGE_H);
    s_graph.range_h = r ? tuple_int(r) : 12;
    int cols = 0;
    read_series(iter, MESSAGE_KEY_SOLAR, SERIES_SOLAR, &cols);
    read_series(iter, MESSAGE_KEY_BATTERY, SERIES_BATTERY, &cols);
    read_series(iter, MESSAGE_KEY_GRID, SERIES_GRID, &cols);
    s_graph.cols = cols;
    Tuple *soc = dict_find(iter, MESSAGE_KEY_SOC);
    s_graph.soc = soc ? tuple_int(soc) : SOC_NONE;
    if (s_graph.soc > 100) s_graph.soc = 100;
    if (s_graph.soc < 0) s_graph.soc = SOC_NONE;
    graph_save();
    dirty = true;
  }

  // PebbleKit JS ist bereit: erst jetzt kann die Anfrage ankommen.
  if (dict_find(iter, MESSAGE_KEY_JS_READY)) request_data();

  if (dirty) layer_mark_dirty(s_canvas);
}

static void bt_handler(bool connected) {
  if (connected) request_data();
}

// ---------------------------------------------------------------------------
// Ablauf
// ---------------------------------------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  layer_mark_dirty(s_canvas);
  if (tick_time->tm_min % REFRESH_MIN == 0) request_data();
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void init(void) {
  settings_load();
  graph_load();

  s_font_time = fonts_get_system_font(FONT_TIME);
  s_font_axis = fonts_get_system_font(FONT_AXIS);
  s_font_status = fonts_get_system_font(FONT_STATUS);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_open(2048, 64);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  connection_service_subscribe((ConnectionHandlers){
    .pebble_app_connection_handler = bt_handler,
  });
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
