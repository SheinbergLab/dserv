# Superseded patches

Kept for history; DO NOT APPLY. Everything here is folded into
`../enet-qos-rx-fixes.patch`, which is the ONE patch to apply to
`~/zephyrproject/zephyr` after a `west update` (it erases the tree copy).

- `enet-qos-rx-timestamp-race.patch` (2026-07-29) -- the bounded wait on the
  RX context descriptor. Carried alone for a day; now hunk 4 of the fixes
  patch.
- `enet-qos-rx-timestamp-instrument.patch` (2026-07-30) -- the SWD-read
  `extio_rxts` counters plus an early three-site buf1_addr restore. The
  counters live on (extended to 8 words, so the pyocd read is `0x20` now);
  the restore grew into the full-descriptor rearm in the fixes patch.

These three files were mutually conflicting variants of the same hunks --
applying the wrong one after a west update was a standing hazard, which is
why only the consolidated patch remains in `patches/`.
