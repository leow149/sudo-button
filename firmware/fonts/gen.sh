#!/bin/sh
# Needs Node (npx) and the Hack font installed (or HACK_TTF=/path/Hack-Regular.ttf). Regenerate the command font (Hack Regular, 20px, printable ASCII only: the
# device escapes everything else as \xNN, so no other glyphs are needed).
cd "$(dirname "$0")" && npx --yes lv_font_conv --font "${HACK_TTF:-/usr/share/fonts/TTF/Hack-Regular.ttf}" --size 20 --bpp 4 \
    --range 0x20-0x7E --format lvgl --no-compress --lv-font-name lv_font_hack_20 \
    --lv-include lvgl.h -o ../src/lv_font_hack_20.c
