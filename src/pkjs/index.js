// PebbleKit JS: holt den Leistungsverlauf aus dem Home-Assistant-Energie-
// Dashboard und schickt ihn verdichtet an die Uhr.
//
// Welche Sensoren gemeint sind, steht nur in den Energie-Einstellungen
// (energy/get_prefs) -- und die gibt es ausschliesslich ueber die WebSocket-
// API, nicht per REST. Deshalb laeuft alles ueber eine kurze WebSocket-
// Sitzung: anmelden, Einstellungen lesen, 5-Minuten-Statistik der
// stat_rate-Sensoren holen, aktuellen Zustand dazu, wieder schliessen.
//
// Das entspricht dem, was das HA-Frontend fuer "Jetzt -> Stromquellen"
// macht (power-sources-graph-data.ts): Solar, Speicher und Netz je als
// Summe ihrer stat_rate-Sensoren, Netz positiv = Bezug, Speicher positiv =
// Entladen.

var Clay = require('@rebble/clay');
var clayConfig = require('./config');

// Clay nicht automatisch senden lassen: URL und Token sollen nie auf die Uhr.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var ERR_NONE = 0;
var ERR_CONFIG = 1;
var ERR_AUTH = 2;
var ERR_LINK = 3;
var ERR_NO_SENSORS = 4;
var ERR_NO_WEBSOCKET = 5;
var ERR_HA = 6;

var DEFAULT_RANGE_H = 12;
var DEFAULT_COLS = 160;
var SESSION_TIMEOUT_MS = 20000;
var PREFS_MAX_AGE_MS = 60 * 60 * 1000;   // Energie-Einstellungen stuendlich neu
var STATS_OVERLAP_MS = 15 * 60 * 1000;   // letzte Bloecke immer neu holen
var MAX_GAP_MS = 30 * 60 * 1000;         // groessere Luecken nicht ueberbruecken
var UNIT_W = 10;                         // Uebertragung in 10-W-Schritten
var INT16_MAX = 32767;

var SOURCE_TYPES = ['solar', 'battery', 'grid'];

// -------------------------------------------------------------------
// Einstellungen
// -------------------------------------------------------------------
function readSettings() {
  var stored = {};
  try {
    stored = JSON.parse(localStorage.getItem('clay-settings') || '{}') || {};
  } catch (e) {
    stored = {};
  }
  function text(key) {
    var v = stored[key];
    return String(v === undefined || v === null ? '' : v).trim();
  }
  var range = parseInt(text('range'), 10);
  return {
    url: text('haUrl').replace(/\/+$/, ''),
    token: text('haToken'),
    rangeH: [3, 6, 12, 24].indexOf(range) >= 0 ? range : DEFAULT_RANGE_H,
    stored: stored
  };
}

function timeToMinutes(value, fallback) {
  var m = /^(\d{1,2}):(\d{2})/.exec(String(value || ''));
  if (!m) return fallback;
  return (parseInt(m[1], 10) % 24) * 60 + (parseInt(m[2], 10) % 60);
}

