# Tcl runtime ownership

**Status:** proposal, not started. Written 2026-08-12.
**Scope:** `dserv`, `stim2`, `dlsh` Debian packaging on Linux. macOS is unaffected
throughout (Homebrew on dserv/stim2, self-contained bundle on dlsh).

## The question

Three packages, in three install combinations — `dserv+stim2+dlsh`,
`stim2+dlsh`, `dserv+dlsh` — need one shared Tcl 9 runtime. Which package should
own `/usr/local/lib/libtcl9.0.so`?

Today it is `dserv`, which is wrong for a simple reason: **dserv is optional and
dlsh is not.** Neither dserv nor stim2 is functional without dlsh, so dlsh is
already a hard prerequisite of every configuration. The runtime belongs with the
component that is always present.

## Verified current state

Measured on officepi (trixie, aarch64) and office-stim (x86_64), 2026-08-12.

| Package | Ships a shared libtcl? | Declares a Tcl dep? |
|---|---|---|
| dserv | **yes** — `/usr/local/lib/libtcl9.0.so`, the *only* file it puts in `/usr/local/lib` | no |
| stim2 | no | `Depends: libtcl9.0` on trixie (per-distro list) |
| dlsh | no — builds Tcl **statically** for its own interpreter (`--disable-shared`, `libtcl9.0.a`) | no |

Facts worth carrying into the implementation:

- **One file is enough.** Tcl 9 appends its script library to the shared object
  as a zipfs archive. Confirmed: a ZIP end-of-central-directory record sits at
  the tail of our `libtcl9.0.so`, and `info library` reports
  `//zipfs:/lib/tcl/tcl_library`. There is no `tcl9.0/` script directory to ship.
  Debian's `libtcl9.0` does **not** do this — it keeps scripts in
  `/usr/share/tcltk` — so our copy and the distro's are not interchangeable as
  files, only as ABI.
- **Debian's Tcl is `+dfsg`-unbundled.** It links system libtommath, so
  `libtommath.so.1` appearing in an `ldd` is a reliable tell that the apt copy is
  loaded. A symbol diff between the two shows ~54 `TclBN_mp_*`; that is the
  unbundling, not a 9.0.1→9.0.4 API change.
- **`libtclstub.a`, `tclConfig.sh`, `tclooConfig.sh` in `/usr/local/lib` are
  unowned** by any package on officepi — they are residue from a local
  `make install`. The deb ships only the `.so`. Any new package should decide
  deliberately whether it also ships build-time artifacts (see open questions).

## What already landed, and stays regardless

Both binaries now carry `DT_RUNPATH = /usr/local/lib`
(dserv `76b7bb58`, stim2 `62b0111`). This is **orthogonal to ownership** and
survives this proposal unchanged.

