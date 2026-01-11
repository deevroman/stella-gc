#!/usr/bin/env bash

set -u

mkdir -p "build"
mkdir -p "tmp"

BIN="./build/stella"
if [[ "$(uname -s)" == "Linux" ]]; then
    STELLA_COMPILER="./bin/stella-linux"
else
    STELLA_COMPILER="./bin/stella"
fi

MAX_ALLOC_SIZES=(
    $((1024*2))
    $((1024*64))
    $((1024*256))
    $((1024*1024))
    $((1024*1024*4))
    $((1024*1024*32))
)

for EXAMPLE in examples/*; do
    [[ -f "$EXAMPLE" ]] || continue

    EXAMPLE_NAME="$(basename "$EXAMPLE")"
    EXAMPLE_BASENAME="${EXAMPLE_NAME%.*}"

    OUT_FILE="$(pwd)/tmp/stella_run_${EXAMPLE_BASENAME}.log"
    echo "Example: $EXAMPLE_NAME" > "$OUT_FILE"
    echo "Started at: $(date)" >> "$OUT_FILE"

    "$STELLA_COMPILER" compile < "$EXAMPLE" > main.c

    for MAX_ALLOC_SIZE in "${MAX_ALLOC_SIZES[@]}"; do
        rm -rf build/*
        cmake -B build "-DMAX_ALLOC_SIZE=${MAX_ALLOC_SIZE}"
        cmake --build build

        for INPUT in $(seq 1 20); do
            echo -n "RUN: example=${EXAMPLE_NAME}, MAX_ALLOC_SIZE=${MAX_ALLOC_SIZE}, input=${INPUT} output=" >> "$OUT_FILE"

            MAX_ALLOC_SIZE="$MAX_ALLOC_SIZE" \
            echo "$INPUT" | "$BIN" >>"$OUT_FILE" 2>&1

            STATUS=$?

            if [[ "$STATUS" -ne 0 && "$STATUS" -ne 42 ]]; then
                echo >> "$OUT_FILE"
                echo "ERROR: unexpected exit code $STATUS" >> "$OUT_FILE"
                echo "Output file: $OUT_FILE"
                exit "$STATUS"
            fi
        done
    done

    echo "Finished successfully at: $(date)" >> "$OUT_FILE"
    echo >> "$OUT_FILE"
done

exit 0
