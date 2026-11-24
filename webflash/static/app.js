// webflash page: owns the Web Serial ports of this computer and runs what the server asks.
import { ESPLoader, Transport } from "./esptool.js";

const VENDORS = {
  0x303a: "Espressif",
  0x10c4: "Silicon Labs CP210x",
  0x1a86: "WCH CH34x/CH910x",
  0x0403: "FTDI",
  0x239a: "Adafruit",
  0x2e8a: "Raspberry Pi",
  0x1915: "Nordic",
};

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const hex4 = (n) => n.toString(16).padStart(4, "0");

const devices = new Map(); // id -> { id, port, name, state: closed|open|busy, baud, reader, writer, wanted, pump }
let nextId = 1;
let selected = 0; // device the Flash buttons use
let socket = null;
let offers = [];

// output pane

// Text for the terminal pane is collected and shown a few times a second: showing every chunk of serial
// data at once forces a layout each time, which held up the serial traffic itself by 200 ms a chunk.
let termPending = [];
let termTimer = 0;

function term(text, cls) {
  const last = termPending[termPending.length - 1];
  if (last && last.cls === cls) last.text += text;
  else termPending.push({ text, cls });
  if (!termTimer) termTimer = setTimeout(termFlush, 250);
}

function termFlush() {
  termTimer = 0;
  const pane = $("terminal");
  const follow = pane.scrollHeight - pane.scrollTop - pane.clientHeight < 40;
  for (const { text, cls } of termPending) {
    const span = document.createElement("span");
    if (cls) span.className = cls;
    span.textContent = text;
    pane.append(span);
  }
  termPending = [];
  while (pane.childNodes.length > 1500) pane.firstChild.remove();
  if (follow) pane.scrollTop = pane.scrollHeight;
}

// A line about what the page is doing; the server sees it too.
function tool(text, cls = "tool") {
  term(text + "\n", cls);
  send({ t: "log", text });
}

// esptool-js prints through this
let partial = "";
const loaderTerminal = {
  clean() {},
  write(text) {
    partial += text;
  },
  writeLine(text) {
    tool(partial + text);
    partial = "";
  },
};

// server link

function send(message) {
  if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message));
}

function report() {
  const ports = [...devices.values()].map(({ id, name, state, baud }) => ({ id, name, state, baud }));
  send({ t: "ports", ports, remote: $("remote").checked });
}

function connect() {
  const url = new URL("ws", location.href);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(url);
  socket.binaryType = "arraybuffer";
  socket.onopen = () => {
    send({ t: "hello", serial: "serial" in navigator });
    report();
  };
  socket.onclose = () => {
    $("link").className = "pill off";
    $("link").textContent = "server not connected, retrying";
    setTimeout(connect, 2000);
  };
  socket.onmessage = async ({ data }) => {
    if (data instanceof ArrayBuffer) {
      // bytes for a device: first byte is its number
      const bytes = new Uint8Array(data);
      const device = devices.get(bytes[0]);
      if ($("remote").checked && device?.state === "open") device.writer.write(bytes.subarray(1)).catch(() => {});
      return;
    }
    const m = JSON.parse(data);
    if (m.t === "welcome") {
      $("link").className = "pill on";
      $("link").textContent = `connected to ${location.host} as ${m.id}`;
    }
    if (m.offers) {
      offers = m.offers;
      render();
    }
    if (m.t === "cmd") {
      try {
        if (!$("remote").checked) throw new Error("remote control is switched off on the page");
        send({ t: "reply", id: m.id, ok: true, result: await run(m) });
      } catch (e) {
        tool(`${m.op} failed: ${e.message}`, "err");
        send({ t: "reply", id: m.id, ok: false, error: e.message });
      }
    }
  };
}

async function run(cmd) {
  const device = devices.get(cmd.port);
  if (!device) throw new Error(`no device ${cmd.port} on this page`);
  switch (cmd.op) {
    case "open":
      if (device.state === "open" && device.baud !== (cmd.baud || 115200)) await closeDevice(device);
      if (device.state !== "open") await openDevice(device, cmd.baud || 115200);
      return { baud: device.baud };
    case "close":
      await closeDevice(device);
      return {};
    case "reset":
      await resetDevice(device);
      return {};
    case "chip":
      return identify(device);
    case "flash":
      return flash(device, cmd, (file, written, total) => send({ t: "progress", id: cmd.id, file, written, total }));
    default:
      throw new Error(`unknown command ${cmd.op}`);
  }
}

// devices

function describe(port) {
  const { usbVendorId, usbProductId } = port.getInfo();
  if (usbVendorId === undefined) return "serial port (not USB)";
  const vendor = VENDORS[usbVendorId] ? ` ${VENDORS[usbVendorId]}` : "";
  return `USB ${hex4(usbVendorId)}:${hex4(usbProductId)}${vendor}`;
}

// Bring the list in line with the ports the browser has granted and can see.
async function refresh() {
  if (!("serial" in navigator)) return;
  const granted = await navigator.serial.getPorts();
  for (const port of granted) {
    if (![...devices.values()].some((d) => d.port === port)) {
      devices.set(nextId, { id: nextId, port, name: describe(port), state: "closed", baud: 0 });
      nextId++;
    }
  }
  for (const device of [...devices.values()]) {
    if (!granted.includes(device.port) && device.state === "closed") devices.delete(device.id);
  }
  changed();
}

function changed() {
  if (!devices.has(selected)) selected = devices.keys().next().value ?? 0;
  render();
  report();
}

async function openDevice(device, baud = 115200) {
  if (device.state !== "closed") throw new Error(`device ${device.id} is ${device.state}`);
  await device.port.open({ baudRate: baud, bufferSize: 65536 });
  device.state = "open";
  device.baud = baud;
  device.wanted = true;
  device.writer = device.port.writable.getWriter();
  device.pump = pump(device);
  tool(`device ${device.id}: monitoring at ${baud} baud`);
  changed();
}

