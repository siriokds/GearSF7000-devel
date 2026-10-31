#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026  Saverio Russo
# The web player: the libretro core and player.cpp compiled with Emscripten
# into core.js and core.wasm, with the generic page of generic/.
#   platforms/web/build-player.sh [output folder]   (default platforms/web/dist-player)
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
out="${1:-platforms/web/dist-player}"
: "${EM_CACHE:=$root/platforms/web/.emcache}"
export EM_CACHE

make -C platforms/libretro -j "${WEB_JOBS:-4}" platform=emscripten \
    CC=emcc CXX=em++ AR=emar GIT_VERSION=web

mkdir -p "$out" platforms/web/build
cp platforms/libretro/gearsc3000_libretro_emscripten.bc platforms/web/build/core.a
em++ -O3 -std=c++20 platforms/web/player.cpp \
    -Wl,--whole-archive platforms/web/build/core.a -Wl,--no-whole-archive \
    -sMODULARIZE=1 -sEXPORT_NAME=createGearSC3000 -sALLOW_MEMORY_GROWTH=1 \
    -sENVIRONMENT=web,node -sASSERTIONS=0 \
    -sEXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
    -sEXPORTED_FUNCTIONS='["_malloc","_free","_web_init","_web_load","_web_run","_web_reset","_web_set_mapper","_web_set_asc16l_ram","_web_set_timing","_web_set_borders","_web_set_target_fps","_web_pad","_web_key","_web_width","_web_height","_web_pixels","_web_audio","_web_audio_frames","_web_fps","_web_ram_bytes","_web_is_internal_mapper","_web_is_asc16l_mapper"]' \
    -o "$out/core.js"
cp platforms/web/generic/index.html platforms/web/generic/player.js "$out/"
