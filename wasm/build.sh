#!/bin/sh
# Build ancient as a WebAssembly module (ancient.mjs + ancient.wasm).
# Requires Emscripten (emcc/em++ on PATH, e.g. after `source emsdk_env.sh`).
# Usage: wasm/build.sh [output-dir]   (default: wasm/dist)
set -e
cd "$(dirname "$0")/.."
OUT="${1:-wasm/dist}"
mkdir -p "$OUT" wasm/obj
SRCS="$(awk '/^OBJS_N/{f=1} /^TEST_N/{f=0} f' Makefile.unix | tr -s ' \t\\=' '\n' | grep '\.o$' | sed 's/\.o$//')"
FLAGS="-O2 -std=c++17 -fwasm-exceptions -Isrc -Iapi -Iapi/ancient -DANCIENT_API_VISIBILITY_DEFAULT"
OBJS=""
for n in $SRCS; do
	src="src/$n.cpp"; [ -f "$src" ] || src="src/common/$n.cpp"
	obj="wasm/obj/$n.o"
	if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
		em++ $FLAGS -c "$src" -o "$obj" &
	fi
	OBJS="$OBJS $obj"
done
wait
em++ $FLAGS -c wasm/ancient_wasm.cpp -o wasm/obj/ancient_wasm.o
em++ $FLAGS $OBJS wasm/obj/ancient_wasm.o -o "$OUT/ancient.mjs" \
	-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createAncient \
	-sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
	-sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString \
	-sEXPORTED_FUNCTIONS=_anc_malloc,_anc_free,_anc_scan,_anc_decompress,_anc_identify,_anc_try_exact,_anc_out_ptr,_anc_name,_anc_out_release
echo "Built $OUT/ancient.mjs and $OUT/ancient.wasm"
