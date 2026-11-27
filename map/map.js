// The JinxMesh map browser: draws the vector maps (.jmv, "JMVM" version 4) straight from the map
// repositories, a few cells at a time by byte range, and cuts a chosen area out as a new .jmv file.
// The format is described in docs/MAPS.md of the project; the drawing follows the device's own.

const INDEX = "../maps/index.json";
const EXTENT = 4096, TILE = 256;
// cell zoom, first and last tile zoom of each level, as the builder makes them
const LEVELS = [[13, 13, 17], [11, 11, 12], [9, 9, 10], [7, 7, 8]];
const MIN_DATA_ZOOM = 7, MAX_ZOOM = 18, MIN_ZOOM = 3;
const LIMIT = 48 * 1000 * 1000; // what a piece may weigh: larger files are not for the device

const C = {
  ground: "#E9DEC3", contour: "#C2BAA8", contourIndex: "#9F9888", contourMain: "#7B756A", water: "#A9CFF0",
  waterLine: "#2F78BF", motorway: "#C0392B", primary: "#D35400", secondary: "#1E1E1E", minor: "#444444",
  service: "#6E6E6E", track: "#8D6E3F", path: "#2E7D32", rail: "#111111", boundary: "#8E44AD", text: "#111111",
  halo: "#E9DEC3", nothing: "#d9dee6",
};
const K = { Contour20: 1, Contour100: 2, Contour500: 3, Stream: 10, River: 11, Water: 12, Coast: 13, Motorway: 20,
  Primary: 21, Secondary: 22, Minor: 23, Service: 24, Track: 25, Path: 26, Rail: 27, Boundary: 30, StateLine: 31,
  NationLine: 32, City: 64, Town: 65, Village: 66, Peak: 67, RoadName: 68, WaterName: 69, ContourName: 70 };

const layers = { contours: true, water: true, roads: true, trails: true, boundaries: true, names: true };

// color, width and dashes of a kind of line at a zoom; null: not drawn there
function lookOf(kind, z) {
  const L = layers;
  switch (kind) {
    case K.Contour20: return L.contours && z >= 15 ? [C.contour, 1] : null;
    case K.Contour100: return L.contours && z >= 13 ? [z >= 15 ? C.contourIndex : C.contour, 1] : null;
    case K.Contour500: return L.contours && z >= 11 ? [z >= 13 ? C.contourMain : C.contourIndex, z >= 15 ? 2 : 1] : null;
    case K.Stream: return L.water && z >= 13 ? [C.waterLine, 1] : null;
    case K.River: return L.water ? [C.waterLine, z >= 12 ? 2 : 1] : null;
    case K.Water: return L.water ? [C.water, 1] : null;
    case K.Coast: return L.water ? [C.waterLine, z >= 9 ? 2 : 1] : null;
    case K.StateLine: return L.boundaries ? [C.boundary, z >= 9 ? 2 : 1, 8, 3] : null;
    case K.NationLine: return L.boundaries ? [C.boundary, z >= 9 ? 3 : 2, 10, 3] : null;
    case K.Motorway: return L.roads ? [C.motorway, z >= 11 ? 3 : 2] : null;
    case K.Primary: return L.roads ? [C.primary, z >= 13 ? 3 : z >= 10 ? 2 : 1] : null;
    case K.Secondary: return L.roads ? [C.secondary, z >= 13 ? 2 : 1] : null;
    case K.Minor: return L.roads && z >= 12 ? [C.minor, z >= 16 ? 2 : 1] : null;
    case K.Service: return L.roads && z >= 15 ? [C.service, 1] : null;
    case K.Track: return L.trails && z >= 13 ? [C.track, 1, 5, 2] : null;
    case K.Path: return L.trails && z >= 13 ? [C.path, z >= 15 ? 2 : 1, 3, 2] : null;
    case K.Rail: return L.roads && z >= 10 ? [C.rail, 1, 6, 3] : null;
    case K.Boundary: return L.boundaries && z >= 10 ? [C.boundary, 1, 6, 2] : null;
  }
  return null;
}

// ---------- places

const mercX = lon => (lon + 180) / 360;
const mercY = lat => (1 - Math.asinh(Math.tan(lat * Math.PI / 180)) / Math.PI) / 2;
const lonOf = x => x * 360 - 180;
const latOf = y => Math.atan(Math.sinh(Math.PI * (1 - 2 * y))) * 180 / Math.PI;

// ---------- reading

async function range(url, start, length) {
  const response = await fetch(url, { headers: { Range: `bytes=${start}-${start + length - 1}` } });
  if (response.status !== 206 && response.status !== 200) throw new Error(`${response.status} for ${url}`);
  let data = await response.arrayBuffer();
  if (response.status === 200) data = data.slice(start, start + length); // a server that sent it all
  if (data.byteLength < length && response.status === 206 && start === 0) return data; // a file shorter than asked
  if (data.byteLength !== length) throw new Error(`short answer for ${url}`);
  return data;
}

