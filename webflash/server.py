#!/usr/bin/env python3
"""webflash server: lets this machine use serial devices plugged into a browser's computer.

The page under /pub/ opens Web Serial ports on the computer that loads it and keeps a
WebSocket to this server. Commands arrive on /ctl/ (loopback only, never proxied) from
the `webflash` CLI; the page runs them (monitor, reset, esptool flash) and streams the
output back. Bytes read from an open port are appended to logs/ and offered on a raw
TCP port, so pyserial tools can use `socket://127.0.0.1:4404`.
"""

import argparse
import asyncio
import itertools
import json
import secrets
import time
from pathlib import Path

from aiohttp import WSMsgType, web

ROOT = Path(__file__).resolve().parent
STATIC = ROOT / "static"
LOGS = ROOT / "logs"


class Problem(Exception):
    """A request that cannot be carried out; the text goes back to the caller."""


class Client:
    """One open page."""

    def __init__(self, cid, ws, addr, agent):
        self.id = cid
        self.ws = ws
        self.addr = addr
        self.agent = agent
        self.since = time.time()
        self.serial = False  # browser has Web Serial
        self.remote = False  # page allows commands from here
        self.ports = []  # as last reported by the page
        self.pending = {}  # command id -> queue of the call that waits for the reply
        self.listeners = set()  # queues of calls that follow this page's output

    def log_path(self, port):
        return LOGS / f"{self.id}-port{port}.log"

    def info(self):
        return {
            "id": self.id,
            "addr": self.addr,
            "agent": self.agent,
            "since": self.since,
            "serial": self.serial,
            "remote": self.remote,
            "ports": [dict(p, log=str(self.log_path(p["id"]))) for p in self.ports],
        }


