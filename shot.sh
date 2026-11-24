#!/bin/bash
# shot.sh <name> [keys...]: send keys to the simulator window, then save $S/kb/<name>.png at twice the size
# Keys are xdotool names, 0.35 s apart. Faster than about 60 ms and Xvfb drops most of them.
source "$(dirname "$0")/env.sh"
name=$1; shift
win=$(xdotool search --onlyvisible --name . | head -1)
for k in "$@"; do xdotool key --window $win $k 2>/dev/null || xdotool key $k; sleep 0.35; done
sleep 0.6
import -window root -crop 320x240+0+0 +repage -filter point -resize 200% "$S/kb/$name.png"