class Piece {
  constructor(map, piece) {
    this.url = map.base + piece.file;
    this.file = piece.file;
    this.title = map.title;
    this.bytes = piece.bytes;
    this.x0 = mercX(piece.west); this.x1 = mercX(piece.east);
    this.y0 = mercY(piece.north); this.y1 = mercY(piece.south);
    this.head = null;
    this.tables = {};
  }
  // header and levels
  open() {
    if (!this.head) {
      this.head = range(this.url, 0, 256).then(buffer => {
        const view = new DataView(buffer);
        if (String.fromCharCode(...new Uint8Array(buffer, 0, 4)) !== "JMVM" || view.getUint16(4, true) !== 4)
          throw new Error(`${this.file} is no map of version 4`);
        const count = view.getUint16(6, true), levels = [];
        for (let i = 0; i < count; i++) {
          const at = 28 + 20 * i;
          levels.push({ zoom: view.getUint8(at), first: view.getUint8(at + 1), last: view.getUint8(at + 2),
            x0: view.getUint32(at + 4, true), y0: view.getUint32(at + 8, true), cols: view.getUint16(at + 12, true),
            rows: view.getUint16(at + 14, true), where: view.getUint32(at + 16, true) });
        }
        return { low: view.getUint8(24), high: view.getUint8(25), levels };
      });
      this.head.catch(() => { this.head = null; });
    }
    return this.head;
  }
  // the directory of the level of that cell zoom: {level, view} or null
  table(zoom) {
    if (!this.tables[zoom]) {
      this.tables[zoom] = this.open().then(async head => {
        const level = head.levels.find(l => l.zoom === zoom);
        if (!level) return null;
        return { level, view: new DataView(await range(this.url, level.where, 12 * level.cols * level.rows)) };
      });
      this.tables[zoom].catch(() => { delete this.tables[zoom]; });
    }
    return this.tables[zoom];
  }
  static entry(table, cx, cy) {
    if (!table) return null;
    const l = table.level;
    if (cx < l.x0 || cy < l.y0 || cx >= l.x0 + l.cols || cy >= l.y0 + l.rows) return null;
    const at = 12 * ((cy - l.y0) * l.cols + (cx - l.x0));
    const lines = table.view.getUint32(at + 4, true), names = table.view.getUint32(at + 8, true);
    return lines + names ? { offset: table.view.getUint32(at, true), lines, names } : null;
  }
}

let pieces = [], maps = [];

// the pieces that may hold a cell: those whose land, and a cell around it, it lies in
function piecesAt(zoom, cx, cy) {
  const n = 2 ** zoom, pad = 1.5 / n;
  const x0 = cx / n, x1 = (cx + 1) / n, y0 = cy / n, y1 = (cy + 1) / n;
  return pieces.filter(p => p.x1 + pad > x0 && p.x0 - pad < x1 && p.y1 + pad > y0 && p.y0 - pad < y1);
}

// Where a cell is: [{piece, offset, lines, names}], empty when no map has it. In the finest level a cell
// along a border holds the land of both states, so one map's cell is all of it. The coarser levels are made
// of a state's own land alone: there every state that has the cell adds its part (the pieces of one state
// all carry the same).
function sourcesOf(zoom, cx, cy, tables) {
  const sources = [], states = new Set();
  for (const piece of piecesAt(zoom, cx, cy)) {
    if (states.has(piece.title)) continue;
    const entry = Piece.entry(tables.get(piece), cx, cy);
    if (!entry) continue;
    sources.push({ piece, ...entry });
    if (zoom === LEVELS[0][0]) break;
    states.add(piece.title);
  }
  return sources;
}

async function findCell(zoom, cx, cy) {
  const candidates = piecesAt(zoom, cx, cy);
  const tables = new Map(await Promise.all(candidates.map(async p => [p, await p.table(zoom).catch(() => null)])));
  return sourcesOf(zoom, cx, cy, tables);
}

