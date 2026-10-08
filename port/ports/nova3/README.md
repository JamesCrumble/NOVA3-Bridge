# N.O.V.A. 3: Near Orbit Vanguard Alliance — PortMaster port

Native Android→Linux port of Gameloft's 2012 science-fiction shooter for ARM
handhelds (R36S and family). Bring your own game: install the release ZIP
through PortMaster's autoinstall, drop your own v1.0.7 APK plus both OBB
expansion files in `ports/nova3/`, and the first launch imports and validates
them automatically. The OBBs are accepted as loose files, as an `Android/obb`
backup zip, or as that backup's extracted package-named folder — whichever
your copy came as. No game data is ever distributed.

This README's controls section is the authoritative map — the game was
designed for a touchscreen plus the Xperia Play's and MOGA's controls, and
this port spells all of it with buttons.

## Controls

### Combat

| Control | Action |
|---|---|
| Left stick | Move |
| Right stick | Camera / aim |
| R1 or R2 | Fire |
| L1 or L2 | Aim down sights |
| A | Throw grenade / throw the selected item |
| B | Jump / interact / fly |
| X | Cycle items |
| Y | Use power |
| D-pad Up | Sprint |
| D-pad Down | Quick weapon change |
| D-pad Right | Reload |
| L3 (left stick click) | Weapon change |
| R3 (right stick click) | Action |
| Start | Pause menu |

The engine ignores a button whose action does not apply right now — no
grenades carried, a full magazine, nothing to interact with. That is the game,
not a missing binding.

D-pad Left sends a key the engine accepts but whose effect has not been
identified on hardware yet. It is left bound rather than removed, so that a
player who works out what it does can say so.

### Menus

Menu screens are touch-first, so the port raises a pointer there
automatically — no modifier to hold. The engine itself is asked whether it is
in a menu, so the switch happens on its terms, not on a guess:

| Control (in menus) | Action |
|---|---|
| Right stick | Move the pointer |
| A, L2 or R2 | Tap at the pointer |
| D-pad | Menu navigation |
| Select | Accept |

### Tuning (launcher environment variables)

| Variable | Default | Effect |
|---|---|---|
| `NOVA3_RPAD_SCALE` | 100 | Camera speed, as a percentage of the engine's acceleration ramp (25–400) |
| `NOVA3_SCALE` | `fit` | Panel scaling: `fit` keeps 4:3 and letterboxes, `stretch` fills the panel, `integer` scales by a whole number |
| `NOVA3_GL_SINGLE_DISPATCH` | 1 | Resolve the interposed GL calls through SDL's own dispatch |

On a real 640×480 panel `NOVA3_SCALE` is identity — nothing is scaled.

## The first launch takes a while

The engine never reads its own OBB containers: on Android the store installer
unpacked them, and that installer is not part of this port. So the port opens
the containers itself and unpacks each asset the first time the engine asks
for it. The consequence is that the first boot, and the first minutes of a
level you have not played yet, are noticeably slower than later ones. The
containers stay where they are; only what the game actually touches is
unpacked beside them.

## Status

Playable on hardware: the campaign runs, the control map above is settled from
hardware sessions, and the eapx first-boot import is in place.

Known and honest about it:

- D-pad Left sends key 122, which the engine accepts and whose effect is still
  unidentified.
- The audio mixer logs OpenSL buffer underruns ("device draining at 100%")
  while sounding correct on the device. The watermark has not been retuned.
- Screenshots and cover art are placeholders until gameplay captures from
  hardware replace them.