class Hub:
    def __init__(self):
        self.clients = {}
        self.offers = {}  # name -> {"name", "note", "token", "parts": [{"path", "offset"}]}
        self.ids = itertools.count(1)
        self.tcp = set()  # writers of raw TCP bridge connections

    # pages

    def add(self, ws, request):
        addr = request.headers.get("X-Forwarded-For", request.remote)
        client = Client(f"c{next(self.ids)}", ws, addr, request.headers.get("User-Agent", ""))
        self.clients[client.id] = client
        print(f"{client.id} connected from {addr}", flush=True)
        return client

    def remove(self, client):
        self.clients.pop(client.id, None)
        for queue in client.pending.values():
            queue.put_nowait({"t": "reply", "ok": False, "error": "the page went away"})
        print(f"{client.id} disconnected", flush=True)

    def pick(self, cid=None, port=None, need_open=False):
        """Resolve the page and device a command is meant for."""
        if cid:
            if cid not in self.clients:
                raise Problem(f"no page {cid} is connected")
            client = self.clients[cid]
        else:
            clients = [c for c in self.clients.values() if c.ports] or list(self.clients.values())
            if not clients:
                raise Problem("no page is connected; open the flasher page in a Chromium browser")
            client = clients[-1]
        if not client.remote:
            raise Problem(f"remote control is switched off on page {client.id}")
        ports = [p for p in client.ports if p["state"] == "open"] if need_open else client.ports
        if port is None:
            candidates = ports if len(ports) == 1 else [p for p in ports if p["state"] == "open"]
            if len(candidates) != 1:
                what = "open devices" if need_open else "devices"
                raise Problem(f"page {client.id} has {len(ports)} {what}; " + ("pass --port" if ports else "add one on the page"))
            port = candidates[0]["id"]
        elif port not in [p["id"] for p in ports]:
            raise Problem(f"page {client.id} has no {'open ' if need_open else ''}device {port}")
        return client, port

    def page_message(self, client, m):
        kind = m.get("t")
        if kind == "hello":
            client.serial = bool(m.get("serial"))
        elif kind == "ports":
            client.ports = m.get("ports", [])
            client.remote = bool(m.get("remote"))
        elif kind == "reply":
            queue = client.pending.pop(m.get("id"), None)
            if queue:
                queue.put_nowait(m)
        elif kind in ("log", "progress"):
            if kind == "log":  # also kept for flashes started on the page itself
                with open(LOGS / f"{client.id}-events.log", "a") as log:
                    log.write(m.get("text", "") + "\n")
            for queue in client.listeners:
                queue.put_nowait(m)

    # serial data

    def serial_in(self, client, port, data):
        with open(client.log_path(port), "ab") as log:
            log.write(data)
        for writer in list(self.tcp):
            writer.write(data)

    async def serial_out(self, data, cid=None, port=None):
        client, port = self.pick(cid, port, need_open=True)
        await client.ws.send_bytes(bytes([port]) + data)
        return client, port

    async def bridge(self, reader, writer):
        self.tcp.add(writer)
        try:
            while data := await reader.read(4096):
                try:
                    await self.serial_out(data)
                except Problem as e:
                    print(f"bridge: dropped {len(data)} bytes, {e}", flush=True)
        except ConnectionError:
            pass
        finally:
            self.tcp.discard(writer)
            writer.close()

    # firmware offers

    def offer(self, name, parts, note=""):
        checked = []
        for part in parts:
            path = Path(part["path"]).resolve()
            if not path.is_file():
                raise Problem(f"{path} is not a file")
            checked.append({"path": str(path), "offset": int(part["offset"])})
        if not checked:
            raise Problem("an offer needs at least one OFFSET:FILE part")
        self.offers[name] = {"name": name, "note": note, "token": secrets.token_hex(8), "parts": checked}
        return self.offers[name]

    def offer_view(self, offer):
        """An offer as the page sees it: URLs relative to the page, no server paths."""
        parts = []
        for i, part in enumerate(offer["parts"]):
            path = Path(part["path"])
            parts.append(
                {
                    "url": f"fw/{offer['token']}/{i}",
                    "offset": part["offset"],
                    "file": path.name,
                    "size": path.stat().st_size if path.is_file() else 0,
                }
            )
        return {"name": offer["name"], "note": offer["note"], "parts": parts}

    async def announce_offers(self):
        offers = [self.offer_view(o) for o in self.offers.values()]
        for client in list(self.clients.values()):
            try:
                await client.ws.send_json({"t": "offers", "offers": offers})
            except ConnectionError:
                pass


# routes the browser reaches (proxied as /flash/)


async def index(request):
    return web.FileResponse(STATIC / "index.html", headers={"Cache-Control": "no-cache"})


async def page_socket(request):
    hub = request.app["hub"]
    ws = web.WebSocketResponse(heartbeat=20, max_msg_size=4 * 1024 * 1024)
    await ws.prepare(request)
    client = hub.add(ws, request)
    offers = [hub.offer_view(o) for o in hub.offers.values()]
    await ws.send_json({"t": "welcome", "id": client.id, "offers": offers})
    try:
        async for msg in ws:
            if msg.type == WSMsgType.BINARY and len(msg.data) > 1:
                hub.serial_in(client, msg.data[0], msg.data[1:])
            elif msg.type == WSMsgType.TEXT:
                hub.page_message(client, json.loads(msg.data))
    finally:
        hub.remove(client)
    return ws


async def firmware(request):
    hub = request.app["hub"]
    for offer in hub.offers.values():
        if offer["token"] == request.match_info["token"]:
            index = int(request.match_info["index"])
            if index < len(offer["parts"]):
                return web.FileResponse(offer["parts"][index]["path"], headers={"Cache-Control": "no-store"})
    raise web.HTTPNotFound()


# control routes (loopback only)


@web.middleware
async def control_guard(request, handler):
    if request.path.startswith("/ctl/"):
        # the reverse proxy only maps /pub/; anything forwarded here is a mistake
        if "X-Forwarded-For" in request.headers or request.remote not in ("127.0.0.1", "::1"):
            raise web.HTTPForbidden()
        try:
            return await handler(request)
        except Problem as e:
            return web.json_response({"error": str(e)}, status=409)
    return await handler(request)


