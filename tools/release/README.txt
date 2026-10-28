WarioBlast Recomp @VERSION@
=======================

An unofficial static recompilation of Wario Blast Featuring Bomberman!
(Game Boy, 1994), with Super Game Boy colours, border and sound, 4-player
Super Game Boy battles with one controller per player, widescreen views
and controller support. Not affiliated with, endorsed by or sponsored by
Nintendo or Konami (Hudson Soft). Wario, Game Boy and Super Game Boy are
trademarks of Nintendo; Bomberman is a trademark of Konami.

This package contains NO game code or data. You need your own copy of:

  * the game ROM (required):
      Wario Blast Featuring Bomberman! (USA, Europe) (SGB Enhanced).gb
      CRC32 927B57A1, SHA-1 279fb0223e362db553b739b1b8f9c18b81d92413
  * a Super Game Boy BIOS (optional, adds the SGB's SNES sound effects):
      Super Game Boy (World).sfc or another revision

dumped from hardware you own.

How it works: when you press PLAY, the player recompiles YOUR ROM's code to
C and compiles it in memory with TinyCC (about a third of a second). Nothing
derived from the game is shipped with this program or written to disk.

Running
-------
Windows: run warioblast.exe. Keep libtcc.dll and SDL2.dll next to it.
Linux:   run ./warioblast. Keep libtcc.so next to it. Needs SDL2 from your
         distribution (e.g. Debian/Ubuntu: libsdl2-2.0-0) and glibc 2.34+.
         ./install-desktop-entry.sh adds it (with its icon) to your
         application menu.

The launcher opens. Put the ROM (and BIOS) next to the program or choose
them with Browse... / drag and drop; the launcher checks them, then press
PLAY. Settings are remembered.

Controls
--------
  Game Boy    Keyboard                    Controller
  D-pad       arrow keys                  D-pad or left stick
  A           X or K                      A / Y
  B           Z or J                      B / X
  Start       Enter                       Start
  Select      Right Shift / Backspace     Back
  fast-fwd    Tab (hold)                  RB or RT (hold)
  view        F10                         LB
  fullscreen  F11                         Guide
  cheats      F8
  mods        F9
  quit        Esc

Battle mode on the Super Game Boy has up to 4 players: the first
controller (and the keyboard) is player 1, the second controller player 2,
and so on. Outside multiplayer every controller drives player 1.

The game has no battery save; it uses passwords (title screen).

Mods: put mod packs in the mods/ folder and enable them in the launcher
(MOD PACKS > MANAGE...). F9 toggles them in game. See MODDING.md.
mods/WarioBlast-Example is an example pack (HD border, HD status-bar digits,
a new first-stage tune) - enable it to try mods, or copy it to start your own.

Cheats: GameShark (01VVLLHH) and Game Genie (ABC-DEF-GHI) codes, under
CHEATS > MANAGE... in the launcher. "Never lose a bout" and a timer
freeze are built in. F8 toggles cheats in game.

Stage select: START STAGE in the launcher starts new Normal games at any
round, 1-1 to 8-4 (Page Up / Down: a world at a time). Passwords still work.

Command line: warioblast --help (any option skips the launcher).

Licenses
--------
  WarioBlast Recomp  MIT       LICENSE.txt
  TinyCC             LGPL-2.1  licenses/TinyCC-COPYING.txt,
                     (libtcc.dll / libtcc.so, unmodified 0.9.27; source in
                      source/tcc-0.9.27.tar.bz2 - you may replace the library
                      with your own build)
  SDL2               zlib      licenses/SDL2-LICENSE.txt (Windows)

Source code: the WarioBlast-Recomp repository.