function decode(buffer, linesLength) {
  const bytes = new Uint8Array(buffer);
  let at = 0;
  const varint = () => {
    let value = 0, shift = 0, b;
    do { b = bytes[at++]; value += (b & 127) * 2 ** shift; shift += 7; } while (b & 128 && at < bytes.length);
    return value;
  };
  const zigzag = () => { const v = varint(); return v % 2 ? -(v + 1) / 2 : v / 2; };
  const features = [], names = [];
  while (at < linesLength) {
    const kind = bytes[at++];
    const height = kind >= 1 && kind <= 3 ? varint() : 0;
    const count = varint();
    if (count > 100000 || kind >= 64) break;
    const points = new Int32Array(count * 2);
    let x = 0, y = 0, x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (let i = 0; i < count; i++) {
      x += zigzag(); y += zigzag();
      points[2 * i] = x; points[2 * i + 1] = y;
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    features.push({ kind, height, points, x0, y0, x1, y1 });
  }
  at = linesLength;
  const text = new TextDecoder();
  while (at < bytes.length) {
    const kind = bytes[at++];
    if (kind < 64) break;
    const x = zigzag(), y = zigzag(), from = bytes[at++], to = bytes[at++];
    const angle = bytes[at] > 127 ? bytes[at] - 256 : bytes[at]; at++;
    const number = varint(), length = bytes[at++];
    names.push({ kind, x, y, from, to, angle, number, text: text.decode(bytes.subarray(at, at + length)) });
    at += length;
  }
  return { features, names };
}

// ---------- cells and tiles, kept for a while

const cells = new Map(); // "zoom/cx/cy" -> {state: "loading" | "ready" | "none" | "failed", features, names}
const tiles = new Map(); // "z/x/y" -> canvas
const MOST_CELLS = 160, MOST_TILES = 140;

function keep(map, most) {
  while (map.size > most) map.delete(map.keys().next().value);
}

function cellOf(zoom, cx, cy) {
  const key = `${zoom}/${cx}/${cy}`;
  let cell = cells.get(key);
  if (cell) {
    cells.delete(key); cells.set(key, cell); // the latest asked for last
    return cell;
  }
  cell = { state: "loading" };
  cells.set(key, cell);
  keep(cells, MOST_CELLS);
  (async () => {
    try {
      const found = await findCell(zoom, cx, cy);
      if (!found.length) { cell.state = "none"; return; }
      const parts = await Promise.all(found.map(async f => decode(await range(f.piece.url, f.offset, f.lines + f.names), f.lines)));
      cell.features = parts.flatMap(p => p.features);
      cell.names = parts.flatMap(p => p.names);
      cell.state = "ready";
    } catch (error) {
      cell.state = "failed";
      console.warn(`cell ${key}: ${error.message}`);
      setTimeout(() => { if (cells.get(key) === cell) cells.delete(key); draw(); }, 4000);
    }
  })().finally(draw);
  return cell;
}

function levelOf(z) {
  for (const [zoom, first, last] of LEVELS) if (z >= first && z <= last) return zoom;
  return z > LEVELS[0][2] ? LEVELS[0][0] : 0;
}

const ratio = Math.min(2, window.devicePixelRatio || 1);

function renderTile(z, x, y, cell) {
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = TILE * ratio;
  const g = canvas.getContext("2d");
  g.scale(ratio, ratio);
  g.fillStyle = C.ground;
  g.fillRect(0, 0, TILE, TILE);
  const zoom = levelOf(z), shift = z - zoom, per = EXTENT >> shift; // units across the tile
  const ox = (x - ((x >> shift) << shift)) * per, oy = (y - ((y >> shift) << shift)) * per;
  const scale = TILE / per, pad = 6 / scale;
  g.lineJoin = g.lineCap = "round";
  let kind = -1, look = null, open = false;
  const stroke = () => { if (open) { g.stroke(); open = false; } };
  for (const f of cell.features) {
    if (f.x1 < ox - pad || f.y1 < oy - pad || f.x0 > ox + per + pad || f.y0 > oy + per + pad) continue;
    if (f.kind !== kind) {
      stroke();
      kind = f.kind;
      look = lookOf(kind, Math.min(z, 17));
      if (look) {
        g.strokeStyle = g.fillStyle = look[0];
        g.lineWidth = look[1] + 0.4;
        g.setLineDash(look[2] ? [look[2], look[3]] : []);
      }
    }
    if (!look) continue;
    const p = f.points;
    if (kind === K.Water) {
      g.beginPath();
      for (let i = 0; i < p.length; i += 2) g[i ? "lineTo" : "moveTo"]((p[i] - ox) * scale, (p[i + 1] - oy) * scale);
      g.closePath();
      g.fill("evenodd");
      g.stroke();
      continue;
    }
    if (!open) { g.beginPath(); open = true; }
    for (let i = 0; i < p.length; i += 2) g[i ? "lineTo" : "moveTo"]((p[i] - ox) * scale, (p[i + 1] - oy) * scale);
  }
  stroke();
  return canvas;
}

// ---------- the view

const canvas = document.getElementById("map"), ctx = canvas.getContext("2d");
const view = { x: mercX(-96), y: mercY(42), z: 4 };
let wide = 0, high = 0, chosen = null, choosing = false, pending = false;

function resize() {
  wide = canvas.clientWidth; high = canvas.clientHeight;
  canvas.width = wide * ratio; canvas.height = high * ratio;
  draw();
}

function draw() {
  if (pending) return;
  pending = true;
  // a frame, or soon after when the browser hands none out (a tab in the background)
  const run = () => { if (pending) { pending = false; paint(); } };
  requestAnimationFrame(run);
  setTimeout(run, 120);
}

function paint() {
  const z = view.z, world = TILE * 2 ** z;
  const left = view.x * world - wide / 2, top = view.y * world - high / 2;
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.fillStyle = C.nothing;
  ctx.fillRect(0, 0, wide, high);
  let waiting = 0;
  if (z >= MIN_DATA_ZOOM && pieces.length) {
    const zoom = levelOf(z), shift = z - zoom, n = 2 ** z;
    const tx0 = Math.floor(left / TILE), tx1 = Math.floor((left + wide) / TILE);
    const ty0 = Math.max(0, Math.floor(top / TILE)), ty1 = Math.min(n - 1, Math.floor((top + high) / TILE));
    for (let ty = ty0; ty <= ty1; ty++)
      for (let tx = tx0; tx <= tx1; tx++) {
        if (tx < 0 || tx >= n) continue;
        const key = `${z}/${tx}/${ty}`;
        let tile = tiles.get(key);
        if (!tile) {
          const cell = cellOf(zoom, tx >> shift, ty >> shift);
          if (cell.state === "loading") waiting++;
          if (cell.state !== "ready") continue;
          tile = renderTile(z, tx, ty, cell);
          tiles.set(key, tile);
          keep(tiles, MOST_TILES);
        }
        ctx.drawImage(tile, Math.round(tx * TILE - left), Math.round(ty * TILE - top), TILE, TILE);
      }
    if (layers.names) paintNames(z, zoom, left, top);
  } else if (z < MIN_DATA_ZOOM) {
    paintCover(world, left, top);
  }
  if (chosen) {
    const x = chosen.x0 * world - left, y = chosen.y0 * world - top;
    const w = (chosen.x1 - chosen.x0) * world, h = (chosen.y1 - chosen.y0) * world;
    ctx.fillStyle = "rgba(27, 118, 214, .14)";
    ctx.fillRect(x, y, w, h);
    ctx.strokeStyle = "#1b76d6";
    ctx.lineWidth = 2;
    ctx.setLineDash([]);
    ctx.strokeRect(x, y, w, h);
  }
  const note = document.getElementById("note");
  note.textContent = z < MIN_DATA_ZOOM ? "Zoom in to see the map. The boxes are the map pieces there are." : waiting ? "Loading" : "";
  note.hidden = !note.textContent;
  document.getElementById("place").textContent = `${latOf(view.y).toFixed(5)}, ${lonOf(view.x).toFixed(5)}  zoom ${z}`;
  const hash = `#${z}/${latOf(view.y).toFixed(5)}/${lonOf(view.x).toFixed(5)}`;
  if (location.hash !== hash) history.replaceState(null, "", hash);
}

// below the zooms the maps hold: where there is a map at all
function paintCover(world, left, top) {
  ctx.setLineDash([]);
  ctx.lineWidth = 1;
  for (const p of pieces) {
    const x = p.x0 * world - left, y = p.y0 * world - top, w = (p.x1 - p.x0) * world, h = (p.y1 - p.y0) * world;
    if (x > wide || y > high || x + w < 0 || y + h < 0) continue;
    ctx.fillStyle = C.ground;
    ctx.fillRect(x, y, w, h);
  }
  ctx.strokeStyle = "rgba(60, 60, 60, .28)";
  for (const p of pieces) {
    const x = p.x0 * world - left, y = p.y0 * world - top, w = (p.x1 - p.x0) * world, h = (p.y1 - p.y0) * world;
    if (x > wide || y > high || x + w < 0 || y + h < 0) continue;
    ctx.strokeRect(Math.round(x) + 0.5, Math.round(y) + 0.5, Math.round(w), Math.round(h));
  }
  if (view.z < 4) return;
  ctx.font = "bold 13px 'Liberation Sans', Arial, Helvetica, sans-serif";
  ctx.textAlign = "center"; ctx.textBaseline = "middle";
  const taken = [];
  for (const m of maps) {
    const x = (m.x0 + m.x1) / 2 * world - left, y = (m.y0 + m.y1) / 2 * world - top;
    if (x < -100 || y < -20 || x > wide + 100 || y > high + 20) continue;
    const half = ctx.measureText(m.title).width / 2 + 4;
    if (taken.some(t => Math.abs(t[0] - x) < t[2] + half && Math.abs(t[1] - y) < 16)) continue; // no room: it shows a zoom further in
    taken.push([x, y, half]);
    label(m.title, x, y, C.text);
  }
}

function label(text, x, y, color) {
  ctx.lineJoin = "round";
  ctx.lineWidth = 3;
  ctx.strokeStyle = C.halo;
  ctx.setLineDash([]);
  ctx.strokeText(text, x, y);
  ctx.fillStyle = color;
  ctx.fillText(text, x, y);
}

function paintNames(z, zoom, left, top) {
  const shift = z - zoom, cellPx = TILE * 2 ** shift, reach = 110, zz = Math.min(z, 17);
  const cx0 = Math.floor((left - reach) / cellPx), cx1 = Math.floor((left + wide + reach) / cellPx);
  const cy0 = Math.floor((top - reach) / cellPx), cy1 = Math.floor((top + high + reach) / cellPx);
  const small = "bold 11px 'Liberation Sans', Arial, Helvetica, sans-serif";
  const large = "bold 14px 'Liberation Sans', Arial, Helvetica, sans-serif";
  ctx.textAlign = "center"; ctx.textBaseline = "middle";
  for (let cy = cy0; cy <= cy1; cy++)
    for (let cx = cx0; cx <= cx1; cx++) {
      const cell = cells.get(`${zoom}/${cx}/${cy}`);
      if (!cell || cell.state !== "ready") continue;
      for (const name of cell.names) {
        if (zz < name.from || zz > name.to || (name.kind === K.ContourName && !layers.contours)) continue;
        const x = (cx + name.x / EXTENT) * cellPx - left, y = (cy + name.y / EXTENT) * cellPx - top;
        if (x < -reach || y < -reach || x > wide + reach || y > high + reach) continue;
        const ink = name.kind === K.WaterName ? C.waterLine : C.text;
        const big = (name.kind === K.City || name.kind === K.Town) && zz >= 10;
        ctx.font = big ? large : small;
        if (name.kind === K.ContourName || (name.angle !== 0 && !big)) {
          ctx.save();
          ctx.translate(x, y);
          ctx.rotate(name.angle / 256 * 2 * Math.PI);
          label(name.kind === K.ContourName ? String(name.number) : name.text, 0, 0, name.kind === K.ContourName ? C.contourMain : ink);
          ctx.restore();
        } else if (name.kind === K.Peak) {
          ctx.fillStyle = ink;
          ctx.beginPath(); ctx.moveTo(x, y - 3); ctx.lineTo(x + 4, y + 2); ctx.lineTo(x - 4, y + 2); ctx.fill();
          label(name.text + (name.number && zz >= 14 ? ` ${name.number}'` : ""), x, y + 10, ink);
        } else {
          label(name.text, x, y, ink);
        }
      }
    }
}

// ---------- moving about

function setZoom(z, px, py) {
  z = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, z));
  if (z === view.z) return;
  // the place under the pointer stays there
  const before = TILE * 2 ** view.z, after = TILE * 2 ** z;
  const mx = view.x + (px - wide / 2) / before, my = view.y + (py - high / 2) / before;
  view.x = mx - (px - wide / 2) / after;
  view.y = my - (py - high / 2) / after;
  view.z = z;
  clamp();
  draw();
}

