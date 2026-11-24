// The JinxMesh web flasher: writes the firmware to a ThinkNode M9 over Web Serial, and copies map files to
// its SD card, either straight onto a card in this computer or slowly over the USB cable.
// Nothing leaves the browser: the firmware and the maps are downloaded, the device is on a local port.
import { ESPLoader, Transport } from "./esptool.js";

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const FLASH_BAUD = 230400;

let port = null; // the serial port the visitor picked
let busy = false;
let firmware = null; // firmware/latest.json
let catalog = []; // maps/index.json, its maps
const picked = new Set(); // "<map name>/<piece file>"

// ---------------------------------------------------------------- small things

function log(text, cls = "") {
  const box = $("log");
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = text;
  box.append(line);
  while (box.childElementCount > 400) box.firstElementChild.remove();
  box.scrollTop = box.scrollHeight;
}

function size(bytes) {
  if (bytes >= 1e9) return (bytes / 1e9).toFixed(1) + " GB";
  if (bytes >= 1e6) return (bytes / 1e6).toFixed(bytes >= 1e7 ? 0 : 1) + " MB";
  return Math.max(1, Math.round(bytes / 1e3)) + " kB";
}

function duration(seconds) {
  if (seconds < 90) return Math.max(1, Math.round(seconds)) + " s";
  if (seconds < 5400) return Math.round(seconds / 60) + " min";
  return (seconds / 3600).toFixed(1) + " h";
}

function progress(id, done, of, note = "") {
  const bar = $(id);
  bar.hidden = false;
  bar.querySelector("progress").value = of ? (100 * done) / of : 0;
  bar.querySelector("span").textContent = note;
}

function setBusy(now) {
  busy = now;
  refresh();
}

function refresh() {
  const serial = "serial" in navigator;
  $("connect").disabled = busy || !serial;
  $("connect").textContent = port ? "Choose another port" : "Connect the M9";
  for (const id of ["flash-update", "flash-full", "reset"]) $(id).disabled = busy || !port || !firmware;
  $("port").textContent = port ? "Connected. Ready to flash." : serial ? "Not connected." : "";
  const count = picked.size;
  $("to-card").disabled = busy || !count || !("showDirectoryPicker" in window);
  $("to-cable").disabled = busy || !count || !port;
  $("as-links").disabled = !count;
}

// ---------------------------------------------------------------- the port

async function connect() {
  try {
    // the M9's USB bridge is a CH340; the second entry is Espressif's own USB, for other boards on the bench
    port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x1a86 }, { usbVendorId: 0x303a }] });
    log("Port chosen.");
  } catch (e) {
    if (e.name !== "NotFoundError") log("Could not open the list of ports: " + e.message, "err");
  }
  refresh();
}

async function reset() {
  setBusy(true);
  try {
    await port.open({ baudRate: 115200 });
    await port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await sleep(120);
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    await port.close();
    log("Reset sent. The M9 restarts.");
  } catch (e) {
    log("Reset failed: " + e.message, "err");
    await port.close().catch(() => {});
  }
  setBusy(false);
}

// ---------------------------------------------------------------- flashing

const loaderTerminal = {
  clean() {},
  writeLine(text) {
    if (!/^Writing at /.test(text)) log(text, "dim");
  },
  write() {},
};

