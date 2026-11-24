"""Download the USGS topo tiles of the area into a cache; rerun to resume.

usage: fetch.py CACHE_DIR [ZOOM_MAX]
The USGS National Map base maps are public domain and may be cached; the fetch still stays slow and identifies itself.
"""
import os
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

import area

URL = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}"
AGENT = "m9-offline-map-builder/1.0 (personal one-off for a handheld radio)"
WORKERS = 4


def fetch(job):
    z, x, y = job
    path = os.path.join(cache, str(z), str(x), f"{y}.jpg")
    if os.path.exists(path):
        return "had"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    for attempt in range(5):
        try:
            request = urllib.request.Request(URL.format(z=z, x=x, y=y), headers={"User-Agent": AGENT})
            with urllib.request.urlopen(request, timeout=40) as reply:
                data = reply.read()
            if data[:2] != b"\xff\xd8" and data[:4] != b"\x89PNG":
                raise ValueError("not an image")
            with open(path + ".part", "wb") as out:
                out.write(data)
            os.replace(path + ".part", path)
            return "new"
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return "missing"
            problems[f"HTTP {error.code}"] = problems.get(f"HTTP {error.code}", 0) + 1
            time.sleep(2 + 4 * attempt)
        except Exception as error:
            problems[type(error).__name__] = problems.get(type(error).__name__, 0) + 1
            time.sleep(2 + 4 * attempt)
    return "failed"


cache = sys.argv[1]
zoom_max = int(sys.argv[2]) if len(sys.argv) > 2 else area.ZOOM_MAX
jobs = [job for z in range(area.ZOOM_MIN, zoom_max + 1) for job in area.tiles(z)]
counts = {}
problems = {}
started = time.time()
with ThreadPoolExecutor(WORKERS) as pool:
    for done, result in enumerate(pool.map(fetch, jobs), 1):
        counts[result] = counts.get(result, 0) + 1
        if done % 500 == 0 or done == len(jobs):
            print(f"{done}/{len(jobs)} {counts} retried {problems} {time.time() - started:.0f} s", flush=True)
print("FETCH_DONE", counts, flush=True)
