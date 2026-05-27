# HDMI Audio for the dserv Sound Module on Pi 5

The dserv sound module (`modules/sound/sound.c`) uses an in-process FluidSynth
to render MIDI via ALSA. On a fresh Raspberry Pi OS (Bookworm) install on a
Pi 5, three things have to be true before `sound::init_fluidsynth` will work:

1. The user that runs dserv can open `/dev/snd/*` (must be in the `audio` group).
2. No other process is holding the HDMI playback device exclusively.
3. The right `CARD=` name is passed (the Pi has two HDMI ports → two cards).

These notes walk through the setup end to end.

## 1. Confirm the cards exist

```bash
cat /proc/asound/cards
```

On a Pi 5 you should see both HDMI ports:

```
 0 [vc4hdmi0       ]: vc4-hdmi - vc4-hdmi-0
                      vc4-hdmi-0
 1 [vc4hdmi1       ]: vc4-hdmi - vc4-hdmi-1
                      vc4-hdmi-1
```

`vc4hdmi0` is the HDMI port nearest the USB-C power input; `vc4hdmi1` is the
one next to it. Pick the card matching the HDMI port your speakers/display
are plugged into.

HDMI audio only routes if the attached sink advertises PCM in its EDID. A
plain DVI adapter or a monitor with no speakers will let the card enumerate
but refuse to play.

## 2. Put users in the `audio` group

If `aplay -l` says "no soundcards found" but `/proc/asound/cards` lists them,
the user isn't in the `audio` group and can't open `/dev/snd/*`.

```bash
sudo usermod -aG audio lab
# Add any other user that runs dserv (e.g. root if dserv runs as a system service)
sudo usermod -aG audio root
```

Log out and back in (or restart the dserv service) so the new group takes
effect. Verify:

```bash
groups lab            # should include "audio"
aplay -l              # should now list both vc4hdmi cards
```

## 3. Disable the standalone fluidsynth service

The Debian `fluidsynth` package ships a systemd **user** unit that auto-starts
at login and grabs an HDMI card exclusively. Our in-process FluidSynth then
fails with:

```
fluidsynth: error: The "plughw:CARD=vc4hdmi0,DEV=0" audio device is used by another application
```

Disable it for whichever user logs in to run experiments:

```bash
systemctl --user stop fluidsynth.service
systemctl --user disable fluidsynth.service
systemctl --user mask fluidsynth.service
```

Verify nothing holds the device:

```bash
sudo fuser -v /dev/snd/pcmC0D0p     # should print nothing
sudo fuser -v /dev/snd/pcmC1D0p     # same for the other HDMI card
```

Do **not** `apt purge libfluidsynth3` — dserv links against it. Removing the
`fluidsynth` binary package is fine; only the standalone daemon is the
problem.

If a sound server (PipeWire / PulseAudio) is also grabbing the card on a
desktop install, mask those at the user level too:

```bash
systemctl --user mask pipewire pipewire-pulse wireplumber
systemctl --user stop pipewire pipewire-pulse wireplumber
```

For a dedicated experiment rig this is what you want — exclusive access, no
resampling through a sound server, lower latency.

## 4. Test the device from the shell

Before involving dserv:

```bash
speaker-test -D plughw:CARD=vc4hdmi0,DEV=0 -c 2 -t sine -f 440 -l 1
```

If that beeps, the device is yours. If it says "Device or resource busy,"
re-run the `fuser` check from step 3 — something is still holding it.

## 5. Initialize from dserv

```tcl
sound::init_fluidsynth /usr/share/sounds/sf2/default-GM.sf2 plughw:CARD=vc4hdmi0,DEV=0
```

Use `vc4hdmi1` instead for the other HDMI port. With no device argument the
module auto-probes (`default`, `plughw:0,0`, `sysdefault`, `hw:0,0`) — handy
on dev boxes, but on a rig you should name the card explicitly so it can't
drift if device ordering changes.

To enumerate devices from inside dserv:

```tcl
sound::list_alsa_devices
```

## Troubleshooting quick reference

| Symptom | Likely cause | Fix |
|---|---|---|
| `aplay -l` says "no soundcards found", `/proc/asound/cards` lists them | User not in `audio` group | `sudo usermod -aG audio <user>`, re-login |
| `Cannot get card index for Device` | Wrong `CARD=` name | Use `vc4hdmi0` / `vc4hdmi1`, not the generic `Device` |
| `audio device is used by another application` | Standalone `fluidsynth.service` (or PipeWire) is running | Mask the user services (step 3) |
| Device opens, no sound | HDMI sink doesn't advertise PCM | Try a different display / AVR; check the other HDMI port |