async function flash(full) {
  const part = full ? firmware.full : firmware.update;
  if (full && !confirm("A full install erases everything on the M9: settings, channels, keys and chats.\n\nGo on?")) return;
  setBusy(true);
  let transport = null;
  try {
    progress("flash-progress", 0, 1, "Downloading " + part.file);
    const response = await fetch("../firmware/" + part.file, { cache: "no-cache" });
    if (!response.ok) throw new Error(`the firmware file could not be downloaded (${response.status})`);
    const image = new Uint8Array(await response.arrayBuffer());
    if (image.length !== part.bytes || SparkMD5.ArrayBuffer.hash(image.buffer) !== part.md5) throw new Error("the downloaded firmware file is damaged; reload the page and try again");

    progress("flash-progress", 0, 1, "Connecting to the M9");
    transport = new Transport(port, false);
    const loader = new ESPLoader({ transport, baudrate: FLASH_BAUD, romBaudrate: 115200, terminal: loaderTerminal });
    await loader.main();
    if (!/ESP32-S3/.test(loader.chip.CHIP_NAME)) throw new Error(`this is a ${loader.chip.CHIP_NAME}, not the ESP32-S3 of a ThinkNode M9. Nothing was written.`);
    const started = performance.now();
    await loader.writeFlash({
      fileArray: [{ data: image, address: parseInt(part.offset, 16) }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: full,
      compress: true,
      reportProgress(_file, written, total) {
        progress("flash-progress", written, total, `Writing ${Math.round((100 * written) / Math.max(total, 1))}%`);
      },
      // esptool-js compares this with the MD5 the chip computes over what it wrote
      calculateMD5Hash: (data) => SparkMD5.ArrayBuffer.hash(data.slice().buffer),
    });
    await loader.after("hard_reset");
    progress("flash-progress", 1, 1, "Done");
    log(`Wrote and verified ${size(image.length)} in ${duration((performance.now() - started) / 1000)}. The M9 restarts.`, "ok");
    log(full ? "Next: set your region under Settings, Radio, Region." : "Your settings, channels and chats were kept.", "ok");
    log('If the screen stays dark, press "Restart the M9".');
  } catch (e) {
    progress("flash-progress", 0, 1, "Failed");
    log("Flashing failed: " + e.message, "err");
    if (/connect|stream stopped|timeout/i.test(e.message)) log("Unplug the cable, plug it in again and try right away. Use a port on the computer itself, not a hub.", "err");
    if (/already open/i.test(e.message)) log("Another program or tab holds the port. Close it and try again.", "err");
  } finally {
    await transport?.disconnect().catch(() => {});
    setBusy(false);
  }
}

// ---------------------------------------------------------------- the maps list

function key(map, piece) {
  return map.name + "/" + piece.file;
}

function chosen() {
  const out = [];
  for (const map of catalog) for (const piece of map.pieces) if (picked.has(key(map, piece))) out.push({ map, piece });
  return out;
}

function totals() {
  const list = chosen();
  const bytes = list.reduce((sum, c) => sum + c.piece.bytes, 0);
  $("total").textContent = list.length
    ? `${list.length} file${list.length > 1 ? "s" : ""}, ${size(bytes)}. Over the cable that takes about ${duration(bytes / 58000)}.`
    : "Nothing chosen yet.";
  refresh();
}

// the pieces of a map as boxes, north up; a click on a box takes the piece in or out
function pieceMap(map, onChange) {
  const svgNs = "http://www.w3.org/2000/svg";
  const west = Math.min(...map.pieces.map((p) => p.west)), east = Math.max(...map.pieces.map((p) => p.east));
  const south = Math.min(...map.pieces.map((p) => p.south)), north = Math.max(...map.pieces.map((p) => p.north));
  const squeeze = Math.cos((((south + north) / 2) * Math.PI) / 180); // a degree of longitude is shorter up north
  const w = (east - west) * squeeze, h = north - south;
  const scale = Math.min(420 / w, 300 / h);
  const svg = document.createElementNS(svgNs, "svg");
  svg.setAttribute("viewBox", `0 0 ${w * scale + 2} ${h * scale + 2}`);
  svg.setAttribute("class", "pieces");
  svg.setAttribute("role", "group");
  svg.setAttribute("aria-label", `The pieces of ${map.title}; north is up`);
  map.pieces.forEach((piece, i) => {
    const g = document.createElementNS(svgNs, "g");
    const rect = document.createElementNS(svgNs, "rect");
    const x = (piece.west - west) * squeeze * scale + 1, y = (north - piece.north) * scale + 1;
    const pw = (piece.east - piece.west) * squeeze * scale, ph = (piece.north - piece.south) * scale;
    rect.setAttribute("x", x), rect.setAttribute("y", y), rect.setAttribute("width", pw), rect.setAttribute("height", ph);
    const label = document.createElementNS(svgNs, "text");
    label.setAttribute("x", x + pw / 2), label.setAttribute("y", y + ph / 2 + 4);
    label.textContent = map.pieces.length > 1 ? i + 1 : "";
    const title = document.createElementNS(svgNs, "title");
    title.textContent = `${piece.file}, ${size(piece.bytes)}`;
    g.append(rect, label, title);
    g.setAttribute("tabindex", "0");
    g.setAttribute("role", "checkbox");
    const paint = () => {
      g.classList.toggle("on", picked.has(key(map, piece)));
      g.setAttribute("aria-checked", picked.has(key(map, piece)));
    };
    const toggle = () => {
      const k = key(map, piece);
      picked.has(k) ? picked.delete(k) : picked.add(k);
      onChange();
    };
    g.addEventListener("click", toggle);
    g.addEventListener("keydown", (e) => {
      if (e.key === " " || e.key === "Enter") e.preventDefault(), toggle();
    });
    g.paint = paint;
    paint();
    svg.append(g);
  });
  svg.repaint = () => svg.querySelectorAll("g").forEach((g) => g.paint());
  return svg;
}

function renderMaps() {
  const list = $("maps");
  list.textContent = "";
  const filter = $("filter").value.trim().toLowerCase();
  let country = "";
  for (const map of catalog) {
    if (filter && !map.title.toLowerCase().includes(filter)) continue;
    if (map.country !== country) {
      country = map.country;
      const head = document.createElement("h3");
      head.textContent = country;
      list.append(head);
    }
    const row = document.createElement("details");
    row.className = "map";
    const summary = document.createElement("summary");
    const box = document.createElement("input");
    box.type = "checkbox";
    box.setAttribute("aria-label", "All of " + map.title);
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = map.title;
    const info = document.createElement("span");
    info.className = "info";
    summary.append(box, name, info);
    row.append(summary);

    let svg = null;
    const paint = () => {
      const on = map.pieces.filter((p) => picked.has(key(map, p)));
      box.checked = on.length === map.pieces.length;
      box.indeterminate = on.length > 0 && on.length < map.pieces.length;
      const bytes = on.reduce((sum, p) => sum + p.bytes, 0);
      info.textContent = on.length && on.length < map.pieces.length
        ? `${on.length} of ${map.pieces.length} pieces, ${size(bytes)}`
        : `${map.pieces.length} piece${map.pieces.length > 1 ? "s" : ""}, ${size(map.bytes)}`;
      svg?.repaint();
      totals();
    };
    box.addEventListener("click", (e) => e.stopPropagation());
    box.addEventListener("change", () => {
      for (const p of map.pieces) box.checked ? picked.add(key(map, p)) : picked.delete(key(map, p));
      paint();
    });
    row.addEventListener("toggle", () => {
      if (!row.open || svg) return;
      const hint = document.createElement("p");
      hint.className = "hint";
      hint.textContent = map.pieces.length > 1
        ? "North is up. Click the pieces that cover where you will be; you do not need the whole map."
        : "This map is a single file.";
      svg = pieceMap(map, paint);
      row.append(hint, svg);
    });
    paint();
    list.append(row);
  }
  if (!list.childElementCount) list.textContent = filter ? "No map by that name." : "The list of maps could not be loaded.";
}

// ---------------------------------------------------------------- maps onto a card in this computer

async function fetchPiece(map, piece, onBytes) {
  const response = await fetch(map.base + piece.file);
  if (!response.ok) throw new Error(`${piece.file} could not be downloaded (${response.status})`);
  const reader = response.body.getReader();
  const chunks = [];
  let got = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
    got += value.length;
    onBytes(got);
  }
  const blob = new Blob(chunks);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", await blob.arrayBuffer()));
  const hex = [...digest].map((b) => b.toString(16).padStart(2, "0")).join("");
  if (blob.size !== piece.bytes || hex !== piece.sha256) throw new Error(`${piece.file} arrived damaged; try again`);
  return blob;
}

