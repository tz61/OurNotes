#!/usr/bin/env bash
# Downloads the twelve BanG Dream! bands, from Poppin'Party to Ikka Dumb Rock!, from the
# BanG Dream! wiki (bandori.miraheze.org) into <data dir>/backgrounds/bands/. OurNotes
# shows one of them behind the item list, a different one every two hours: Poppin'Party
# from midnight, Afterglow from 2:00, ... Ikka Dumb Rock! from 22:00 (Preferences >
# General turns it off).
#
# The pictures belong to their owners (Bushiroad); they are fetched for your own use and
# are not part of OurNotes or its packages.
#
# Usage: scripts/fetch-band-backgrounds.sh [DATA_DIR]   (default: $OURNOTES_DATA_DIR or ~/OurNotes)
set -euo pipefail

DATA_DIR="${1:-${OURNOTES_DATA_DIR:-$HOME/OurNotes}}"
DEST="$DATA_DIR/backgrounds/bands"
WIKI="https://bandori.miraheze.org/w/index.php?title=Special:FilePath"
UA="OurNotes (band backgrounds; https://github.com/tz61/OurNotes)"

# slot (two hours each, from midnight) | name on disk | the wiki's file
BANDS=(
    "01|Poppin'Party|Poppin'Party_artist_cover.jpg"
    "02|Afterglow|Afterglow_artist_cover.jpg"
    "03|Pastel Palettes|Pastel_Palettes_artist_cover.jpg"
    "04|Roselia|Roselia_artist_cover.jpg"
    "05|Hello, Happy World!|ハロー、ハッピーワールド!_artist_cover.jpg"
    "06|Morfonica|Morfonica_artist_cover.jpg"
    "07|RAISE A SUILEN|RAISE_A_SUILEN_artist_cover.jpg"
    "08|MyGO!!!!!|MyGO!!!!!_artist_cover.jpg"
    "09|Ave Mujica|Ave_Mujica_artist_cover.jpg"
    "10|Mugendai MewType|夢限大みゅーたいぷ_artist_cover.jpg"
    "11|Millsage|BanG_Dream!_Our_Notes_millsage.png"
    "12|Ikka Dumb Rock!|BanG_Dream!_Our_Notes_Ikka_Dumb_Rock!.png"
)

command -v curl >/dev/null || { echo "error: curl is required" >&2; exit 1; }
mkdir -p "$DEST"
for entry in "${BANDS[@]}"; do
    IFS='|' read -r slot name file <<<"$entry"
    out="$DEST/$slot $name.${file##*.}"
    if [ -s "$out" ]; then
        echo "    have $slot $name"
        continue
    fi
    echo "==> $slot $name"
    # the file name as a parameter, so curl encodes it (some are Japanese)
    curl -fsSL --retry 3 -A "$UA" -G --data-urlencode "file=$file" -o "$out.part" "$WIKI"
    mv -f "$out.part" "$out"
done
echo "==> Done: $DEST"
