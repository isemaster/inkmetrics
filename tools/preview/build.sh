#!/usr/bin/env bash
# Сборка стенда: настоящий screen.c + шрифты из fonts.h + заглушка панели.
# Запускать из WSL (там есть gcc): bash tools/preview/build.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MAIN="$ROOT/idf/main"

gcc -std=c99 -Wall -Wextra -O1 \
    -I"$MAIN" -I"$HERE/stub" \
    "$MAIN/screen.c" "$HERE/host_display.c" "$HERE/preview_main.c" \
    -o "$HERE/preview"

cd "$HERE"
./preview
echo "готово: $HERE/summary.pgm, setup.pgm, offline.pgm, nodata.pgm"
