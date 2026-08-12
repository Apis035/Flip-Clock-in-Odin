# flip-clock

A raylib flip clock: IST in full size with seconds and the date, Pacific time
smaller underneath.

## Build

    make

raylib is resolved in this order: `pkg-config`, then `./vendor`, then
`/usr/include/raylib.h`. If none of those exist:

    sudo pacman -S raylib      # or, without root:
    make vendor                # drops the prebuilt 5.5 release into ./vendor

`make vendor` links with an rpath, so the binary runs standalone. Switching to a
system raylib later needs `make distclean && make`, otherwise the vendored copy
keeps winning.

## Run

    ./flipclock

`F11` or `F` toggles fullscreen. Escape does not quit; set `SetExitKey` back to
`KEY_ESCAPE` in `main.c` if you want it to.

Flip duration is a compile-time knob:

    make CFLAGS="-O2 -DFLIP_SECONDS=0.6f"

The default 0.9s nearly fills the tick, which keeps the card moving instead of
snapping and then sitting still for the rest of the second. Values above ~1.0
never finish before the next change.

## Implementation notes

Times come from `localtime_r` with `$TZ` swapped per clock, so DST is the
system's problem and the zone label is whatever is currently true — the small
clock reads PDT in summer and PST in winter.

Each digit is pre-rendered into a card face at 2x and filtered down, so drawing
is pure texture work and the glyphs are supersampled. Faces and font atlases are
rebaked on resize, debounced so a drag doesn't thrash them. Every string is
drawn at exactly the size its atlas was baked at; nothing is resampled.

The flip is a textured quad with the seam edge pinned and the free edge lifting
toward the viewer, so it foreshortens by `cos` and widens slightly for
perspective. The angle sweeps at a constant rate — easing it stalls the leaf at
both ends, and the visible height already carries the fast-through-the-middle
motion.

Animation phase is derived from `clock_gettime` each frame rather than
accumulated from frame deltas, and is anchored to the instant the second
actually turned over rather than the frame that noticed. Successive flips stay
exactly one second apart, and a hitched frame skips ahead instead of stretching
the animation.

## Fonts

Resolved at runtime from a short list — DejaVu, Liberation, Noto, Inter — with
digits bold and the date and zone lines regular. To pin something specific, put
it at the top of both lists in `FontPath`.