async function toCard() {
  let root;
  try {
    root = await window.showDirectoryPicker({ id: "jinxmesh-card", mode: "readwrite" });
  } catch (e) {
    if (e.name !== "AbortError") log("The folder could not be opened: " + e.message, "err");
    return;
  }
  const list = chosen();
  const all = list.reduce((sum, c) => sum + c.piece.bytes, 0);
  setBusy(true);
  let before = 0, skipped = 0;
  try {
    const maps = await root.getDirectoryHandle("maps", { create: true });
    for (const { map, piece } of list) {
      const folder = await maps.getDirectoryHandle(map.folder, { create: true });
      const there = await folder.getFileHandle(piece.file).then((h) => h.getFile(), () => null);
      if (there && there.size === piece.bytes) {
        skipped++;
      } else {
        const blob = await fetchPiece(map, piece, (got) => progress("map-progress", before + got, all, `${piece.file}: ${size(got)} of ${size(piece.bytes)}`));
        progress("map-progress", before + piece.bytes, all, `Writing ${piece.file} to the card`);
        const file = await (await folder.getFileHandle(piece.file, { create: true })).createWritable();
        await file.write(blob);
        await file.close();
      }
      before += piece.bytes;
      progress("map-progress", before, all, "");
    }
    progress("map-progress", 1, 1, "Done");
    log(`${list.length - skipped} map file${list.length - skipped === 1 ? "" : "s"} written to the card${skipped ? `, ${skipped} already there` : ""}. Eject the card before you pull it.`, "ok");
    log("On the M9: Map, hold OK, Map style, then pick state or province.");
  } catch (e) {
    progress("map-progress", before, all, "Stopped");
    log("Copying stopped: " + e.message, "err");
    if (e.name === "NotAllowedError") log("The browser was not allowed to write there. Pick the card itself, not a system folder.", "err");
  }
  setBusy(false);
}