function clamp() {
  view.x = Math.max(0, Math.min(1, view.x));
  view.y = Math.max(0.02, Math.min(0.98, view.y));
}

function show(x0, y0, x1, y1) {
  view.x = (x0 + x1) / 2; view.y = (y0 + y1) / 2;
  let z = MAX_ZOOM;
  while (z > MIN_ZOOM && ((x1 - x0) * TILE * 2 ** z > wide * 0.9 || (y1 - y0) * TILE * 2 ** z > high * 0.9)) z--;
  view.z = z;
  clamp();
  draw();
}

const pointers = new Map();
let drag = null, pinch = 0, wheel = 0;

function spot(event) {
  const box = canvas.getBoundingClientRect();
  return [event.clientX - box.left, event.clientY - box.top];
}

function placeAt(px, py) {
  const world = TILE * 2 ** view.z;
  return [view.x + (px - wide / 2) / world, view.y + (py - high / 2) / world];
}

canvas.addEventListener("pointerdown", event => {
  canvas.setPointerCapture(event.pointerId);
  const [px, py] = spot(event);
  pointers.set(event.pointerId, [px, py]);
  if (pointers.size === 2) {
    const [a, b] = [...pointers.values()];
    pinch = Math.hypot(a[0] - b[0], a[1] - b[1]);
    drag = null;
  } else if (choosing) {
    const [x, y] = placeAt(px, py);
    drag = { choose: [x, y] };
    chosen = { x0: x, y0: y, x1: x, y1: y };
  } else {
    drag = { px, py, x: view.x, y: view.y };
  }
});

