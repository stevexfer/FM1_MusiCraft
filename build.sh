#!/bin/sh
# FM-1 MusiCraft firmware: build app + loader -> build/musicraft.fwsc
# Needs the JieLi PI32 toolchain (~/.jieli/toolchain) and the AC79 SDK (as FM1_Mario / Felucca).
set -e
cd "$(dirname "$0")"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
mkdir -p build
python3 tools/build.py "$@"
