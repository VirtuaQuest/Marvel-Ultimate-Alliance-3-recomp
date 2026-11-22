#!/bin/sh
# Apply the requested Steam Deck display defaults without changing other settings.
set -eu
cd "$(dirname "$0")/.."
[ "${MUA3_DECK_NATIVE:-1}" != 0 ] || exit 0
settings=${LARECOMP_SETTINGS:-"$PWD/larecomp.ini"}
input=$settings
[ -f "$input" ] || input=/dev/null
umask 077
temp=$(mktemp "${settings}.deck-native.XXXXXX")
trap 'rm -f "$temp"' EXIT HUP INT TERM
awk '
BEGIN {
    value["window_width"]="1280"; value["window_height"]="800";
    value["resolution"]="1x"; value["fullscreen"]="1";
    value["exclusive_fullscreen"]="0";
    # first-run defaults only: the choice made in the menu is kept (FSR 1, anti-aliasing, aspect: stretch fills 16:10)
    initial["upscaler"]="bilinear"; initial["antialias"]="off"; initial["aspect"]="stretch";
}
{
    key=$0; sub(/=.*/, "", key); gsub(/^[ \t]+|[ \t]+$/, "", key);
    if (index($0,"=") && key in value) {
        if (!seen[key]++) print key " = " value[key];
    } else {
        if (index($0,"=") && key in initial) seen[key]++;
        print;
    }
}
END {
    for (key in value) if (!seen[key]) print key " = " value[key];
    for (key in initial) if (!seen[key]) print key " = " initial[key];
}
' "$input" > "$temp"
if [ -f "$settings" ] && cmp -s "$settings" "$temp"; then
    exit 0
fi
if [ -f "$settings" ] && [ ! -e "${settings}.before-deck-resolution" ]; then
    cp -p "$settings" "${settings}.before-deck-resolution"
fi
mv -f "$temp" "$settings"
echo '[linux-start] Deck display set to 1280x800, fullscreen, internal resolution 1x (upscaler and anti-aliasing: your menu choice; bilinear / off by default).'
