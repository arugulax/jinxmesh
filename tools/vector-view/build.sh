#!/bin/bash
# build.sh: the desktop viewer of the vector map, with the address and bounds checks of the compiler switched on
source "$(dirname "$0")/../../env.sh"
g++ -O1 -g -std=c++17 -fsanitize=address,undefined -I "$UI/include" -o "$S/vector-view" "$(dirname "$0")/vector-view.cpp" "$UI/source/util/VectorMap.cpp" && echo "$S/vector-view"