// Forward everything the device sends until it is closed or lost.
async function pump(device) {
  const decoder = new TextDecoder();
  while (device.wanted && device.port.readable) {
    device.reader = device.port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await device.reader.read();
        if (done) break;
        if (!value.length) continue;
        device.tap?.(value);
        if (socket?.readyState === WebSocket.OPEN) {
          const frame = new Uint8Array(value.length + 1);
          frame[0] = device.id;
          frame.set(value, 1);
          socket.send(frame);
        }
        term(decoder.decode(value, { stream: true }));
      }
    } catch (e) {
      // framing and overrun errors are recoverable: the loop takes a new reader
      term(`\n[device ${device.id}: ${e.message}]\n`, "err");
    } finally {
      device.reader.releaseLock();
    }
  }
  try {
    device.writer.releaseLock();
    await device.port.close();
  } catch {
    // already gone
  }
  if (device.wanted) tool(`device ${device.id}: connection lost`, "err");
  device.wanted = false;
  device.state = "closed";
  await refresh();
}

async function closeDevice(device) {
  if (device.state !== "open") return;
  device.wanted = false;
  await device.reader.cancel().catch(() => {});
  await device.pump;
}

// Restart the device with an RTS pulse and leave the monitor running to catch the boot log.
async function resetDevice(device) {
  if (device.state === "closed") await openDevice(device);
  if (device.state !== "open") throw new Error(`device ${device.id} is ${device.state}`);
  await device.port.setSignals({ dataTerminalReady: false, requestToSend: true });
  await sleep(120);
  await device.port.setSignals({ dataTerminalReady: false, requestToSend: false });
  tool(`device ${device.id}: reset`);
}

// Hand the port to esptool-js for one job, then give it back to the monitor if that was running.
async function withLoader(device, baud, work) {
  const monitored = device.state === "open" ? device.baud : 0;
  await closeDevice(device);
  if (device.state !== "closed") throw new Error(`device ${device.id} is ${device.state}`);
  device.state = "busy";
  changed();
  const transport = new Transport(device.port, false);
  try {
    const loader = new ESPLoader({ transport, baudrate: baud, romBaudrate: 115200, terminal: loaderTerminal });
    await loader.main();
    const result = await work(loader);
    await loader.after("hard_reset");
    return result;
  } finally {
    await transport.disconnect().catch(() => {});
    device.state = "closed";
    await refresh();
    if (monitored && devices.has(device.id)) {
      await openDevice(device, monitored).catch((e) => tool(`device ${device.id}: monitor did not reopen (${e.message})`, "err"));
    }
  }
}

function identify(device) {
  return withLoader(device, 115200, async (loader) => ({
    chip: await loader.chip.getChipDescription(loader),
    features: (await loader.chip.getChipFeatures(loader)).join(", "),
    mac: await loader.chip.readMac(loader),
    flash: (await loader.detectFlashSize()) ?? "unknown",
  }));
}

async function flash(device, job, onProgress) {
  const files = [];
  for (const part of job.parts) {
    tool(`downloading ${part.file} for 0x${part.offset.toString(16)}`);
    const response = await fetch(part.url, { cache: "no-store" });
    if (!response.ok) throw new Error(`${part.file}: server answered ${response.status}`);
    files.push({ data: new Uint8Array(await response.arrayBuffer()), address: part.offset });
  }
  const bytes = files.reduce((sum, file) => sum + file.data.length, 0);
  const bar = $("progress");
  bar.hidden = false;
  bar.value = 0;
  try {
    return await withLoader(device, job.baud || 460800, async (loader) => {
      await loader.writeFlash({
        fileArray: files,
        flashMode: "keep",
        flashFreq: "keep",
        flashSize: "keep",
        eraseAll: !!job.eraseAll,
        compress: true,
        reportProgress(file, written, total) {
          bar.value = (100 * (file + written / Math.max(total, 1))) / files.length;
          onProgress(file, written, total);
        },
        // esptool-js compares this with the MD5 the chip computes over what it wrote
        calculateMD5Hash: (image) => SparkMD5.ArrayBuffer.hash(image.slice().buffer),
      });
      tool(`device ${device.id}: wrote and verified ${bytes} bytes`);
      return { chip: loader.chip.CHIP_NAME, bytes };
    });
  } finally {
    bar.hidden = true;
  }
}

// Files for the SD card over the serial console: the page's side of firmware/src/jinx/JinxFiles.cpp, where
// the protocol is described. The same as serialfiles.py does through the server.

const FILES = { BEGIN: 1, DATA: 2, END: 3, STAT: 4, DELETE: 5, BAUD: 6, EXIT: 7, LIST: 8, CRC: 9, TONE: 10, READ: 11, ACK: 0x80, ERROR: 0x81, OFFSET: 0x82 };
const FILE_CHUNK = 7680; // two of them fit into the receive buffer of the console of the device
const FILE_WINDOW = 2; // Data frames on their way before an answer is in
const FILE_BAUD = 921600;

const crcTable = new Uint32Array(256).map((_, i) => {
  let c = i;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

// running CRC-32 (the one of zlib): start with 0
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
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

class FileLink {
  constructor(device) {
    this.device = device;
    this.buffer = new Uint8Array(0);
    this.sequence = 0;
    this.wake = null;
    device.tap = (bytes) => {
      this.buffer = joined(this.buffer, bytes);
      this.wake?.();
    };
  }

  release() {
    this.device.tap = null;
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
    await this.device.writer.write(joined(Uint8Array.of(0xa5, 0x4a), body, le(crc32(body), 4)));
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
          if (got.type === FILES.ERROR) throw new Error("the device says: " + new TextDecoder().decode(got.payload));
          return got;
        }
      }
    }
    throw new Error("the device does not answer");
  }

  async hello() {
    const text = new TextEncoder();
    for (let i = 0; i < 4; i++) {
      await this.device.writer.write(text.encode("\nJXFILES1\n"));
      const got = await this.frame(2000);
      if (got?.type === FILES.ACK && new TextDecoder().decode(got.payload.subarray(0, 8)) === "JXFILES1") return true;
    }
    // it may still be in a transfer from before
    return this.ask(FILES.STAT, text.encode("/"), 3000, 2).then(() => true, () => false);
  }
}

async function monitorAt(device, baud) {
  if (device.state === "open" && device.baud !== baud) await closeDevice(device);
  if (device.state === "closed") await openDevice(device, baud);
  if (device.state !== "open") throw new Error(`device ${device.id} is ${device.state}`);
}

async function fileCrc(file) {
  let crc = 0;
  for (let at = 0; at < file.size; at += 1 << 20) crc = crc32(new Uint8Array(await file.slice(at, at + (1 << 20)).arrayBuffer()), crc);
  return crc;
}