function asLinks() {
  const box = $("links");
  box.textContent = "";
  box.hidden = false;
  const note = document.createElement("p");
  note.textContent = "Save these files, then copy each to the folder named beside it on the SD card:";
  box.append(note);
  for (const { map, piece } of chosen()) {
    const row = document.createElement("div");
    const a = document.createElement("a");
    a.href = map.base + piece.file;
    a.textContent = piece.file;
    a.download = piece.file;
    row.append(a, ` (${size(piece.bytes)}) goes to /maps/${map.folder}/`);
    box.append(row);
  }
}

// ---------------------------------------------------------------- maps over the cable
// The page's side of the file transfer in the firmware (src/jinx/JinxFiles.cpp on the firmware branch, where
// the protocol is described): frames of 0xA5 0x4A, type, sequence, length, payload, CRC-32 on the console.

const FILES = { BEGIN: 1, DATA: 2, END: 3, STAT: 4, BAUD: 6, EXIT: 7, CRC: 9, ACK: 0x80, ERROR: 0x81, OFFSET: 0x82 };
const FILE_CHUNK = 7680; // two of them fit into the receive buffer of the device's console
const FILE_WINDOW = 2; // Data frames on their way before an answer is in
const FILE_BAUD = 921600;

const crcTable = new Uint32Array(256).map((_, i) => {
  let c = i;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

function crc32(bytes, crc = 0) {
  crc = ~crc >>> 0;
  for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return ~crc >>> 0;
}

function le(value, bytes) {
  const out = new Uint8Array(bytes);
  let v = BigInt(value);
  for (let i = 0; i < bytes; i++, v >>= 8n) out[i] = Number(v & 0xffn);
  return out;
}

function fromLe(bytes) {
  let v = 0n;
  for (let i = bytes.length - 1; i >= 0; i--) v = (v << 8n) | BigInt(bytes[i]);
  return Number(v);
}

function joined(...parts) {
  const out = new Uint8Array(parts.reduce((sum, p) => sum + p.length, 0));
  let at = 0;
  for (const p of parts) out.set(p, at), (at += p.length);
  return out;
}

// The open port with a reader that never stops, so that nothing the device says is lost between requests.
class Line {
  constructor() {
    this.buffer = new Uint8Array(0);
    this.sequence = 0;
    this.wake = null;
    this.open = false;
  }

  async start(baud) {
    await port.open({ baudRate: baud, bufferSize: 65536 });
    this.open = true;
    this.writer = port.writable.getWriter();
    this.done = (async () => {
      while (this.open && port.readable) {
        this.reader = port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) break;
            this.buffer = joined(this.buffer, value);
            this.wake?.();
          }
        } catch {
          // framing and overrun errors are recoverable: the loop takes a new reader
        } finally {
          this.reader.releaseLock();
        }
      }
    })();
  }

  async stop() {
    if (!this.open) return;
    this.open = false;
    await this.reader?.cancel().catch(() => {});
    await this.done;
    this.writer.releaseLock();
    await port.close().catch(() => {});
    this.buffer = new Uint8Array(0);
  }

  async speed(baud) {
    await this.stop();
    await this.start(baud);
  }

  // the next good frame in what came so far, or null; what the console logs in between is skipped
  take() {
    for (;;) {
      const b = this.buffer;
      let start = -1;
      for (let i = 0; i + 1 < b.length; i++) {
        if (b[i] === 0xa5 && b[i + 1] === 0x4a) {
          start = i;
          break;
        }
      }
      if (start < 0) {
        this.buffer = b.slice(Math.max(b.length - 1, 0));
        return null;
      }
      const f = b.subarray(start);
      if (f.length < 6) {
        this.buffer = f.slice();
        return null;
      }
      const length = f[4] | (f[5] << 8);
      if (length > 4096) {
        this.buffer = f.slice(2);
        continue;
      }
      if (f.length < 10 + length) {
        this.buffer = f.slice();
        return null;
      }
      const body = f.subarray(2, 6 + length);
      if (crc32(body) !== fromLe(f.subarray(6 + length, 10 + length))) {
        this.buffer = f.slice(2);
        continue;
      }
      const frame = { type: body[0], sequence: body[1], payload: body.slice(4) };
      this.buffer = f.slice(10 + length);
      return frame;
    }
  }

  async frame(timeout) {
    const until = performance.now() + timeout;
    for (;;) {
      const got = this.take();
      if (got) return got;
      const left = until - performance.now();
      if (left <= 0) return null;
      await new Promise((resolve) => {
        this.wake = resolve;
        setTimeout(resolve, left);
      });
      this.wake = null;
    }
  }

  async send(kind, payload = new Uint8Array(0)) {
    this.sequence = (this.sequence + 1) & 0xff;
    const body = joined(Uint8Array.of(kind, this.sequence, payload.length & 0xff, payload.length >> 8), payload);
    await this.writer.write(joined(Uint8Array.of(0xa5, 0x4a), body, le(crc32(body), 4)));
    return this.sequence;
  }

  // sends a request until its answer comes
  async ask(kind, payload, timeout = 3000, tries = 6) {
    for (let i = 0; i < tries; i++) {
      const sequence = await this.send(kind, payload);
      const until = performance.now() + timeout;
      for (let left = timeout; left > 0; left = until - performance.now()) {
        const got = await this.frame(left);
        if (got && got.sequence === sequence) {
          if (got.type === FILES.ERROR) throw new Error("the M9 says: " + new TextDecoder().decode(got.payload));
          return got;
        }
      }
    }
    throw new Error("the M9 does not answer");
  }

  async hello() {
    const text = new TextEncoder();
    for (let i = 0; i < 4; i++) {
      await this.writer.write(text.encode("\nJXFILES1\n"));
      const got = await this.frame(2000);
      if (got?.type === FILES.ACK && new TextDecoder().decode(got.payload.subarray(0, 8)) === "JXFILES1") return true;
    }
    // it may still be in a transfer from before
    return this.ask(FILES.STAT, text.encode("/"), 3000, 2).then(() => true, () => false);
  }
}