function colorValue(value, fallback) {
  if (typeof value === 'number') return value;
  var n = parseInt(String(value || '').replace(/^#|^0x/i, ''), 16);
  return isNaN(n) ? fallback : n;
}

function settingsMessage() {
  var s = readSettings().stored;
  return {
    DAY_BG: colorValue(s.DAY_BG, 0x000000),
    DAY_FG: colorValue(s.DAY_FG, 0xFFFFFF),
    NIGHT_BG: colorValue(s.NIGHT_BG, 0xFFFFFF),
    NIGHT_FG: colorValue(s.NIGHT_FG, 0x000000),
    NIGHT_MODE: s.NIGHT_MODE ? 1 : 0,
    NIGHT_START: timeToMinutes(s.nightStart, 22 * 60),
    DAY_START: timeToMinutes(s.dayStart, 7 * 60)
  };
}

// -------------------------------------------------------------------
// AppMessage-Warteschlange: immer nur eine Nachricht unterwegs
// -------------------------------------------------------------------
var queue = [];
var sending = false;

function send(msg) {
  queue.push(msg);
  pump();
}

function pump() {
  if (sending || !queue.length) return;
  sending = true;
  var msg = queue[0];
  Pebble.sendAppMessage(msg, function() {
    queue.shift();
    sending = false;
    pump();
  }, function() {
    // Einmal verwerfen statt endlos wiederholen; die naechste Runde kommt.
    queue.shift();
    sending = false;
    pump();
  });
}

function sendError(code) {
  send({ ERROR: code });
}

// -------------------------------------------------------------------
// Zwischenspeicher: nur neue 5-Minuten-Bloecke nachladen
// -------------------------------------------------------------------
var cache = {
  prefs: null,        // { solar: [ids], battery: [ids], grid: [ids] }
  prefsAt: 0,
  statsKey: '',
  stats: {},          // id -> { startMs: { t, v } }
  fetchedUntil: 0
};

function resetStats() {
  cache.statsKey = '';
  cache.stats = {};
  cache.fetchedUntil = 0;
}

function parsePrefs(prefs) {
  // soc: Speicher mit Fuellstandssensor (stat_soc), optional mit nutzbarer
  // Kapazitaet in kWh -- HA gewichtet damit den gemeinsamen Fuellstand.
  var ids = { solar: [], battery: [], grid: [], soc: [] };
  var sources = (prefs && prefs.energy_sources) || [];
  sources.forEach(function(src) {
    if (ids[src.type] && src.stat_rate) ids[src.type].push(src.stat_rate);
    if (src.type === 'battery' && src.stat_soc) {
      ids.soc.push({ id: src.stat_soc, capacity: parseFloat(src.capacity) || 0 });
    }
  });
  return ids;
}

// Gemeinsamer Fuellstand: nach Kapazitaet gewichtet, wenn alle Speicher
// eine haben, sonst einfacher Mittelwert. undefined = nichts zu zeigen.
function combinedSoc(prefs, live) {
  var known = prefs.soc.filter(function(s) { return live[s.id] !== undefined; });
  if (!known.length) return undefined;
  var weighted = known.every(function(s) { return s.capacity > 0; });
  var sum = 0, weight = 0;
  known.forEach(function(s) {
    var w = weighted ? s.capacity : 1;
    sum += live[s.id] * w;
    weight += w;
  });
  return Math.max(0, Math.min(100, Math.round(sum / weight)));
}

function allIds(prefs) {
  var list = [];
  SOURCE_TYPES.forEach(function(type) {
    prefs[type].forEach(function(id) {
      if (list.indexOf(id) < 0) list.push(id);
    });
  });
  return list;
}

function toMs(v) {
  return typeof v === 'number' ? v : Date.parse(v);
}

function mergeStats(result) {
  Object.keys(result || {}).forEach(function(id) {
    var bucket = cache.stats[id] || (cache.stats[id] = {});
    (result[id] || []).forEach(function(p) {
      if (p.mean === null || p.mean === undefined) return;
      var start = toMs(p.start);
      var end = p.end !== undefined ? toMs(p.end) : start + 5 * 60 * 1000;
      bucket[start] = { t: (start + end) / 2, v: p.mean };
    });
  });
}

function pruneStats(windowStart) {
  Object.keys(cache.stats).forEach(function(id) {
    var bucket = cache.stats[id];
    Object.keys(bucket).forEach(function(k) {
      if (bucket[k].t < windowStart - MAX_GAP_MS) delete bucket[k];
    });
  });
}

// -------------------------------------------------------------------
// Verdichten auf Pixelspalten
// -------------------------------------------------------------------
function seriesPoints(id, live, nowMs) {
  var bucket = cache.stats[id] || {};
  var pts = Object.keys(bucket).map(function(k) { return bucket[k]; });
  pts.sort(function(a, b) { return a.t - b.t; });
  if (live[id] !== undefined) {
    // HA haengt fuer "heute" den aktuellen Zustand an; die letzte
    // Statistik ist sonst bis zu zehn Minuten alt.
    while (pts.length && pts[pts.length - 1].t >= nowMs) pts.pop();
    pts.push({ t: nowMs, v: live[id] });
  }
  return pts;
}

function valueAt(pts, t) {
  // Lineare Interpolation zwischen den Nachbarpunkten
  var lo = 0, hi = pts.length - 1;
  if (!pts.length || t < pts[0].t || t > pts[hi].t) return 0;
  while (hi - lo > 1) {
    var mid = (lo + hi) >> 1;
    if (pts[mid].t <= t) lo = mid; else hi = mid;
  }
  var a = pts[lo], b = pts[hi];
  if (b.t === a.t) return a.v;
  if (b.t - a.t > MAX_GAP_MS) return 0;
  return a.v + (b.v - a.v) * (t - a.t) / (b.t - a.t);
}

function columnValues(pts, startMs, endMs, cols) {
  var out = [];
  var span = (endMs - startMs) / cols;
  var j = 0;
  for (var i = 0; i < cols; i++) {
    var c0 = startMs + i * span, c1 = c0 + span;
    // Breite Spalten mitteln, damit kurze Spitzen nicht zufaellig
    // wegfallen; schmale Spalten interpolieren.
    while (j < pts.length && pts[j].t < c0) j++;
    var sum = 0, n = 0;
    for (var k = j; k < pts.length && pts[k].t < c1; k++) {
      sum += pts[k].v;
      n++;
    }
    out.push(n ? sum / n : valueAt(pts, (c0 + c1) / 2));
  }
  return out;
}

function toBytes(values) {
  var bytes = [];
  values.forEach(function(w) {
    var v = Math.round(w / UNIT_W);
    if (v > INT16_MAX) v = INT16_MAX;
    if (v < -INT16_MAX) v = -INT16_MAX;
    if (v < 0) v += 65536;
    bytes.push(v & 0xFF, (v >> 8) & 0xFF);
  });
  return bytes;
}

function buildMessage(prefs, live, rangeH, cols, nowMs) {
  var startMs = nowMs - rangeH * 3600 * 1000;
  var msg = { ERROR: ERR_NONE, RANGE_H: rangeH, END_TIME: Math.floor(nowMs / 1000) };
  var keys = { solar: 'SOLAR', battery: 'BATTERY', grid: 'GRID' };
  SOURCE_TYPES.forEach(function(type) {
    if (!prefs[type].length) return;
    var sum = null;
    prefs[type].forEach(function(id) {
      var vals = columnValues(seriesPoints(id, live, nowMs), startMs, nowMs, cols);
      if (!sum) { sum = vals; return; }
      for (var i = 0; i < cols; i++) sum[i] += vals[i];
    });
    msg[keys[type]] = toBytes(sum);
  });
  return msg;
}

// -------------------------------------------------------------------
// Aktueller Zustand aus subscribe_entities (komprimiertes Format)
// -------------------------------------------------------------------
function powerInWatts(state, unit) {
  var v = parseFloat(state);
  if (isNaN(v)) return undefined;
  switch (String(unit || 'W')) {
    case 'kW': return v * 1000;
    case 'MW': return v * 1000000;
    case 'mW': return v / 1000;
    default: return v;
  }
}

// -------------------------------------------------------------------
// WebSocket-Sitzung
// -------------------------------------------------------------------
var busy = false;

function wsUrl(base) {
  return base.replace(/^http/i, 'ws') + '/api/websocket';
}

function fetchAndSend() {
  var settings = readSettings();
  if (!settings.url || !settings.token) { sendError(ERR_CONFIG); return; }
  if (typeof WebSocket === 'undefined') { sendError(ERR_NO_WEBSOCKET); return; }
  if (busy) return;
  busy = true;

  var cols = parseInt(localStorage.getItem('cols'), 10) || DEFAULT_COLS;
  var rangeH = settings.rangeH;
  var ws;
  var nextId = 1;
  var handlers = {};
  var done = false;

  var timer = setTimeout(function() { finish(ERR_LINK); }, SESSION_TIMEOUT_MS);

  function finish(err) {
    if (done) return;
    done = true;
    busy = false;
    clearTimeout(timer);
    try { ws.close(); } catch (e) { /* schon zu */ }
    if (err !== undefined) sendError(err);
  }

  function call(msg, cb) {
    msg.id = nextId++;
    handlers[msg.id] = cb;
    ws.send(JSON.stringify(msg));
    return msg.id;
  }

  function withPrefs(next) {
    if (cache.prefs && Date.now() - cache.prefsAt < PREFS_MAX_AGE_MS) { next(cache.prefs); return; }
    call({ type: 'energy/get_prefs' }, function(m) {
      if (!m.success) {
        // Energie-Dashboard nie eingerichtet: HA meldet "not_found".
        finish(m.error && m.error.code === 'not_found' ? ERR_NO_SENSORS : ERR_HA);
        return;
      }
      cache.prefs = parsePrefs(m.result);
      cache.prefsAt = Date.now();
      next(cache.prefs);
    });
  }

  function run() {
    withPrefs(function(prefs) {
      var ids = allIds(prefs);
      if (!ids.length) { finish(ERR_NO_SENSORS); return; }

      var nowMs = Date.now();
      var windowStart = nowMs - rangeH * 3600 * 1000;
      var key = ids.join(',') + '|' + rangeH;
      if (cache.statsKey !== key) {
        resetStats();
        cache.statsKey = key;
      }
      var from = Math.max(windowStart - STATS_OVERLAP_MS, cache.fetchedUntil - STATS_OVERLAP_MS);

      call({
        type: 'recorder/statistics_during_period',
        start_time: new Date(from).toISOString(),
        end_time: new Date(nowMs).toISOString(),
        statistic_ids: ids,
        period: '5minute',
        types: ['mean'],
        units: { power: 'W' }
      }, function(m) {
        if (!m.success) { finish(ERR_HA); return; }
        mergeStats(m.result);
        pruneStats(windowStart);
        cache.fetchedUntil = nowMs;

        // Aktuelle Werte: subscribe_entities liefert zuerst ein Ereignis
        // mit dem vollen Zustand genau dieser Entitaeten.
        var live = {};
        var socIds = prefs.soc.map(function(s) { return s.id; });
        call({ type: 'subscribe_entities', entity_ids: ids.concat(socIds) }, function(r) {
          if (r.type === 'result') {
            if (!r.success) complete();   // aeltere HA-Version: ohne Livewert
            return;
          }
          var added = (r.event && r.event.a) || {};
          Object.keys(added).forEach(function(id) {
            var st = added[id];
            if (socIds.indexOf(id) >= 0) {
              var pct = parseFloat(st.s);
              if (!isNaN(pct)) live[id] = pct;
              return;
            }
            var w = powerInWatts(st.s, st.a && st.a.unit_of_measurement);
            if (w !== undefined) live[id] = w;
          });
          complete();
        });

        function complete() {
          if (done) return;
          var msg = buildMessage(prefs, live, rangeH, cols, nowMs);
          var soc = combinedSoc(prefs, live);
          msg.SOC = soc === undefined ? -1 : soc;   // -1: kein Balken
          finish();
          send(msg);
        }
      });
    });
  }

  try {
    ws = new WebSocket(wsUrl(settings.url));
  } catch (e) {
    finish(ERR_LINK);
    return;
  }

  ws.onmessage = function(ev) {
    var m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.type === 'auth_required') {
      ws.send(JSON.stringify({ type: 'auth', access_token: settings.token }));
    } else if (m.type === 'auth_invalid') {
      finish(ERR_AUTH);
    } else if (m.type === 'auth_ok') {
      run();
    } else if ((m.type === 'result' || m.type === 'event') && handlers[m.id]) {
      var cb = handlers[m.id];
      if (m.type === 'result' && m.success === false) delete handlers[m.id];
      cb(m);
    }
  };
  ws.onerror = function() { finish(ERR_LINK); };
  ws.onclose = function() { finish(ERR_LINK); };
}

// -------------------------------------------------------------------
// Ereignisse
// -------------------------------------------------------------------
Pebble.addEventListener('ready', function() {
  send(settingsMessage());
  send({ JS_READY: 1 });
});

Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload || {};
  if (p.COLS !== undefined) {
    localStorage.setItem('cols', String(p.COLS));
    fetchAndSend();
  }
});

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response || e.response === 'CANCELLED') return;
  try {
    clay.getSettings(e.response, false);   // legt clay-settings ab
  } catch (err) {
    return;
  }
  cache.prefs = null;
  resetStats();
  send(settingsMessage());
  fetchAndSend();
});