// Copies a file to the card of the device and compares it there. onProgress(bytes done, of, bytes a second).
async function sendFile(device, file, path, onProgress) {
  const text = new TextEncoder();
  await monitorAt(device, 115200);
  const link = new FileLink(device);
  let fast = false;
  try {
    if (!(await link.hello())) throw new Error("the device does not answer the file transfer; does its firmware have it?");
    await link.ask(FILES.BAUD, le(FILE_BAUD, 4));
    fast = true;
    await sleep(200);
    await monitorAt(device, FILE_BAUD);
    await link.ask(FILES.STAT, text.encode("/"));

    const begun = await link.ask(FILES.BEGIN, joined(le(file.size, 8), Uint8Array.of(1), text.encode(path)), 10000);
    let offset = fromLe(begun.payload);
    if (offset) tool(`going on at ${offset.toLocaleString()} of ${file.size.toLocaleString()} bytes`);
    const started = performance.now(), from = offset;
    const pending = []; // sequence numbers of the Data frames without an answer yet, oldest first
    let sent = offset, silent = 0; // sent: where the next frame starts; offset: what the device has confirmed
    while (offset < file.size) {
      while (pending.length < FILE_WINDOW && sent < file.size) {
        const data = new Uint8Array(await file.slice(sent, sent + FILE_CHUNK).arrayBuffer());
        pending.push(await link.send(FILES.DATA, joined(le(sent, 8), data)));
        sent += data.length;
      }
      const got = await link.frame(5000);
      if (!got) {
        if (++silent >= 8) throw new Error("the device does not answer");
        pending.length = 0; // again from what is confirmed
        sent = offset;
        continue;
      }
      if (!pending.length || got.sequence !== pending[0]) continue; // the answer to a frame that was given up
      silent = 0;
      if (got.type === FILES.ERROR) throw new Error("the device says: " + new TextDecoder().decode(got.payload));
      offset = fromLe(got.payload);
      if (got.type === FILES.OFFSET) {
        pending.length = 0; // a frame got lost: the device says where it is
        sent = offset;
      } else {
        pending.shift();
      }
      onProgress(offset, file.size, ((offset - from) * 1000) / Math.max(performance.now() - started, 1));
    }
    await link.ask(FILES.END, undefined, 20000);
    tool(`saved ${path}: ${file.size.toLocaleString()} bytes; comparing`);
    const there = await link.ask(FILES.CRC, text.encode(path), 900000, 1);
    if (fromLe(there.payload.subarray(0, 4)) !== (await fileCrc(file))) throw new Error(`${path} on the card differs from the file; send it again`);
    tool(`${path}: the file on the card is the same`);
  } finally {
    await link.ask(FILES.EXIT, undefined, 2000, 2).catch(() => {}); // it also goes back by itself after 30 s of silence
    link.release();
    if (fast) {
      await sleep(300);
      await monitorAt(device, 115200).catch((e) => tool(e.message, "err"));
    }
  }
}

function filePathFor(kind, name) {
  if (kind === "roads") return "/geo/roads.bin";
  if (kind === "towns") return "/geo/towns.bin";
  const clean = name.replace(/[^A-Za-z0-9._-]/g, "_");
  if (kind === "map") {
    const style = clean.replace(/\.pmtiles$/i, "");
    return `/maps/${style}/${style}.pmtiles`;
  }
  return "/" + clean;
}

function fileKindOf(name) {
  if (/\.pmtiles$/i.test(name)) return "map";
  if (/road/i.test(name)) return "roads";
  if (/town/i.test(name)) return "towns";
  return "other";
}

let sending = false;

function fileControls() {
  const file = $("file").files[0];
  const target = devices.get(selected);
  $("sendfile").disabled = sending || !file || !target || target.state === "busy" || !$("filepath").value.startsWith("/");
  $("sendfile").textContent = target ? `Send to device ${target.id}` : "Send to the device";
  $("sendserver").disabled = sending || !serverFiles.length || !target || target.state === "busy";
  $("sendserver").textContent = $("sendfile").textContent;
}

$("file").onchange = () => {
  const file = $("file").files[0];
  if (file) {
    $("filekind").value = fileKindOf(file.name);
    $("filepath").value = filePathFor($("filekind").value, file.name);
  }
  fileControls();
};
$("filekind").onchange = () => {
  $("filepath").value = filePathFor($("filekind").value, $("file").files[0]?.name ?? "file.bin");
  fileControls();
};
$("filepath").oninput = fileControls;
// Sends a file (or a Blob with a name) to the card, with the bar and the line of text showing how far it is.
async function runSend(device, file, name, path) {
  const bar = $("fileprogress"), state = $("filestate");
  sending = true;
  fileControls();
  tuneChanged();
  bar.hidden = false;
  bar.value = 0;
  tool(`sending ${name} (${file.size.toLocaleString()} bytes) to ${path} on device ${device.id}`);
  let shown = 0;
  try {
    await sendFile(device, file, path, (done, of, rate) => {
      bar.value = (100 * done) / Math.max(of, 1);
      if (performance.now() - shown < 500 && done < of) return;
      shown = performance.now();
      const left = rate > 0 ? (of - done) / rate / 60 : 0;
      state.textContent = `${((100 * done) / Math.max(of, 1)).toFixed(0)}% · ${(done / 1e6).toFixed(1)} of ${(of / 1e6).toFixed(1)} MB · ${(rate / 1000).toFixed(0)} kB/s · ${left.toFixed(1)} min left. Keep this tab in front.`;
    });
    state.textContent = `${name} is on the card as ${path}, and compared: the same.`;
  } catch (e) {
    tool(`sending ${name} failed: ${e.message}`, "err");
    state.textContent = `Failed: ${e.message}. Send it again to go on where it stopped.`;
  } finally {
    sending = false;
    bar.hidden = true;
    fileControls();
    tuneChanged();
  }
}

$("sendfile").onclick = () => {
  const file = $("file").files[0];
  return runSend(devices.get(selected), file, file.name, $("filepath").value.trim());
};

// Data files the server has ready (static/data/index.json): fetched here, then sent like any other file.
let serverFiles = [];
fetch("data/index.json", { cache: "no-store" })
  .then((r) => r.json())
  .then((list) => {
    serverFiles = list;
    $("serverfile").replaceChildren();
    for (const [i, entry] of list.entries()) {
      const option = document.createElement("option");
      option.value = i;
      option.textContent = `${entry.name} (${(entry.bytes / 1e6).toFixed(0)} MB, to ${entry.path})`;
      $("serverfile").append(option);
    }
    fileControls();
  })
  .catch(() => {});
