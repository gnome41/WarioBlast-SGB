#!/bin/sh
# Adds WarioBlast Recomp to your desktop's application menu (current user only).
# Run it from the folder you unpacked; run it again if you move the folder.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
APPS=${XDG_DATA_HOME:-$HOME/.local/share}/applications
mkdir -p "$APPS"
cat > "$APPS/warioblast-recomp.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=WarioBlast Recomp
Comment=Unofficial static recompilation of Wario Blast (Game Boy, 1994)
Exec="$DIR/warioblast"
Path=$DIR
Icon=$DIR/warioblast.png
Terminal=false
Categories=Game;
DESKTOP
echo "Added $APPS/warioblast-recomp.desktop"