async def ctl_status(request):
    hub = request.app["hub"]
    return web.json_response(
        {
            "clients": [c.info() for c in hub.clients.values()],
            "offers": list(hub.offers.values()),
            "bridge": request.app["bridge"],
        }
    )


async def ctl_pick(request):
    hub = request.app["hub"]
    port = request.query.get("port")
    client, port = hub.pick(request.query.get("client"), int(port) if port else None)
    return web.json_response({"client": client.id, "port": port, "log": str(client.log_path(port))})


async def ctl_offer(request):
    hub = request.app["hub"]
    body = await request.json()
    if body.get("remove"):
        if hub.offers.pop(body["name"], None) is None:
            raise Problem(f"no offer named {body['name']}")
    else:
        hub.offer(body["name"], body["parts"], body.get("note", ""))
    await hub.announce_offers()
    return web.json_response({"offers": list(hub.offers.values())})


async def ctl_write(request):
    hub = request.app["hub"]
    body = await request.json()
    client, port = await hub.serial_out(bytes.fromhex(body["hex"]), body.get("client"), body.get("port"))
    return web.json_response({"client": client.id, "port": port})


async def ctl_cmd(request):
    """Run one command on a page; the response is a stream of JSON lines ending with the reply."""
    hub = request.app["hub"]
    body = await request.json()
    client, port = hub.pick(body.get("client"), body.get("port"))
    cmd = {"t": "cmd", "id": next(hub.ids), "op": body["op"], "port": port, **body.get("args", {})}
    if cmd["op"] == "flash":
        if "parts" in cmd:
            hub.offer(cmd["name"], cmd["parts"], cmd.get("note", ""))
            await hub.announce_offers()
        if cmd["name"] not in hub.offers:
            raise Problem(f"no offer named {cmd['name']}")
        cmd["parts"] = hub.offer_view(hub.offers[cmd["name"]])["parts"]

    response = web.StreamResponse(headers={"Content-Type": "application/x-ndjson"})
    await response.prepare(request)
    queue = asyncio.Queue()
    client.pending[cmd["id"]] = queue
    client.listeners.add(queue)
    try:
        await client.ws.send_json(cmd)
        while True:
            try:
                m = await asyncio.wait_for(queue.get(), body.get("timeout", 600))
            except asyncio.TimeoutError:
                m = {"t": "reply", "ok": False, "error": "timed out waiting for the page"}
            await response.write(json.dumps(dict(m, client=client.id, port=port)).encode() + b"\n")
            if m["t"] == "reply":
                break
    finally:
        client.listeners.discard(queue)
        client.pending.pop(cmd["id"], None)
    await response.write_eof()
    return response


async def start_bridge(app):
    hub = app["hub"]
    app["bridge_server"] = await asyncio.start_server(hub.bridge, "127.0.0.1", app["bridge"])


async def stop_bridge(app):
    app["bridge_server"].close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8377, help="HTTP port on 127.0.0.1")
    parser.add_argument("--bridge", type=int, default=4404, help="raw TCP port on 127.0.0.1 for the open device")
    args = parser.parse_args()

    LOGS.mkdir(exist_ok=True)
    app = web.Application(middlewares=[control_guard])
    app["hub"] = Hub()
    app["bridge"] = args.bridge
    app.on_startup.append(start_bridge)
    app.on_cleanup.append(stop_bridge)
    app.add_routes(
        [
            web.get("/pub/", index),
            web.get("/pub/ws", page_socket),
            web.get("/pub/fw/{token}/{index:\\d+}", firmware),
            web.static("/pub/", STATIC),
            web.get("/ctl/status", ctl_status),
            web.get("/ctl/pick", ctl_pick),
            web.post("/ctl/offer", ctl_offer),
            web.post("/ctl/write", ctl_write),
            web.post("/ctl/cmd", ctl_cmd),
        ]
    )
    web.run_app(app, host="127.0.0.1", port=args.port, print=None)


if __name__ == "__main__":
    main()