$("sendserver").onclick = async () => {
  const entry = serverFiles[$("serverfile").value], device = devices.get(selected), state = $("filestate");
  if (!entry || !device) return;
  sending = true;
  fileControls();
  let blob;
  try {
    state.textContent = `Fetching ${entry.file} from the server (${(entry.bytes / 1e6).toFixed(0)} MB)...`;
    const response = await fetch(`data/${entry.file}`);
    if (!response.ok) throw new Error(`the server answered ${response.status}`);
    blob = await response.blob();
    if (blob.size !== entry.bytes) throw new Error(`${entry.file} came incomplete (${blob.size} of ${entry.bytes} bytes)`);
  } catch (e) {
    tool(`fetching ${entry.file} failed: ${e.message}`, "err");
    state.textContent = `Failed: ${e.message}`;
    sending = false;
    fileControls();
    return;
  }
  await runSend(device, blob, entry.file, entry.path);
};

// Tunes: an editor for RTTTL text, a player with the sound of a piezo, and the way to the device.

const TUNE_NOTES = ["c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"];

// "name:d=4,o=5,b=120:8c6,p,2a#." as { name, notes: [{ hz (0: pause), ms }] }; throws with what is wrong
function parseTune(text) {
  const parts = text.replace(/\s+/g, "").split(":");
  if (parts.length !== 3) throw new Error("a tune has three parts: name, settings, notes, with a colon between");
  let duration = 4, octave = 6, tempo = 63;
  for (const setting of parts[1].toLowerCase().split(",").filter(Boolean)) {
    const [key, value] = setting.split("=");
    const number = parseInt(value, 10);
    if (!(number > 0)) throw new Error(`setting ${setting} has no number`);
    if (key === "d") duration = number;
    else if (key === "o") octave = number;
    else if (key === "b") tempo = number;
    else throw new Error(`unknown setting ${setting}`);
  }
  const whole = (60000 / tempo) * 4;
  const notes = [];
  for (const token of parts[2].toLowerCase().split(",").filter(Boolean)) {
    const m = /^(\d*)([a-hp])(#?)(\.?)(\d?)(\.?)$/.exec(token);
    if (!m) throw new Error(`"${token}" is no note`);
    let ms = whole / (m[1] ? parseInt(m[1], 10) : duration);
    if (m[4] || m[6]) ms *= 1.5;
    let hz = 0;
    if (m[2] !== "p") {
      const letter = m[2] === "h" ? "b" : m[2];
      const index = TUNE_NOTES.indexOf(letter + m[3]);
      if (index < 0) throw new Error(`"${token}" is no note`);
      const scale = m[5] ? parseInt(m[5], 10) : octave;
      hz = 440 * 2 ** ((index - 9) / 12 + (scale - 4)); // a4 is 440 Hz
    }
    notes.push({ hz, ms });
  }
  if (!notes.length) throw new Error("no notes yet");
  return { name: parts[0], notes };
}

// One audio context for the page: a browser allows only a few, and one that was just closed still counts
// for a moment, so a new one for every note left some notes silent.
let tuneAudio = null;
let tuneVoice = null; // the oscillator that plays now

function stopTune() {
  try {
    tuneVoice?.stop();
  } catch {
    // has ended already
  }
  tuneVoice?.disconnect();
  tuneVoice = null;
}

// Plays a tune here; minMs makes every note at least that long, for hearing one short note
function playTuneHere(text, minMs = 0) {
  const { notes } = parseTune(text);
  stopTune();
  tuneAudio ??= new AudioContext();
  const audio = tuneAudio;
  if (audio.state === "suspended") audio.resume();
  const gain = audio.createGain();
  gain.gain.value = 0;
  gain.connect(audio.destination);
  const oscillator = audio.createOscillator();
  oscillator.type = "square"; // what a piezo driven by a pin gives
  oscillator.connect(gain);
  let at = audio.currentTime + 0.03;
  for (const note of notes) {
    const seconds = Math.max(note.ms, minMs) / 1000;
    if (note.hz) {
      oscillator.frequency.setValueAtTime(note.hz, at);
      gain.gain.setValueAtTime(0.15, at);
      gain.gain.setValueAtTime(0, at + seconds * 0.9); // a short gap, so that equal notes in a row stay apart
    }
    at += seconds;
  }
  oscillator.start();
  oscillator.stop(at + 0.02);
  oscillator.onended = () => {
    gain.disconnect();
    if (tuneVoice === oscillator) tuneVoice = null;
  };
  tuneVoice = oscillator;
  return at - audio.currentTime;
}

// One short job on the device's file transfer, at the console's own speed.
async function withFiles(device, work) {
  await monitorAt(device, 115200);
  const link = new FileLink(device);
  try {
    if (!(await link.hello())) throw new Error("the device does not answer; does its firmware have the file transfer?");
    return await work(link);
  } finally {
    await link.ask(FILES.EXIT, undefined, 2000, 2).catch(() => {});
    link.release();
  }
}

function tuneHeader() {
  const name = $("tunename").value.replace(/[^A-Za-z0-9_-]/g, "") || "tune";
  const tempo = Math.min(Math.max(parseInt($("tunetempo").value, 10) || 160, 25), 900);
  return `${name}:d=${$("tunelength").value},o=5,b=${tempo}:`;
}

function tuneNotes() {
  const text = $("tunetext").value.replace(/\s+/g, "");
  const colon = text.lastIndexOf(":");
  return colon < 0 ? text : text.slice(colon + 1);
}

function setTune(notes) {
  $("tunetext").value = tuneHeader() + notes;
  tuneChanged();
}

function tuneChanged() {
  const text = $("tunetext").value.replace(/\s+/g, "");
  let problem = "";
  try {
    const { notes } = parseTune(text);
    const seconds = notes.reduce((sum, n) => sum + n.ms, 0) / 1000;
    problem = `${notes.length} notes · ${seconds.toFixed(1)} s · ${text.length} characters`;
    if (text.length > 399) problem += " · too long, 399 at most";
    else if (text.length > 230) problem += " · too long for \"Notification tone\" (230), fine for the others";
  } catch (e) {
    problem = e.message;
  }
  $("tunestate").textContent = problem;
  const target = devices.get(selected);
  const ready = !sending && target && target.state !== "busy";
  $("tunedevice").disabled = $("tunesave").disabled = !ready;
}

// every note is written out with its length and octave, so that the text reads the same whatever its settings say
function addNote(note, octave) {
  const token = `${$("tunelength").value}${note}${$("tunedotted").checked ? "." : ""}${note === "p" ? "" : octave}`;
  const notes = tuneNotes();
  setTune(notes ? `${notes},${token}` : token);
  // hear what was pressed: here, and on the device's own buzzer when that is asked for
  const single = `n:d=4,o=5,b=${parseInt($("tunetempo").value, 10) || 160}:${token}`;
  try {
    playTuneHere(single, 180); // long enough to hear, however short the note is
  } catch {
    // no sound, the note is still added
  }
  const device = devices.get(selected);
  if ($("tunelive").checked && note !== "p" && device && device.state !== "busy" && !sending) {
    // one after the other: the device's console takes one request at a time
    liveNotes = liveNotes
      .then(() => withFiles(device, (link) => link.ask(FILES.TONE, new TextEncoder().encode(single), 2000, 1)))
      .catch((e) => {
        $("tunelive").checked = false;
        tool(`sounding notes on the device stopped: ${e.message}`, "err");
      });
  }
}

let liveNotes = Promise.resolve();

// The notes as a scale: a row for each octave RTTTL has (4 to 7), the twelve notes from C to B in it.
{
  const scale = $("scale");
  scale.append(span("octave", ""));
  for (const note of TUNE_NOTES) scale.append(span("head", note.toUpperCase()));
  for (let octave = 4; octave <= 7; octave++) {
    scale.append(span("octave", `Octave ${octave}`));
    for (const note of TUNE_NOTES) {
      const key = document.createElement("button");
      key.textContent = `${note.toUpperCase()}${octave}`;
      key.title = `${note.toUpperCase()}${octave}`;
      if (note.endsWith("#")) key.className = "sharp";
      key.onclick = () => addNote(note, octave);
      scale.append(key);
    }
  }
}
$("tunepause").onclick = () => addNote("p", 0);
$("tuneundo").onclick = () => setTune(tuneNotes().split(",").slice(0, -1).join(","));
$("tuneclear").onclick = () => setTune("");
$("tunelength").onchange = () => setTune(tuneNotes());
// the name and the tempo are part of the text: changing them rewrites its first two parts
$("tunename").oninput = $("tunetempo").oninput = () => setTune(tuneNotes());
$("tunetext").oninput = tuneChanged;
$("tuneplay").onclick = () => {
  try {
    playTuneHere($("tunetext").value);
  } catch (e) {
    $("tunestate").textContent = e.message;
  }
};
$("tunestop").onclick = stopTune;

async function tuneJob(what, work) {
  const device = devices.get(selected);
  const text = $("tunetext").value.replace(/\s+/g, "");
  try {
    parseTune(text);
    if (text.length > 399) throw new Error("the tune is too long, 399 characters at most");
    sending = true;
    tuneChanged();
    fileControls();
    $("tunestate").textContent = await withFiles(device, (link) => work(link, text));
  } catch (e) {
    tool(`${what} failed: ${e.message}`, "err");
    $("tunestate").textContent = `Failed: ${e.message}`;
  } finally {
    sending = false;
    fileControls();
    const state = $("tunestate").textContent;
    tuneChanged();
    $("tunestate").textContent = state;
  }
}

$("tunedevice").onclick = () =>
  tuneJob("playing on the device", async (link, text) => {
    const answer = await link.ask(FILES.TONE, new TextEncoder().encode(text), 3000, 2);
    return answer.payload[0] ? "The device plays it." : "No sound: sound is switched off on the device (Settings, Sounds).";
  });
$("tunesave").onclick = () =>
  tuneJob("saving the tune", async (link, text) => {
    const name = text.slice(0, text.indexOf(":")).replace(/[^A-Za-z0-9_-]/g, "") || "tune";
    const path = `/tones/${name}.rtttl`;
    const data = new TextEncoder().encode(text);
    await link.ask(FILES.BEGIN, joined(le(data.length, 8), Uint8Array.of(0), new TextEncoder().encode(path)), 10000);
    await link.ask(FILES.DATA, joined(le(0, 8), data), 5000);
    await link.ask(FILES.END, undefined, 20000);
    tool(`saved ${path} on the device`);
    showTunes(await listFolder(link, "/tones").catch(() => []));
    return `Saved as ${path}. Pick it on the device in Settings, Sounds.`;
  });
setTune("8c6,8e6,8g6,4c7");

// What is on the card: a folder as a list, folders entered, files deleted.

let cardPath = "/";

function cardJob(what, work) {
  const device = devices.get(selected);
  if (!device || device.state === "busy" || sending) return Promise.resolve();
  sending = true;
  fileControls();
  tuneChanged();
  return withFiles(device, work)
    .catch((e) => {
      tool(`${what} failed: ${e.message}`, "err");
      $("cardstate").textContent = `Failed: ${e.message}`;
    })
    .finally(() => {
      sending = false;
      fileControls();
      tuneChanged();
    });
}

async function listFolder(link, path) {
  const answer = await link.ask(FILES.LIST, new TextEncoder().encode(path), 20000, 2);
  const entries = [];
  for (const line of new TextDecoder().decode(answer.payload).split("\n")) {
    if (line.startsWith("d ")) entries.push({ name: line.slice(2), folder: true });
    else if (line.startsWith("f ")) {
      const space = line.indexOf(" ", 2);
      entries.push({ name: line.slice(space + 1), size: parseInt(line.slice(2, space), 10) });
    } else if (line === "...") entries.push({ more: true });
  }
  return entries;
}

function sizeText(bytes) {
  return bytes >= 1e6 ? `${(bytes / 1e6).toFixed(1)} MB` : bytes >= 1e3 ? `${(bytes / 1e3).toFixed(0)} kB` : `${bytes} bytes`;
}

function showCard(path, entries) {
  cardPath = path;
  $("cardpath").textContent = path;
  $("cardstate").textContent = "";
  const list = $("cardlist");
  list.replaceChildren();
  const join = (name) => (path === "/" ? "" : path) + "/" + name.replace(/^.*\//, "");
  const sorted = entries.filter((e) => !e.more).sort((a, b) => !!b.folder - !!a.folder || a.name.localeCompare(b.name));
  for (const entry of sorted) {
    const row = document.createElement("div");
    row.className = "row";
    const full = join(entry.name);
    row.append(span("name", entry.name.replace(/^.*\//, "") + (entry.folder ? "/" : "")), span("detail", entry.folder ? "folder" : sizeText(entry.size)), span("grow", ""));
    if (entry.folder) row.append(button("Open", () => openCard(full)));
    else
      row.append(
        button("Save here", () =>
          cardJob("saving", async (link) => {
            $("cardstate").textContent = `Reading ${full} ...`;
            const bytes = await readFile(link, full, entry.size, (done) => {
              $("cardstate").textContent = `Reading ${full}: ${sizeText(done)} of ${sizeText(entry.size)}`;
            });
            saveHere(entry.name.replace(/^.*\//, ""), bytes);
            tool(`saved ${full} from the device: ${bytes.length} bytes`);
            $("cardstate").textContent = `Saved ${full} to this computer's downloads.`;
          }),
        ),
      );
    row.append(
      button("Delete", async () => {
        if (!confirm(`Delete ${full} from the card?${entry.folder ? " (a folder has to be empty)" : ""}`)) return;
        await cardJob("deleting", async (link) => {
          await link.ask(FILES.DELETE, new TextEncoder().encode(full), 20000, 2);
          tool(`deleted ${full} on the device`);
          showCard(path, await listFolder(link, path));
        });
      }),
    );
    list.append(row);
  }
  if (!sorted.length) list.append(span("empty", "Nothing in this folder."));
  if (entries.some((e) => e.more)) list.append(span("detail", "There are more files than fit in one answer."));
}

function openCard(path) {
  return cardJob("reading the card", async (link) => showCard(path, await listFolder(link, path)));
}

$("cardshow").onclick = () => openCard(cardPath);
$("cardup").onclick = () => openCard(cardPath.replace(/\/[^/]*$/, "") || "/");
$("cardwardrive").onclick = () => openCard("/apps/wardrive");

// The logs of the card over Bluetooth: the read-only file service of the JinxMesh firmware (firmware/src/jinx/JinxBle.cpp).
// One write (where to start, the path) and one read (0, where it starts, the bytes; or 1 and a text) per piece.

const BLE = {
  MESH: "6ba1b218-15a8-461f-9fa8-5dcae273eafd", // what the device advertises
  SERVICE: "9971f534-e514-4da7-81ce-e011c4e47254",
  FILES: "c4f2a44c-dece-403d-851b-8f4ee0c4b4ad",
  OFFER: "9c6efe0e-fe07-414d-9ee1-e7b101e01d9f", // notifies: the path of a file the device wants to send
  INBOX: "5d0b7c7e-2f0a-4c55-9d53-6a0c1f1b7e21", // takes a line of text for the screen: a destination, the stops of a route
};
let bleFiles = null; // the characteristic while connected
let bleInbox = null; // where destinations go; null with firmware that has none
let bleBusy = false;
let blePath = "/apps/wardrive";

async function blePiece(path, offset) {
  for (let tries = 0; ; tries++) {
    try {
      await bleFiles.writeValueWithResponse(joined(le(offset, 4), new TextEncoder().encode(path)));
      const view = await bleFiles.readValue();
      const bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
      if (!bytes.length) throw new Error("an empty answer");
      if (bytes[0] !== 0) throw Object.assign(new Error(new TextDecoder().decode(bytes.subarray(1))), { final: true });
      if (bytes.length < 5 || new DataView(bytes.buffer, bytes.byteOffset + 1, 4).getUint32(0, true) !== offset) throw new Error("an answer out of step");
      return bytes.slice(5);
    } catch (e) {
      if (e.final || tries >= 3 || !bleFiles || !bleFiles.service.device.gatt.connected) throw e;
      await sleep(200);
    }
  }
}

async function bleRead(path, onProgress = null) {
  const parts = [];
  let length = 0;
  for (;;) {
    const piece = await blePiece(path, length);
    if (!piece.length) return joined(...parts);
    parts.push(piece);
    length += piece.length;
    if (onProgress) onProgress(length);
    if (length > 64 << 20) throw new Error("that file is too large to read here");
  }
}

function bleJob(what, work) {
  if (bleBusy || !bleFiles) return Promise.resolve();
  bleBusy = true;
  return work()
    .catch((e) => {
      tool(`Bluetooth: ${what} failed: ${e.message}`, "err");
      $("blestate").textContent = `Failed: ${e.message}`;
    })
    .finally(() => {
      bleBusy = false;
    });
}

function bleOpen(path) {
  return bleJob("reading the folder", async () => {
    $("blestate").textContent = `Reading ${path} ...`;
    const text = new TextDecoder().decode(await bleRead(path + "/"));
    blePath = path;
    $("blepath").textContent = path;
    $("bleup").disabled = path === "/apps";
    const entries = [];
    for (const line of text.split("\n")) {
      if (line.startsWith("d ")) entries.push({ name: line.slice(2).replace(/^.*\//, ""), folder: true });
      else if (line.startsWith("f ")) {
        const space = line.indexOf(" ", 2);
        entries.push({ name: line.slice(space + 1).replace(/^.*\//, ""), size: parseInt(line.slice(2, space), 10) });
      }
    }
    entries.sort((a, b) => !!b.folder - !!a.folder || b.name.localeCompare(a.name)); // the newest log first
    const list = $("blelist");
    list.replaceChildren();
    for (const entry of entries) {
      const full = `${path}/${entry.name}`;
      const row = document.createElement("div");
      row.className = "row";
      row.append(span("name", entry.name + (entry.folder ? "/" : "")), span("detail", entry.folder ? "folder" : sizeText(entry.size)), span("grow", ""));
      if (entry.folder) row.append(button("Open", () => bleOpen(full)));
      else
        row.append(
button("Save here", () => bleSave(full, entry.size)));
      list.append(row);
    }
    if (!entries.length) list.append(span("empty", "Nothing in this folder."));
    $("blestate").textContent = "";
  });
}

// A file by its path, to this computer's downloads.
function bleSave(full, size = 0) {
  return bleJob("saving", async () => {
    const started = performance.now();
    const bytes = await bleRead(full, (done) => {
      $("blestate").textContent = `Reading ${full}: ${sizeText(done)}${size ? ` of ${sizeText(size)}` : ""}`;
    });
    saveHere(full.replace(/^.*\//, ""), bytes);
    const seconds = (performance.now() - started) / 1000;
    tool(`Bluetooth: saved ${full}: ${bytes.length} bytes in ${seconds.toFixed(0)} s`);
    $("blestate").textContent = `Saved ${full} to this computer's downloads.`;
  });
}

// "Send the log by Bluetooth" was chosen on the device: fetch what it names (a folder: show it).
let bleWaiting = Promise.resolve();
function bleOffered(event) {
  const view = event.target.value;
  const path = new TextDecoder().decode(new Uint8Array(view.buffer, view.byteOffset, view.byteLength));
  tool(`Bluetooth: the device sends ${path}`);
  bleWaiting = bleWaiting.then(async () => {
    while (bleBusy) await sleep(300); // after what is running
    if (!bleFiles) return;
    if (path.endsWith("/")) await bleOpen(path.slice(0, -1));
    else await bleSave(path);
  });
}

// Navigate: a destination or a route of several stops for the Navigate app of the device, as lines of text
// (device-ui KeyboardViewAppsRoute.cpp, routeCommand): D lat,lon,c|f,name / R name / S lat,lon,name / G c|f / K.

// "44.2601, -72.5754 Montpelier" or "44.2601 -72.5754, Montpelier": two numbers, then what the place is called.
function placeOf(text) {
  const m = text.trim().match(/^(-?\d+(?:\.\d+)?)\s*[,;\s]\s*(-?\d+(?:\.\d+)?)\s*[,;]?\s*(.*)$/);
  if (!m) return null;
  const lat = parseFloat(m[1]), lon = parseFloat(m[2]);
  if (!(Math.abs(lat) <= 90 && Math.abs(lon) <= 180) || (lat === 0 && lon === 0)) return null;
  return { lat, lon, name: m[3].replace(/,/g, " ").trim().slice(0, 24) };
}

async function bleLines(lines) {
  for (const line of lines) {
    await bleInbox.writeValueWithResponse(new TextEncoder().encode(line.slice(0, 150)));
    // the screen takes them a few at a time: the next one only when it has room, or it would be dropped
    for (let waited = 0; ; waited += 300) {
      const room = await bleInbox.readValue();
      if (!room.byteLength || room.getUint8(0) >= 2) break;
      if (waited >= 30000) throw new Error("the device takes no more lines: is its screen task busy? Try again");
      await sleep(300);
    }
  }
}

function goSend(start) {
  const mode = $("gomode").value;
  const places = [];
  const rows = $("gostops").value.split("\n").filter((row) => row.trim());
  for (const [i, row] of rows.entries()) {
    const place = placeOf(row);
    if (!place) {
      $("gostate").textContent = `Line ${i + 1} is no place: latitude, longitude, name. Like: 44.2601, -72.5754, Montpelier`;
      return Promise.resolve();
    }
    places.push(place);
  }
  if (!places.length) {
    $("gostate").textContent = "No place yet: latitude, longitude, name. One line for a destination, several for a route.";
    return Promise.resolve();
  }
  if (places.length > 1024) {
    $("gostate").textContent = "At most 1024 stops.";
    return Promise.resolve();
  }
  const fixed = (place) => `${place.lat.toFixed(6)},${place.lon.toFixed(6)}`;
  const name = $("goname").value.replace(/[^A-Za-z0-9 _-]/g, "").trim().slice(0, 28);
  let lines;
  if (places.length === 1 && start && !name) lines = [`D ${fixed(places[0])},${mode},${places[0].name}`];
  else
    lines = [`R ${name || "From the page"}`, ...places.map((place, i) => `S ${fixed(place)},${place.name || `Stop ${i + 1}`}`), start ? `G ${mode}` : "K"];
  return bleJob("sending the route", async () => {
    await bleLines(lines);
    const what = places.length === 1 ? "The destination" : `The route of ${places.length} stops`;
    $("gostate").textContent = `${what} is on the device${start ? ": Navigate shows the way" : ", under Navigate, A saved route"}.`;
    tool(`Bluetooth: sent ${places.length} place${places.length === 1 ? "" : "s"} to the device`);
  });
}
$("gostart").onclick = () => goSend(true);
$("gokeep").onclick = () => goSend(false);

function bleGone() {
  bleInbox = null;
  $("gostart").disabled = $("gokeep").disabled = true;
  bleFiles = null;
  $("bleconnect").textContent = "Connect by Bluetooth";
  $("bleup").disabled = true;
  $("blestate").textContent = "Not connected.";
}

$("bleconnect").onclick = async () => {
  if (bleFiles) {
    bleFiles.service.device.gatt.disconnect();
    return;
  }
  if (!navigator.bluetooth) {
    $("blestate").textContent = "This browser has no Web Bluetooth (Chrome or Edge have it; on Linux it may have to be turned on in chrome://flags).";
    return;
  }
  try {
    const device = await navigator.bluetooth.requestDevice({ filters: [{ services: [BLE.MESH] }], optionalServices: [BLE.SERVICE] });
    $("blestate").textContent = `Connecting to ${device.name || "the device"} ...`;
    device.addEventListener("gattserverdisconnected", bleGone);
    const server = await device.gatt.connect();
    let service;
    try {
      service = await server.getPrimaryService(BLE.SERVICE);
    } catch (e) {
      server.disconnect();
      throw new Error("this device has no file service: it needs newer JinxMesh firmware");
    }
    bleFiles = await service.getCharacteristic(BLE.FILES);
    try {
      const offer = await service.getCharacteristic(BLE.OFFER);
      offer.addEventListener("characteristicvaluechanged", bleOffered);
      await offer.startNotifications();
    } catch (e) {
      tool("Bluetooth: this firmware cannot start a send from the device; fetching from here works", "err");
    }
    try {
      bleInbox = await service.getCharacteristic(BLE.INBOX);
      $("gostart").disabled = $("gokeep").disabled = false;
      $("gostate").textContent = "";
    } catch (e) {
      $("gostate").textContent = "This firmware takes no destinations yet: it needs JinxMesh from 2026-11-22 on.";
    }
    $("bleconnect").textContent = "Disconnect";
    tool(`Bluetooth: connected to ${device.name || "a device"}`);
    await bleOpen("/apps/wardrive");
  } catch (e) {
    if (e.name === "NotFoundError") return; // the chooser was closed
    tool(`Bluetooth: ${e.message}`, "err");
    $("blestate").textContent = `Failed: ${e.message}`;
  }
};
$("bleup").onclick = () => bleOpen(blePath.replace(/\/[^/]*$/, "") || "/apps");

// The tunes on the device, and the ones it comes with as a start.

// A file of the card, read block by block. Tunes are small; a log to keep (limit given) may be up to 64 MB.
async function readFile(link, path, limit = 1 << 20, onProgress = null) {
  const parts = [];
  let length = 0;
  for (;;) {
    const answer = await link.ask(FILES.READ, joined(le(length, 8), new TextEncoder().encode(path)), 10000, 3);
    if (!answer.payload.length) return joined(...parts);
    parts.push(answer.payload);
    length += answer.payload.length;
    if (onProgress) onProgress(length);
    if (length > Math.max(limit, 1 << 20) + 4096 || length > 64 << 20) throw new Error("that file is too large to read here");
  }
}

// hands bytes to the browser as a download
function saveHere(name, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: name.endsWith(".csv") ? "text/csv" : "application/octet-stream" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

function loadTune(text) {
  const clean = text.replace(/\s+/g, "");
  const parts = clean.split(":");
  if (parts.length === 3) {
    $("tunename").value = parts[0].replace(/[^A-Za-z0-9_-]/g, "").slice(0, 20) || "tune";
    const tempo = /b=(\d+)/i.exec(parts[1]);
    if (tempo) $("tunetempo").value = tempo[1];
  }
  $("tunetext").value = clean; // as it is: its own settings stay
  tuneChanged();
}

function showTunes(entries) {
  const list = $("tunelist");
  list.replaceChildren();
  const tunes = entries.filter((e) => !e.folder && !e.more && /\.rtttl$/i.test(e.name)).sort((a, b) => a.name.localeCompare(b.name));
  for (const entry of tunes) {
    const name = entry.name.replace(/^.*\//, "");
    const path = `/tones/${name}`;
    const row = document.createElement("div");
    row.className = "row";
    row.append(
      span("name", name.replace(/\.rtttl$/i, "")),
      span("grow", ""),
      button("Load", () => cardJob("reading the tune", async (link) => loadTune(new TextDecoder().decode(await readFile(link, path))))),
      button("Delete", async () => {
        if (!confirm(`Delete the tune ${name} from the device?`)) return;
        await cardJob("deleting the tune", async (link) => {
          await link.ask(FILES.DELETE, new TextEncoder().encode(path), 20000, 2);
          showTunes(await listFolder(link, "/tones"));
        });
      }),
    );
    list.append(row);
  }
  if (!tunes.length) list.append(span("empty", "No tunes of your own on the device yet."));
}

function refreshTunes() {
  return cardJob("reading the tunes", async (link) => showTunes(await listFolder(link, "/tones").catch(() => [])));
}

$("tunerefresh").onclick = refreshTunes;
fetch("ringtones.json")
  .then((r) => r.json())
  .then((tones) => {
    for (const tone of tones) {
      const option = document.createElement("option");
      option.textContent = tone.name;
      option.value = tone.rtttl;
      $("tunebuiltin").append(option);
    }
  })
  .catch(() => {});
$("tunebuiltin").onchange = () => {
  if ($("tunebuiltin").value) loadTune($("tunebuiltin").value);
  $("tunebuiltin").selectedIndex = 0;
};

// page

function button(text, onClick, disabled = false) {
  const b = document.createElement("button");
  b.textContent = text;
  b.disabled = disabled;
  b.onclick = () => onClick().catch((e) => tool(e.message, "err"));
  return b;
}

function span(cls, text) {
  const s = document.createElement("span");
  s.className = cls;
  s.textContent = text;
  return s;
}

function render() {
  const list = $("devices");
  list.replaceChildren();
  for (const device of devices.values()) {
    const row = document.createElement("div");
    row.className = "row";
    const pick = document.createElement("input");
    pick.type = "radio";
    pick.name = "device";
    pick.checked = device.id === selected;
    pick.setAttribute("aria-label", `Use device ${device.id} for flashing`);
    pick.onchange = () => {
      selected = device.id;
      render();
    };
    const state = device.state === "open" ? `monitoring, ${device.baud} baud` : device.state;
    row.append(
      pick,
      span("name", `Device ${device.id}`),
      span("detail", device.name),
      span(`state ${device.state}`, state),
      span("grow", ""),
      device.state === "open"
        ? button("Stop monitor", () => closeDevice(device))
        : button("Monitor", () => openDevice(device), device.state !== "closed"),
      button("Reset", () => resetDevice(device), device.state === "busy"),
      button("Forget", async () => {
        await closeDevice(device);
        await device.port.forget();
        await refresh();
      }, device.state === "busy"),
    );
    list.append(row);
  }
  if (!devices.size) list.append(span("empty", "None yet."));

  const target = devices.get(selected);
  const box = $("offers");
  box.replaceChildren();
  for (const offer of offers) {
    const row = document.createElement("div");
    row.className = "row";
    const files = offer.parts.map((p) => `${p.file} at 0x${p.offset.toString(16)} (${(p.size / 1024).toFixed(0)} KB)`);
    const text = document.createElement("div");
    text.className = "grow";
    text.append(span("name", offer.name), document.createElement("br"), span("detail", [offer.note, ...files].filter(Boolean).join(" · ")));
    const go = button(target ? `Flash to device ${target.id}` : "Flash", async () => {
      const result = await flash(target, offer, () => {});
      tool(`flashed ${result.bytes} bytes to ${result.chip}, device restarted`);
    }, !target || target.state === "busy");
    go.className = "primary";
    row.append(text, go);
    box.append(row);
  }
  if (!offers.length) box.append(span("empty", "Nothing offered right now."));
  fileControls();
  tuneChanged();
  const ready = !sending && target && target.state !== "busy";
  for (const id of ["cardshow", "cardup", "tunerefresh"]) $(id).disabled = !ready;
}

$("add").onclick = async () => {
  try {
    await navigator.serial.requestPort();
    await refresh();
  } catch (e) {
    if (e.name !== "NotFoundError") tool(e.message, "err"); // NotFoundError: the chooser was dismissed
  }
};
$("remote").onchange = report;
$("clear").onclick = () => $("terminal").replaceChildren();

if ("serial" in navigator) {
  navigator.serial.addEventListener("connect", refresh);
  navigator.serial.addEventListener("disconnect", refresh);
} else {
  $("unsupported").hidden = false;
  $("add").disabled = true;
}
render();
refresh();
connect();
