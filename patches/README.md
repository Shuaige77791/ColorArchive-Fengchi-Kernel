# Change notes — Fengchi port

See the top-level `README.md` for the full picture.  This directory keeps the
machine-readable bits.

- `FILELIST.txt` — the exact 29 files in `overlay/`
- `fengchi.config` — config fragment
- `BUILD_NOTES.txt` — build recipe and the DEBUG_INFO trap

## The trap, in one paragraph

`CONFIG_DEBUG_INFO=y` makes `.eh_frame` grow; ~267 KB of it ends up **past
`_end`**, so `image_size` (= `_end - _text`) no longer covers the Image.  The
bootloader then fails the image and the phone resets before any kernel output.
Symptom: *logo flashes once, then reboot*, with nothing in `pstore` and nothing
in the OPLUS log partitions.  Fix: `# CONFIG_DEBUG_INFO is not set` (that is how
the working `#50` reference build was configured).