// Copies one file to the card in the device and compares it there. A broken transfer goes on where it stopped.
async function sendFile(line, blob, path, onProgress) {
  const text = new TextEncoder();
  const begun = await line.ask(FILES.BEGIN, joined(le(blob.size, 8), Uint8Array.of(1), text.encode(path)), 10000);
  let offset = fromLe(begun.payload);
  const started = performance.now(), from = offset;
  const pending = []; // sequence numbers of the Data frames without an answer yet, oldest first
  let sent = offset, silent = 0; // sent: where the next frame starts; offset: what the device has confirmed
  while (offset < blob.size) {
    while (pending.length < FILE_WINDOW && sent < blob.size) {
      const data = new Uint8Array(await blob.slice(sent, sent + FILE_CHUNK).arrayBuffer());
      pending.push(await line.send(FILES.DATA, joined(le(sent, 8), data)));
      sent += data.length;
    }
    const got = await line.frame(5000);
    if (!got) {
      if (++silent >= 8) throw new Error("the M9 does not answer");
      pending.length = 0; // again from what is confirmed
      sent = offset;
      continue;
    }
    if (!pending.length || got.sequence !== pending[0]) continue; // the answer to a frame that was given up
    silent = 0;
    if (got.type === FILES.ERROR) throw new Error("the M9 says: " + new TextDecoder().decode(got.payload));
    offset = fromLe(got.payload);
    if (got.type === FILES.OFFSET) {
      pending.length = 0; // a frame got lost: the device says where it is
      sent = offset;
    } else {
      pending.shift();
    }
    onProgress(offset, ((offset - from) * 1000) / Math.max(performance.now() - started, 1));
  }
  await line.ask(FILES.END, undefined, 20000);
  const there = await line.ask(FILES.CRC, text.encode(path), 900000, 1);
  let crc = 0;
  for (let at = 0; at < blob.size; at += 1 << 20) crc = crc32(new Uint8Array(await blob.slice(at, at + (1 << 20)).arrayBuffer()), crc);
  if (fromLe(there.payload.subarray(0, 4)) !== crc) throw new Error(`${path} on the card differs from the file; send it again`);
}