canvas.addEventListener("pointermove", event => {
  if (!pointers.has(event.pointerId)) return;
  const [px, py] = spot(event);
  pointers.set(event.pointerId, [px, py]);
  if (pointers.size === 2 && pinch) {
    const [a, b] = [...pointers.values()];
    const now = Math.hypot(a[0] - b[0], a[1] - b[1]);
    if (now > pinch * 1.6 || now < pinch / 1.6) {
      setZoom(view.z + (now > pinch ? 1 : -1), (a[0] + b[0]) / 2, (a[1] + b[1]) / 2);
      pinch = now;
    }
  } else if (drag && drag.choose) {
    const [x, y] = placeAt(px, py);
    chosen = { x0: Math.min(x, drag.choose[0]), y0: Math.min(y, drag.choose[1]), x1: Math.max(x, drag.choose[0]), y1: Math.max(y, drag.choose[1]) };
    draw();
  } else if (drag) {
    const world = TILE * 2 ** view.z;
    view.x = drag.x - (px - drag.px) / world;
    view.y = drag.y - (py - drag.py) / world;
    clamp();
    draw();
  }
});

function release(event) {
  pointers.delete(event.pointerId);
  if (pointers.size < 2) pinch = 0;
  if (drag && drag.choose) {
    const world = TILE * 2 ** view.z;
    if ((chosen.x1 - chosen.x0) * world < 8 || (chosen.y1 - chosen.y0) * world < 8) chosen = null;
    setChoosing(false);
    measure();
  }
  drag = null;
  draw();
}
canvas.addEventListener("pointerup", release);
canvas.addEventListener("pointercancel", release);

