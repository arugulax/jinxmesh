"""pagecheck.py <path to a headless chrome>: loads the flasher page from the server and prints every script error,\nthen whether it connected and how many firmware offers it shows. Run it after changing static/app.js:\n  .venv/bin/python pagecheck.py ~/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell\n"""
import asyncio, json, subprocess, sys, urllib.request, time
import aiohttp
C=sys.argv[1]
p=subprocess.Popen([C,"--no-sandbox","--disable-gpu","--remote-debugging-port=9333","about:blank"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
time.sleep(2)
async def main():
    tabs=json.load(urllib.request.urlopen("http://127.0.0.1:9333/json"))
    async with aiohttp.ClientSession() as s:
        ws=await s.ws_connect(tabs[0]["webSocketDebuggerUrl"],max_msg_size=0)
        i=0
        async def cmd(m,**pa):
            nonlocal i; i+=1
            await ws.send_str(json.dumps({"id":i,"method":m,"params":pa}))
        await cmd("Runtime.enable"); await cmd("Log.enable"); await cmd("Page.enable")
        await cmd("Page.navigate",url="http://127.0.0.1:8377/pub/")
        end=time.time()+6
        while time.time()<end:
            try:
                m=json.loads((await asyncio.wait_for(ws.receive(),0.5)).data)
            except asyncio.TimeoutError: continue
            if m.get("method") in ("Runtime.exceptionThrown","Runtime.consoleAPICalled","Log.entryAdded"):
                print(json.dumps(m["params"])[:700])
        await cmd("Runtime.evaluate",expression="document.getElementById('link').textContent + ' | offers: ' + document.getElementById('offers').children.length")
        for _ in range(30):
            m=json.loads((await ws.receive()).data)
            if m.get("id")==i: print(m["result"]["result"].get("value")); break
try:
    asyncio.run(main())
finally:
    p.kill()
