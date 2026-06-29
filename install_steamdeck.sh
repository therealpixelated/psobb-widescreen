#!/usr/bin/env bash
# install_steamdeck.sh — install the psobb-widescreen ASI on Linux / Steam Deck
# (PSOBB.IO under Proton or Wine). The mod is a drop-in .asi; this copies it (plus
# the .ini and the optional boot-poster PNG) into <gamedir>/patches/, backing up
# anything it would overwrite. Run it from the unzipped release folder.
#
#   ./install_steamdeck.sh /path/to/your/PSOBB.IO
#
# NOTE: untested by us — Steam Deck / Wine reports are welcome.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="${1:-}"

if [ -z "$GAMEDIR" ]; then
  echo "Usage: $0 /path/to/PSOBB.IO"
  echo "  (the folder that contains psobb.exe and a patches/ directory)"
  exit 1
fi

GAMEDIR="${GAMEDIR%/}"
if [ ! -d "$GAMEDIR/patches" ]; then
  echo "error: no 'patches/' directory under '$GAMEDIR'."
  echo "       point me at your PSOBB.IO folder (the one with psobb.exe)."
  exit 1
fi

echo "Installing psobb-widescreen into: $GAMEDIR/patches/"
for f in pso_widescreen.asi pso_widescreen.ini psobb_boot_poster.png; do
  [ -f "$HERE/$f" ] || continue
  dst="$GAMEDIR/patches/$f"
  if [ -f "$dst" ]; then
    cp -f "$dst" "$dst.bak"
    echo "  backed up $f -> $f.bak"
  fi
  cp -f "$HERE/$f" "$dst"
  echo "  installed $f"
done

cat <<'EOF'

Done.

If your .asi mods aren't loading under Proton/Wine, your ASI loader's proxy DLL
needs a DLL override. Add this to the game's Steam launch options (replace <loader>
with your loader's proxy-DLL base name — e.g. dinput8, version, winmm, or whatever
your PSOBB.IO install already uses):

    WINEDLLOVERRIDES="<loader>=n,b" %command%

Then launch as usual. Widescreen auto-enables from your display aspect.
EOF