async function toCable() {
  const list = chosen();
  const all = list.reduce((sum, c) => sum + c.piece.bytes, 0);
  if (!confirm(`The cable moves about 3.5 MB a minute, so ${size(all)} takes about ${duration(all / 58000)}.\nKeep this tab open and the M9 plugged in.\n\nStart?`)) return;
  setBusy(true);
  const line = new Line();
  let before = 0, fast = false;
  try {
    await line.start(115200);
    if (!(await line.hello())) throw new Error("the M9 does not answer the file transfer. It needs JinxMesh, an SD card, and no other program on the port.");
    await line.ask(FILES.BAUD, le(FILE_BAUD, 4));
    fast = true;
    await sleep(200);
    await line.speed(FILE_BAUD);
    await line.ask(FILES.STAT, new TextEncoder().encode("/"));
    for (const { map, piece } of list) {
      const blob = await fetchPiece(map, piece, (got) => progress("map-progress", before, all, `Downloading ${piece.file}: ${size(got)} of ${size(piece.bytes)}`));
      await sendFile(line, blob, `/maps/${map.folder}/${piece.file}`, (done, rate) =>
        progress("map-progress", before + done, all, `${piece.file}: ${size(done)} of ${size(piece.bytes)}, about ${duration((all - before - done) / Math.max(rate, 1))} left`));
      before += piece.bytes;
      log(`${piece.file} is on the card and was compared.`, "ok");
    }
    progress("map-progress", 1, 1, "Done");
    log("On the M9: Map, hold OK, Map style, then pick state or province.");
  } catch (e) {
    progress("map-progress", before, all, "Stopped");
    log("The transfer stopped: " + e.message, "err");
    log("Start it again to go on where it stopped.", "err");
  } finally {
    await line.ask(FILES.EXIT, undefined, 2000, 2).catch(() => {}); // the device also goes back by itself after 30 s of silence
    if (fast) await sleep(300);
    await line.stop();
    setBusy(false);
  }
}

// ---------------------------------------------------------------- start

async function start() {
  if (!("serial" in navigator)) $("no-serial").hidden = false;
  if (!("showDirectoryPicker" in window)) $("no-folders").hidden = false;
  if (!window.isSecureContext) $("no-https").hidden = false;

  $("connect").addEventListener("click", connect);
  $("reset").addEventListener("click", reset);
  $("flash-update").addEventListener("click", () => flash(false));
  $("flash-full").addEventListener("click", () => flash(true));
  $("to-card").addEventListener("click", toCard);
  $("to-cable").addEventListener("click", toCable);
  $("as-links").addEventListener("click", asLinks);
  $("filter").addEventListener("input", renderMaps);
  navigator.serial?.addEventListener("disconnect", (e) => {
    if (e.target === port && !busy) (port = null), log("The M9 was unplugged."), refresh();
  });

  try {
    firmware = await (await fetch("../firmware/latest.json", { cache: "no-cache" })).json();
    $("version").textContent = `Build ${firmware.build.slice(0, 10)} (${firmware.version}), ${size(firmware.update.bytes)}`;
  } catch {
    $("version").textContent = "The firmware list could not be loaded. Reload the page.";
  }
  try {
    const index = await (await fetch("../maps/index.json", { cache: "no-cache" })).json();
    catalog = index.maps.sort((a, b) => (a.country === b.country ? a.title.localeCompare(b.title) : a.country === "United States" ? -1 : 1));
  } catch {
    catalog = [];
  }
  renderMaps();
  totals();
}

start();
