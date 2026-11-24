#!/bin/bash
# build-states.sh <country>/<state>...: vector maps of whole states or provinces for the device, one file
# each, into ~/.cache/m9-tiles/vector/states/<state>.jmv. They are named as their extracts at
# download.geofabrik.de/north-america are (us/new-york, us/vermont, canada/quebec). What a state needs is
# fetched when it is not there yet: its extract and outline, and the extracts of its neighbors, from which
# the cells along its borders are filled (regions.py). See build.py next to this file.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${M9_TILES:-$HOME/.cache/m9-tiles}
FONT=${FONT:-$HOME/github_forks/device-ui/include/util/VectorMapFont.h}
WORKERS=${WORKERS:-16}
mkdir -p "$ROOT/vector/states" "$ROOT/osm"
for region in "$@"; do
    state=${region##*/}
    echo "$(date +%H:%M:%S) preparing $region"
    args=$("$ROOT/venv/bin/python" -I "$HERE/regions.py" "$ROOT/osm" "$region") || { echo "STATE $state FAILED: could not fetch its data"; continue; }
    echo "$(date +%H:%M:%S) building $state"
    # $args: file names without spaces
    if "$ROOT/venv/bin/python" -I "$HERE/build.py" "$ROOT/vector/states/$state.jmv" "$FONT" "$ROOT/vector/work/all" --workers "$WORKERS" $args; then
        echo "STATE $state DONE $(stat -c %s "$ROOT/vector/states/$state.jmv") bytes"
    else
        echo "STATE $state FAILED"
    fi
done
echo "ALL DONE"