It exists because load order was previously decided by architecture: the ld
cache is built by globbing `/etc/ld.so.conf.d/*.conf` alphabetically,
`/usr/local/lib` comes from `libc.conf`, and the system dir from the arch-named
file. `a` < `l` < `x`, so `aarch64-linux-gnu.conf` won on the Pis (apt's Tcl)
while `libc.conf` won on x86_64 (`/usr/local/lib`). The same package silently ran
a different Tcl depending on the box. RUNPATH is searched before the cache, so
the order is now identical everywhere.

RUNPATH fixed the *order*. This proposal fixes the *answer*.

## The residual problem

With ownership in dserv, a **stimulus-only machine** gets a current Tcl only by
installing dserv — a data server it will never run — purely to obtain a library.
That is the wart that motivates moving ownership.

## Proposal

Introduce a `dlsh-tcl` binary package, built and released by dlsh, shipping
`/usr/local/lib/libtcl9.0.so` from the `core-9-0-N` tag dlsh already pins via
`TCL_TAG`.

| Package | Change |
|---|---|
| **dlsh** | add a *shared* Tcl build alongside the existing static one; emit `dlsh-tcl` |
| **dserv** | stop shipping the `.so`; add `Depends: dlsh-tcl (>= N)`; keep RUNPATH |
| **stim2** | flip `Depends: libtcl9.0` → `dlsh-tcl`; keep RUNPATH; retire the per-distro list |

Consequences:

- One Tcl per box, owned by the one package that is always installed.
- A stimulus-only machine needs no dserv.
- The dserv↔dlsh version agreement becomes **structural**. Today "dserv and dlsh
  must be on the same Tcl" is a convention maintained by editing two repos in
  lockstep; if dlsh supplies the runtime, they cannot disagree.
- stim2's per-distro `libtcl9.0` availability list disappears, along with its
  standing request to "extend the list as upstream distros add the package."
  Bookworm and Jammy stop needing an admin to provide Tcl 9 out of band.

## Migration

Each step is independently releasable and testable. Do not compress them.

1. **dlsh: build and package the shared Tcl.** Release. Verify the shipped `.so`
   carries the zipfs archive (EOCD at tail, `info library` →
   `//zipfs:/lib/tcl/tcl_library`) — a build host where the wrong Tcl is picked
   up would otherwise ship a library with no script library at all. Worth a
   configure-time assertion rather than a manual check.
2. **Install-test on a scratch box** in dependency order: `dlsh-tcl` alone, then
   stim2, then dserv. Confirm each resolves to `/usr/local/lib`.
3. **dserv: drop the shipped `.so`, add the dependency.** Blocked on step 1 —
   `dlsh-tcl` must exist in a release before dserv can depend on it.
4. **stim2: flip the Depends.**
5. **Retire** stim2's `_STIM2_DISTROS_WITH_TCL9` block.

### The file-ownership transfer is the fiddly part

`/usr/local/lib/libtcl9.0.so` is currently owned by `dserv` (`dpkg -S` confirms).
Handing it to another package is a standard but error-prone dpkg operation:
`dlsh-tcl` needs `Replaces: dserv (<< <first version that drops it>)` — and
likely `Breaks:` as well — or upgrading a box that already has both fails at
unpack with "trying to overwrite ... which is also in package dserv".

**This must be tested as an upgrade, not just a fresh install**, on a box that
already carries the old dserv. It is the single most likely way this lands badly
on a rig.

## Version policy

- `dlsh-tcl` tracks the Tcl version dlsh pins (`TCL_TAG`/`TCL_VERSION`).
- Bump order becomes: **dlsh first**, then dserv and stim2. This is new
  sequencing — dserv is self-sufficient today — and is the main ongoing cost.
- Consumers built against an older 9.0.x keep working: Tcl 9.0.x is ABI-stable,
  dlsh's own libs use stubs, and older-build-on-newer-runtime is the safe
  direction. The reverse (what we have now: built 9.0.4, ran 9.0.1) is the one
  that bites, and this proposal removes it.

## Risks and open questions

- **Bootstrap ordering.** A fresh install must pull `dlsh-tcl` before dserv is
  configurable. Fine for apt; worth checking against however the rigs are
  provisioned and updated.
- **dlsh builds Tcl twice** — static for its self-contained interpreter, shared
  for the package — unless the interpreter is switched to the shared library.
  Building twice is simpler and keeps the interpreter self-contained; decide
  explicitly rather than by accident.
- **Does `dlsh-tcl` ship build-time artifacts** (`libtclstub.a`, `tclConfig.sh`)?
  dserv and stim2 need Tcl headers and stubs to *build*, but CI builds those from
  the `deps/tcl` submodule and will continue to. Suggest runtime-only, and a
  separate `-dev` package only if a real need appears.
- **The submodule pins do not go away.** Build-time Tcl still comes from
  `deps/tcl` in dserv and stim2; only the runtime moves. The pins must stay in
  step with `dlsh-tcl` — see `project-tcl-version-pinning-three-repos`.
- **Removal semantics.** `apt remove dlsh-tcl` would now break dserv and stim2.
  Correct behavior, but it is a new way to break a rig.

## Rollback

Cheap at every step. dserv re-adds the library to its package (the
`InstallRequiredSystemLibraries` rule) and drops the dependency; stim2 restores
`Depends: libtcl9.0`. The RUNPATH changes stay either way, so no box loses the
determinism fix during a rollback.

## Alternative considered

**stim2 ships its own copy under `/usr/local/stim2/lib` with
`RUNPATH=$ORIGIN/lib`.** Fully self-contained, no dpkg conflict, no cross-package
coupling — but it puts a second Tcl on every rig and leaves dserv owning a
runtime that other packages depend on by accident. Rejected in favour of a single
owner, but it remains the fallback if the ownership transfer proves troublesome.
