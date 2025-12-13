# Installing PREEMPT_RT Kernel on Raspberry Pi

This guide covers installing the Raspberry Pi Foundation's PREEMPT_RT kernel for real-time performance on Pi 4/5 running Raspberry Pi OS (Bookworm/Trixie).

## Why PREEMPT_RT?

The RT kernel provides deterministic timing for applications requiring:

- Consistent frame timing (stimulus presentation)
- Low-latency audio (FluidSynth, ALSA)
- Precise GPIO timing
- Microsecond-accurate data acquisition

## Installation

The Pi Foundation provides a packaged RT kernel, so no custom compilation is needed:

```bash
sudo apt update
sudo apt install linux-image-rpi-v8-rt
```

This installs the kernel and initramfs to `/boot/firmware/`:

- `kernel8_rt.img` — the RT kernel
- `initramfs8_rt` — matching initramfs

## Configuration

Edit the boot configuration:

```bash
sudo nano /boot/firmware/config.txt
```

Add under the `[all]` section:

```
kernel=kernel8_rt.img
```

Note: If `auto_initramfs=1` is already set in config.txt (default on recent installs), the matching initramfs is loaded automatically. Otherwise, also add:

```
initramfs initramfs8_rt followkernel
```

## Reboot and Verify

```bash
sudo reboot
```

After reboot, verify the RT kernel is running:

```bash
uname -a
```

You should see `PREEMPT_RT` in the output:

```
Linux pi4dev 6.12.47+rpt-rpi-v8-rt #1 SMP PREEMPT_RT ...
```

## Systemd Service Configuration

To take advantage of RT scheduling, configure your services with these directives:

```ini
[Service]
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=60
LimitMEMLOCK=infinity
```

Priority values: higher = more priority. Typical allocation:

- 80: Critical timing (hardware I/O)
- 60: Data acquisition (dserv)
- 50: Display/presentation (stim2)
- Default: Everything else

## Optional: CPU Isolation

For maximum determinism, isolate a CPU core from the scheduler.

Edit `/boot/firmware/cmdline.txt` and add:

```
isolcpus=3
```

Then pin your RT process to that core in the systemd service:

```ini
CPUAffinity=3
```

## Optional: Memory Locking

To prevent page fault latency, add `mlockall()` to your application startup:

```cpp
#include <sys/mman.h>

if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
    perror("mlockall failed");
}
```

Requires `LimitMEMLOCK=infinity` in the systemd service.

## Switching Back to Standard Kernel

To return to the non-RT kernel, comment out or remove the kernel line:

```bash
sudo nano /boot/firmware/config.txt
```

```
# kernel=kernel8_rt.img
```

Reboot to use the default `kernel8.img`.

## Keeping Updated

The RT kernel updates through normal apt:

```bash
sudo apt update
sudo apt upgrade
```

The kernel package (`linux-image-rpi-v8-rt`) tracks the same version as the standard Pi kernel, so you get security updates and driver improvements automatically.
