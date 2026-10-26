# PR #2 test logs

These logs were collected to verify the behaviour described in
[PR #2](https://github.com/cwt/libfprint-CS9711/pull/2) on real hardware.

## Test environment

- Fedora Linux 44, kernel `7.2.11-200.fc44.x86_64`
- Reader: `Bus 003 Device 010: ID 2541:0236 Chipsailing CS9711Fingprint`
  - `bcdUSB 2.00`, `bcdDevice 2.00`
  - `iManufacturer Chipsailing`, `iProduct CS9711Fingprint`, no serial
  - `MaxPower 100mA`
- libfprint version reported at startup: `1.94.6`
- PR head: `5b7b87ef9ac5f915e62880eeaa0eaa16071d61e4`
- PR base: `af46d76d5948709a32cbbfdd74c49e54941e8666`

## Build

```
meson setup builddir
ninja -C builddir
meson test -C builddir --print-errorlogs
```

The build completed and the Meson test suite passed:

```
Ok:      23
Fail:    0
Skipped: 2
```

The examples were run through `sudo` because `/dev/bus/usb/003/010` is
`root:root` and there is no udev rule installed for this device.

## Test 1: enrollment with the PR as-is

Command:

```
sudo env G_MESSAGES_DEBUG=all builddir/examples/enroll
```

The right index finger was enrolled. Each placement was a normal
press-and-release; the first placement was deliberately held for about
three seconds to check that a single long press does not consume several
stages.

Result: **15 attempts, 0 stages passed**. Every attempt ended with
`Please try again after removing the finger first.`

```
Enroll stage 0 of 15 failed ... (15 times)
stage passed: 0
```

Frame-to-frame mean absolute difference (MAD) across the 15 presses:

| Comparison | Range | Mean |
|---|---:|---:|
| frame 2 vs 1 | 11.1 - 32.0 | 17.1 |
| frame 3 vs 2 | 5.1 - 9.5 | 6.7 |
| frame 4 vs 3 | 5.1 - 8.1 | 5.9 |
| frame 5 vs 4 | 4.7 - 7.5 | 5.9 |

None of the 60 comparisons was below `CS9711_SETTLE_MAD` (4.0); the
minimum was 4.7. The per-press values are in `mads-per-press.csv`.

The log also shows the activation flush and the press loop:
`enroll-15-presses.log`.

## Test 2: diagnostic build with 40 allowed frames

Only `CS9711_SETTLE_MAX_FRAMES` was changed, from 5 to 40, to find out
whether the frames eventually settle if given more time. The change was
reverted afterwards.

Command:

```
sudo env G_MESSAGES_DEBUG=all builddir-diag/examples/img-capture still.pgm
```

Result: 40 frames over about 14 seconds. MAD stayed between **8.8 and
12.8** and never approached 4.0. The capture then failed with the same
retry-remove-finger error.

Log: `img-capture-40-frames.log`.

## Test 3: cancellation while waiting for a finger

The PR's wait for a touch has no timeout, so a capture was started with
no finger and cancelled with SIGINT after about 3.5 seconds.

The SCAN was sent at `10:44:58.110`, cancellation was handled at
`10:45:01.603`, and deactivation completed at `10:45:01.604` - about
1 ms after cancellation.

Log: `activation-cancel-no-finger.log`.

## Files

- `enroll-15-presses.log` - complete enrollment run with the PR as-is.
- `mads-per-press.csv` - the four MAD values for each of the 15 presses.
- `img-capture-40-frames.log` - diagnostic run with a 40-frame limit.
- `activation-cancel-no-finger.log` - cancellation while waiting for a touch.
