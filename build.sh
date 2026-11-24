#!/bin/bash
# build.sh: rebuild the simulator (env native-tft-kb) detached; $S/build-kb.log ends with BUILD_EXIT=<code>
source "$(dirname "$0")/env.sh"
pkill -x meshtasticd
# the About page shows when the build was made: recompile the file that holds the date
touch "$UI/source/graphics/TFT/KeyboardViewBuildStamp.cpp"
setsid nohup bash -c "cd $FW && TMPDIR=$PIO_TMP $PIO run -e native-tft-kb -j $JOBS > $S/build-kb.log 2>&1; echo BUILD_EXIT=\$? >> $S/build-kb.log" > /dev/null 2>&1 &
disown
echo "log: $S/build-kb.log"