canvas.addEventListener("wheel", event => {
  event.preventDefault();
  const now = Date.now();
  if (now - wheel < 180) return;
  wheel = now;
  const [px, py] = spot(event);
  setZoom(view.z + (event.deltaY < 0 ? 1 : -1), px, py);
}, { passive: false });

canvas.addEventListener("dblclick", event => {
  const [px, py] = spot(event);
  setZoom(view.z + 1, px, py);
});

canvas.addEventListener("keydown", event => {
  const step = 80 / (TILE * 2 ** view.z);
  const moves = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] };
  if (moves[event.key]) { view.x += moves[event.key][0]; view.y += moves[event.key][1]; clamp(); draw(); }
  else if (event.key === "+" || event.key === "=") setZoom(view.z + 1, wide / 2, high / 2);
  else if (event.key === "-") setZoom(view.z - 1, wide / 2, high / 2);
  else return;
  event.preventDefault();
});

document.getElementById("in").onclick = () => setZoom(view.z + 1, wide / 2, high / 2);
document.getElementById("out").onclick = () => setZoom(view.z - 1, wide / 2, high / 2);

for (const box of document.querySelectorAll("#layers input")) {
  box.onchange = () => { layers[box.value] = box.checked; tiles.clear(); draw(); };
}

document.getElementById("goto").onchange = event => {
  const m = maps[event.target.value];
  if (m) show(m.x0, m.y0, m.x1, m.y1);
  event.target.value = "";
};

document.getElementById("find").onsubmit = event => {
  event.preventDefault();
  const numbers = document.getElementById("where").value.match(/-?\d+(\.\d+)?/g);
  if (!numbers || numbers.length < 2 || Math.abs(numbers[0]) > 85 || Math.abs(numbers[1]) > 180) {
    say("Type a latitude and a longitude, like 44.26, -72.58.");
    return;
  }
  view.x = mercX(+numbers[1]); view.y = mercY(+numbers[0]);
  if (view.z < 13) view.z = 13;
  draw();
};

// ---------- cutting an area out

const chooseButton = document.getElementById("choose"), saveButton = document.getElementById("save");
const clearButton = document.getElementById("clear"), status = document.getElementById("status");
let plan = null, counted = 0;

function say(text) { status.textContent = text; }

function setChoosing(on) {
  choosing = on;
  canvas.classList.toggle("choosing", on);
  chooseButton.textContent = on ? "Drag a box on the map" : "Choose an area";
}

chooseButton.onclick = () => { setChoosing(!choosing); if (choosing) say("Drag a box around the land you want."); };
clearButton.onclick = () => { chosen = null; plan = null; saveButton.disabled = clearButton.disabled = true; say(""); draw(); };

