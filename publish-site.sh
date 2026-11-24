#!/bin/bash
# publish-site.sh [site folder]: refresh what the web page serves besides its own files: the firmware of the
# last bundle (publish-bundle.sh) with a short latest.json, and the list of published maps.
# The site is a checkout of the branch gh-pages-dev (default: ../jinxmesh-site); committing and pushing it,
# and moving gh-pages up to it, are done by hand.
set -euo pipefail
source "$(dirname "$0")/env.sh"
SITE=${1:-$HARNESS/../jinxmesh-site}
B=$HARNESS/webflash/static/m9-kb
[ -f "$B/latest.json" ] || { echo "no bundle in $B: run publish-bundle.sh first" >&2; exit 1; }
mkdir -p "$SITE/firmware"
rm -f "$SITE"/firmware/*.bin
python3 -I - "$B" "$SITE" <<'PY'
import json, shutil, sys
bundle, site = sys.argv[1:]
j = json.load(open(f"{bundle}/latest.json"))
out = {k: j[k] for k in ("board", "chip", "version", "build")}
out["update"], out["full"] = j["update"]["parts"][0], j["full"]["parts"][0]
for part in (out["update"], out["full"]):
    shutil.copy2(f"{bundle}/{part['file']}", f"{site}/firmware/{part['file']}")
json.dump(out, open(f"{site}/firmware/latest.json", "w"), indent=1)
print(f"firmware {out['version']} build {out['build']}")
PY
python3 -I "$HARNESS/maps/vector/site-index.py" "$SITE"
