# NXP i.MX Debian images (flexbuild)

Builds Debian for the FRDM-IMX93 and FRDM-IMX95-15x15 boards using NXP's
[flexbuild](https://github.com/NXP/flexbuild), the build system behind their
**Debian Linux SDK** (doc UG10155).

This is Debian *userland* on NXP's vendor kernel — not stock Debian. The tag
stream `LSDK-26.06_DEBIAN-13_LF-6.12.49` gives **Debian 13 trixie** (the same
release the Pi fleet runs) with kernel `nxp-imx/linux-imx` at `lf-6.12.49-2.2.0`.
Because the kernel comes from NXP, hold kernel packages — `apt` must not upgrade
them.

## Layout

| file | runs on | purpose |
|---|---|---|
| `fb-build.sh` | build host | drives a full flexbuild build, detached |
| `fb-run-all.sh` | build host | builds stock i.MX93 + i.MX95, then the RT i.MX93 |
| `fb-mkwic.sh` | build host | assembles all-in-one `.wic.zst` SD images |
| `rt.config` | build host | kernel fragment enabling `PREEMPT_RT` |
| `patches/linux/` | build host | kernel patches, copied into a tree's `patch/linux/` |
| `../imx-sdcard.sh` | macOS | fetch images and flash an SD card |

The build host is currently `pogo`, with trees at `/data/flexbuild` (stock) and
`/data/flexbuild-rt` (RT). These scripts live at `/data/` there.

## Kernel patches

`patches/linux/*.patch` are ours, on top of whatever the SDK tag ships. Copy them
into **each** tree's `patch/linux/` — the two trees are independent checkouts, and
a patch in one is not in the other.

Three things about the mechanism, each of which will waste your afternoon:

- flexbuild applies patches **once** and then touches a `.patchdone` sentinel in
  the source directory. Adding a new patch to an already-built tree is therefore
  **silently ignored** — apply it by hand as well as dropping the file in.
- the kernel source is **not a git checkout** (`dl_github.py` extracts a tarball;
  there is no `.git`), so patches are applied with plain `patch -p1`, not
  `git am`. Generate them with `diff -u`. Running `git` inside that directory
  resolves to the *outer* flexbuild repo and fails with "ignored by .gitignore:
  components_lsdk2606".
- `make dl-kernel` does **not** clobber hand edits to an existing tree, so it is
  safe to edit in place and rebuild.

A kernel-only change does **not** need `make all` or a reflash. `make linux`
alone is incremental (~4 min for one file) and produces
`build_lsdk2606/linux/linux/arm64/IMX/Image`; copy that to the board's
`/boot/Image` and reboot, which preserves the installed system that a reflash
would wipe. Keep the old image (`/boot/Image.pre-*`) and checksum the new one on
the board *before* moving it into place.

## Workflow

```bash
# on the build host
/data/fb-run-all.sh          # build all three targets   (hours)
/data/fb-mkwic.sh            # assemble SD images        (~10 min each)

# on macOS
scripts/imx-sdcard.sh fetch
scripts/imx-sdcard.sh flash sdcard_imx93frdm.wic.zst
```

Images land in `/data/imx-sdcards/sdcard_<machine>[-rt].wic.zst` (+ `.sha256`),
~1.4–2.1 GB compressed. **They expand to 10 GB, so use a 16 GB or larger card.**

## Why the split

`flex-installer` partitions the card and creates an **ext4** filesystem, neither
of which macOS can do. So `mkwic` does all of that on the Linux build host and
emits one compressed whole-disk image; macOS only performs a raw block write.
This also suits the build host being off-site — one file per board crosses the
VPN instead of a partitioning session.

## Gotchas worth keeping

Ordered by how much time they cost.

- **`CONFIG_APP_ML=y` is the default and TensorFlow Lite fails to
  cross-compile.** It aborts `make all` at the apps stage, which runs *before*
  `flash.bin`/`boot`/`merge-apps`/`packrfs` — so the build produces no bootable
  image at all. `fb-build.sh` takes `DISABLE_ML=1`. None of the ML stack (TFLite,
  Ethos-U, NNStreamer) is needed for a dserv host.

- **flexbuild breaks on hosts with more than 24 cores.** `kconfig_hooks.py` sets
  `CONFIG_JOBS` to `cpu_count()`, but `Kconfig` declares `range 0 24`, so
  kconfiglib rejects the value, falls back to the default `0`, and
  `MAKEFLAGS += -j0` makes every sub-make die with *"the '-j' option requires a
  positive integer argument"*. `fb-build.sh` pins `CONFIG_JOBS=24`; it survives
  because the hook only rewrites `JOBS` when it is `0`.

- **An RT build needs its own tree.** `src/linux/linux.mk` generates the kernel
  `.config` only `if [ ! -f $(KOUTDIR)/.config ]`, so rebuilding in the stock
  tree would silently reuse the non-RT config — and `KOUTDIR` doesn't vary with
  the config, so it would also overwrite the baseline. That step is redirected to
  `/dev/null`, so a failed fragment merge is invisible: `fb-build.sh` therefore
  greps the *generated* kernel `.config` for `CONFIG_PREEMPT_RT=y` and aborts
  before the long stages if it isn't there.

- **PREEMPT_RT needs no patch series.** It is mainline as of 6.12 and NXP's
  `lf-6.12.y` carries it (`depends on EXPERT && ARCH_SUPPORTS_RT`, which arm64
  sets). `linux-imx` has no `-rt` branch — don't go looking. The fragment must
  end in `.config` for the kernel's fragment rule to fire.

- **The build host needs an arm64 `binfmt_misc` handler.** The rootfs stage
  debootstraps arm64 and chroots in. `fb-build.sh` registers Debian's
  `qemu-aarch64-static` from inside the privileged container using the `F`
  (fix-binary) flag, so the kernel holds the interpreter open across the chroot
  and after the container exits. It is kernel-global and **lost on reboot**, so
  it is re-checked every run. Debian trixie ships no `update-binfmts` entry, and
  its `binfmt.d` conf points at a `/usr/libexec/qemu-binfmt` wrapper that does
  *not* survive the chroot — hence registering by hand.

- **`make docker` is interactive-only.** For detached builds, run the build as
  the container command with the same contract (privileged, `net=host`, with
  `$FBDIR`, `/dev` and `/lib/modules` mounted).

- **`mkwic` writes to the tree root**, not `images/`, and names the file
  `rootfs_<distro>_<machine>.wic.zst` — a misleading name for a whole-disk image,
  and identical between the stock and RT trees. `fb-mkwic.sh` renames on
  collection.

## Hardware notes

- **i.MX93 has no 3D GPU** — the PXP block is 2D only (rotate/resize/CSC/blit).
  No hardware EGL/GLES, so it is a headless controller only; stim2 will not run
  usefully on it.
- **i.MX95 has a Mali-G310** (GLES 3.2 / Vulkan 1.2). flexbuild pulls NXP's
  proprietary `mali-imx-*` blob, which is GLES/Vulkan only — stim2 targets
  desktop OpenGL (GLFW + GLEW), so it is not a drop-in. Mesa/Panfrost is the
  plausible route but is unproven here.
- Both boards have **two Ethernet controllers**, which is the main advantage over
  a single-NIC Pi 5 for an Ethernet/PTP box network.
- The old Yocto tree used `linux-imx` **6.18.20**; the Debian SDK pins
  **6.12.49**. Same kernel project, older release — and you cannot simply swap in
  6.18, because the SDK's binary blobs are version-matched to 6.12.49.