// the cells an area takes at each level: those the area lies in and, in the finest level, the cells around
// them, whose names a tile at the edge shows (as split.py does for a piece; the coarse cells are far too
// large to take a ring of them along)
async function measure() {
  plan = null;
  saveButton.disabled = true;
  clearButton.disabled = !chosen;
  if (!chosen) { say(""); return; }
  const mine = ++counted;
  say("Counting what is there.");
  const levels = [];
  let total = 0, core = null, most = 0;
  try {
    for (const [zoom, first, last] of LEVELS) {
      const n = 2 ** zoom;
      const cx0 = Math.floor(chosen.x0 * n), cx1 = Math.floor(chosen.x1 * n);
      const cy0 = Math.floor(chosen.y0 * n), cy1 = Math.floor(chosen.y1 * n);
      const rim = zoom === LEVELS[0][0] ? 1 : 0;
      most += (cx1 - cx0 + 3) * (cy1 - cy0 + 3);
      if (most > 60000) throw new Error("big");
      // the directories of every piece that may hold some of it, then cell by cell
      const near = new Set();
      for (let cy = cy0 - rim; cy <= cy1 + rim; cy++)
        for (let cx = cx0 - rim; cx <= cx1 + rim; cx++) for (const p of piecesAt(zoom, cx, cy)) near.add(p);
      const tables = new Map(await Promise.all([...near].map(async p => [p, await p.table(zoom).catch(() => null)])));
      if (mine !== counted) return;
      const found = new Map();
      for (let cy = cy0 - rim; cy <= cy1 + rim; cy++)
        for (let cx = cx0 - rim; cx <= cx1 + rim; cx++) {
          const sources = sourcesOf(zoom, cx, cy, tables);
          if (!sources.length) continue;
          const cell = { cx, cy, sources, lines: sources.reduce((n, s) => n + s.lines, 0), names: sources.reduce((n, s) => n + s.names, 0) };
          found.set(`${cx}/${cy}`, cell);
          total += cell.lines + cell.names;
        }
      if (zoom === LEVELS[0][0]) {
        const inside = [...found.values()].filter(c => c.cx >= cx0 && c.cx <= cx1 && c.cy >= cy0 && c.cy <= cy1);
        if (inside.length) core = { x0: Math.min(...inside.map(c => c.cx)), x1: Math.max(...inside.map(c => c.cx)),
          y0: Math.min(...inside.map(c => c.cy)), y1: Math.max(...inside.map(c => c.cy)) };
      }
      levels.push({ zoom, first, last, cells: [...found.values()], x0: cx0, y0: cy0 });
    }
  } catch (error) {
    say(error.message === "big" ? "That area is far too large for one file. Choose a smaller one, or take whole pieces on the flasher page."
      : "Could not read the maps there. Check the connection and choose the area again.");
    return;
  }
  if (!core) { say("No map there: the area holds no land that is mapped."); return; }
  const size = total + 28 + 20 * levels.length;
  if (size > LIMIT) {
    say(`${(size / 1e6).toFixed(0)} MB: over the 48 MB a map file for the device may have. Choose a smaller area, or take whole pieces on the flasher page.`);
    return;
  }
  plan = { levels, core, total };
  saveButton.disabled = false;
  say(`${(size / 1e6).toFixed(size < 1e6 ? 2 : 1)} MB, all zooms, ${levels[0].cells.length} cells of the finest level.`);
}

// reads many cells with few requests: neighbors in a file are fetched together
async function fetchCells(wanted, progress) {
  const byPiece = new Map();
  for (const c of wanted) {
    if (!byPiece.has(c.piece)) byPiece.set(c.piece, []);
    byPiece.get(c.piece).push(c);
  }
  const jobs = [];
  for (const [piece, list] of byPiece) {
    list.sort((a, b) => a.offset - b.offset);
    let run = null;
    for (const c of list) {
      const end = c.offset + c.lines + c.names;
      if (run && c.offset - run.end <= 65536 && end - run.start <= 6e6) { run.end = Math.max(run.end, end); run.cells.push(c); }
      else jobs.push(run = { piece, start: c.offset, end, cells: [c] });
    }
  }
  let done = 0, next = 0;
  const all = wanted.reduce((sum, c) => sum + c.lines + c.names, 0);
  const worker = async () => {
    while (next < jobs.length) {
      const job = jobs[next++];
      let data = null;
      for (let attempt = 0; !data; attempt++) {
        try { data = await range(job.piece.url, job.start, job.end - job.start); }
        catch (error) { if (attempt >= 3) throw error; await new Promise(r => setTimeout(r, 1500 * (attempt + 1))); }
      }
      for (const c of job.cells) {
        c.data = new Uint8Array(data, c.offset - job.start, c.lines + c.names);
        done += c.data.length;
      }
      progress(done / all);
    }
  };
  await Promise.all(Array.from({ length: 5 }, worker));
}

