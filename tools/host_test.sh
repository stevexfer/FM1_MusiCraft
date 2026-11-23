#!/bin/sh
# Host build + run of tools/host_test.c (the engine, natively): build/host/*.ppm, build/host/seq.wav
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -std=c99 -O2 -Wall -Wno-unused-function -Iengine engine/voxel.c engine/seq.c tools/host_test.c -o build/host/host_test
build/host/host_test "$@"
