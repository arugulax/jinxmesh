#!/bin/bash
# publish-bundle.sh: copy the last M9 keyboard-view build, its checksums, manifests, FORK.md and the
# source patches into webflash/static/m9-kb/, the folder a flasher or a release is fed from.
# Both checkouts must be committed, and the build must come from the firmware commit: the patches
# are the difference between the upstream commits and the fork branches, nothing else.
set -euo pipefail
source "$(dirname "$0")/env.sh"
ENV=thinknode_m9-tft-kb
B=$FW/.pio/build/$ENV
OUT=$HARNESS/webflash/static/m9-kb
KEEP="BRIEF.md FORK.md HANDOFF.md"

for tree in "$FW" "$UI"; do
    if [ -n "$(git -C "$tree" status --porcelain)" ]; then
        echo "uncommitted changes in $tree: commit them and rebuild first" >&2
        exit 1
    fi
done
FW_HEAD=$(git -C "$FW" rev-parse --short=7 HEAD)
UI_HEAD=$(git -C "$UI" rev-parse --short=7 HEAD)
FW_BASE=$(git -C "$FW" merge-base HEAD origin/develop | cut -c1-7)
UI_BASE=$(git -C "$UI" merge-base HEAD origin/master | cut -c1-7)

# the version in the file name ends with the firmware commit the build was made from
APP=$(ls -t "$B"/firmware-$ENV-*.bin | command grep -v factory | head -1)
NAME=$(basename "$APP" .bin)
VERSION=${NAME#firmware-$ENV-}
BUILD=$(date -d "@$(stat -c %Y "$APP")" +%Y-%m-%d-%H%M)
if [ "${VERSION##*.}" != "$FW_HEAD" ]; then
    echo "the last build is $VERSION, the firmware branch is at $FW_HEAD: rebuild first" >&2
    exit 1
fi
# device-ui is linked into the build, its commit is in no file name: compare times instead
if [ "$(git -C "$UI" log -1 --format=%ct)" -gt "$(stat -c %Y "$APP")" ] && [ "${1:-}" != "--force" ]; then
    echo "device-ui was committed after the last build: rebuild, or pass --force if only documents changed" >&2
    exit 1
fi
[ -f "$OUT/FORK.md" ] || { echo "$OUT/FORK.md is missing" >&2; exit 1; }

mkdir -p "$OUT/source"
find "$OUT" -maxdepth 1 -type f $(printf '! -name %s ' $KEEP) -delete
rm -f "$OUT"/source/*
cp -p "$APP" "$B/$NAME.factory.bin" "$B/$NAME.mt.json" "$B/littlefs-$ENV-$VERSION.bin" "$B/bootloader.bin" "$B/partitions.bin" "$OUT/"

git -C "$FW" diff "$FW_BASE" HEAD > "$OUT/source/firmware-$FW_BASE-m9-kb.patch"
git -C "$UI" diff "$UI_BASE" HEAD > "$OUT/source/device-ui-$UI_BASE-kbview.patch"

python3 - "$OUT" "$NAME" "$VERSION" "$BUILD" "$FW_BASE" "$UI_BASE" "$FW_HEAD" "$UI_HEAD" <<'PY'
import hashlib, json, os, sys
out, name, version, build, fw_base, ui_base, fw_head, ui_head = sys.argv[1:]
env = "thinknode_m9-tft-kb"

def entry(file, offset=None, role=""):
    data = open(os.path.join(out, file), "rb").read()
    e = {"file": file, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "md5": hashlib.md5(data).hexdigest()}
    if offset is not None:
        e["offset"] = offset
    if role:
        e["role"] = role
    return e

app, factory = name + ".bin", name + ".factory.bin"
latest = {
    "target": env,
    "board": "Elecrow ThinkNode M9",
    "chip": "ESP32-S3",
    "flashSize": "16MB",
    "version": version,
    "build": build,
    # the upstream commits the fork sits on
    "firmwareCommit": fw_base,
    "deviceUiCommit": ui_base,
    # the local commits this build was made from
    "fork": {"firmware": fw_head, "firmwareBranch": "m9-kb", "deviceUi": ui_head, "deviceUiBranch": "kbview"},
    "update": {"keepsSettings": True, "eraseAll": False, "parts": [entry(app, "0x10000")]},
    "full": {"keepsSettings": False, "eraseAll": True, "parts": [entry(factory, "0x0")]},
    "other": [
        entry("littlefs-%s-%s.bin" % (env, version), "0xc90000", "empty filesystem image; optional, the firmware formats the partition itself"),
        entry("bootloader.bin", "0x0", "already inside the factory image"),
        entry("partitions.bin", "0x8000", "already inside the factory image"),
        entry(name + ".mt.json", role="build manifest in the upstream Meshtastic format"),
        entry("FORK.md", role="what the fork is: features, keys, build, test harness, interfaces"),
    ],
    "source": [entry("source/" + f) for f in sorted(os.listdir(os.path.join(out, "source")))],
}
json.dump(latest, open(os.path.join(out, "latest.json"), "w"), indent=2)

label = "ThinkNode M9 keyboard view (unofficial)"
for file, parts, erase in (("manifest-update.json", [{"path": app, "offset": 0x10000}], False),
                           ("manifest-full.json", [{"path": factory, "offset": 0}], True)):
    manifest = {"name": label, "version": "%s build %s" % (version, build), "new_install_prompt_erase": erase,
                "builds": [{"chipFamily": "ESP32-S3", "parts": parts}]}
    json.dump(manifest, open(os.path.join(out, file), "w"), indent=2)
PY

(cd "$OUT" && find . -type f ! -name SHA256SUMS ! -name BRIEF.md | sort | sed 's|^\./||' | xargs sha256sum > SHA256SUMS)
echo "published $VERSION build $BUILD to $OUT"