saveButton.onclick = async () => {
  if (!plan) return;
  const mine = plan;
  saveButton.disabled = chooseButton.disabled = true;
  try {
    const all = mine.levels.flatMap(l => l.cells);
    await fetchCells(all.flatMap(c => c.sources), part => say(`Fetching the map: ${Math.round(part * 100)}%`));
    // a cell several states have a part of: their lines one after the other, then their names
    for (const c of all) {
      c.data = new Uint8Array(c.lines + c.names);
      let at = 0;
      for (const s of c.sources) { c.data.set(s.data.subarray(0, s.lines), at); at += s.lines; }
      for (const s of c.sources) { c.data.set(s.data.subarray(s.lines), at); at += s.names; }
    }
    // the file: header, levels, a directory per level, the cells
    const grids = mine.levels.map(l => {
      if (!l.cells.length) return { x0: l.x0, y0: l.y0, cols: 1, rows: 1 };
      const xs = l.cells.map(c => c.cx), ys = l.cells.map(c => c.cy);
      const x0 = Math.min(...xs), y0 = Math.min(...ys);
      return { x0, y0, cols: Math.max(...xs) - x0 + 1, rows: Math.max(...ys) - y0 + 1 };
    });
    const head = new DataView(new ArrayBuffer(28 + 20 * grids.length));
    let at = head.byteLength;
    const where = grids.map(g => { const w = at; at += 12 * g.cols * g.rows; return w; });
    const tables = grids.map(g => new DataView(new ArrayBuffer(12 * g.cols * g.rows)));
    const parts = [];
    mine.levels.forEach((l, i) => {
      for (const c of l.cells) {
        const row = 12 * ((c.cy - grids[i].y0) * grids[i].cols + (c.cx - grids[i].x0));
        tables[i].setUint32(row, at, true);
        tables[i].setUint32(row + 4, c.lines, true);
        tables[i].setUint32(row + 8, c.names, true);
        parts.push(c.data);
        at += c.data.length;
      }
    });
    const n = 2 ** LEVELS[0][0], core = mine.core;
    const west = lonOf(core.x0 / n), east = lonOf((core.x1 + 1) / n);
    const north = latOf(core.y0 / n), south = latOf((core.y1 + 1) / n);
    [0x4A, 0x4D, 0x56, 0x4D].forEach((b, i) => head.setUint8(i, b)); // "JMVM"
    head.setUint16(4, 4, true);
    head.setUint16(6, grids.length, true);
    [south, west, north, east].forEach((v, i) => head.setInt32(8 + 4 * i, Math.trunc(v * 1e7), true));
    head.setUint8(24, Math.min(...LEVELS.map(l => l[1])));
    head.setUint8(25, Math.max(...LEVELS.map(l => l[2])));
    head.setUint16(26, 0, true); // no group: a map of its own
    mine.levels.forEach((l, i) => {
      const o = 28 + 20 * i, g = grids[i];
      head.setUint8(o, l.zoom); head.setUint8(o + 1, l.first); head.setUint8(o + 2, l.last);
      head.setUint32(o + 4, g.x0, true); head.setUint32(o + 8, g.y0, true);
      head.setUint16(o + 12, g.cols, true); head.setUint16(o + 14, g.rows, true);
      head.setUint32(o + 16, where[i], true);
    });
    const blob = new Blob([head, ...tables, ...parts], { type: "application/octet-stream" });
    const lat = (north + south) / 2, lon = (west + east) / 2;
    const name = `jinxmesh-${lat >= 0 ? "n" : "s"}${Math.abs(lat).toFixed(2)}-${lon >= 0 ? "e" : "w"}${Math.abs(lon).toFixed(2)}.jmv`;
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = name;
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 60000);
    say(`Saved ${name}, ${(blob.size / 1e6).toFixed(blob.size < 1e6 ? 2 : 1)} MB. Copy it to a folder below /maps on the card, or convert it with the tool below.`);
  } catch (error) {
    say("The download stopped: " + error.message + ". Try again.");
  }
  for (const c of mine.levels.flatMap(l => l.cells)) { delete c.data; for (const s of c.sources) delete s.data; }
  saveButton.disabled = plan !== mine;
  chooseButton.disabled = false;
};

// ---------- start

(async () => {
  window.addEventListener("resize", resize);
  const at = location.hash.slice(1).split("/").map(Number);
  if (at.length === 3 && at.every(Number.isFinite) && Math.abs(at[1]) <= 85) {
    view.z = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, Math.round(at[0])));
    view.x = mercX(at[2]); view.y = mercY(at[1]);
  }
  resize();
  try {
    const index = await (await fetch(INDEX)).json();
    const select = document.getElementById("goto");
    for (const m of index.maps) {
      const own = m.pieces.map(p => new Piece(m, p));
      pieces.push(...own);
      maps.push({ title: m.title, x0: Math.min(...own.map(p => p.x0)), x1: Math.max(...own.map(p => p.x1)),
        y0: Math.min(...own.map(p => p.y0)), y1: Math.max(...own.map(p => p.y1)) });
    }
    // Alaska reaches across the 180th meridian: its box would span the world, so the middle of its pieces counts
    maps.forEach((m, i) => {
      if (m.x1 - m.x0 > 0.4) {
        const own = pieces.filter(p => p.title === m.title && p.x0 < 0.3);
        if (own.length) { m.x0 = Math.min(...own.map(p => p.x0)); m.x1 = Math.max(...own.map(p => p.x1)); }
      }
      select.add(new Option(m.title, i));
    });
    select.disabled = false;
  } catch (error) {
    say("Could not load the list of maps. Reload the page to try again.");
  }
  draw();
})();
