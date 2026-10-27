# PR #2 fallback verification logs (head `50e0dc8`)

These logs verify the fallback added in
[`50e0dc8`](https://github.com/cwt/libfprint-CS9711/commit/50e0dc8a91b1411a90faa52c0efd6ee6082397cb)
("take the steadiest frame when a press does not settle") on the same
`2541:0236` unit that failed the fixed settle threshold.

## Test environment

- Fedora Linux 44, kernel `7.2.11-200.fc44.x86_64`
- Reader: `Bus 003 Device 010: ID 2541:0236 Chipsailing CS9711Fingprint`
  - `bcdUSB 2.00`, `bcdDevice 2.00`
  - `iManufacturer Chipsailing`, `iProduct CS9711Fingprint`, no serial
- libfprint version reported at startup: `1.94.6`
- PR head: `50e0dc8a91b1411a90faa52c0efd6ee6082397cb`
- PR base: `af46d76d5948709a32cbbfdd74c49e54941e8666`
- No build-time overrides: `CS9711_SETTLE_MAD` is `4.0` and
  `CS9711_SETTLE_MAX_FRAMES` is `5`.

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

## Test 1: enrollment

Command:

```
sudo env G_MESSAGES_DEBUG=all builddir/examples/enroll
```

The right index finger was enrolled with 15 normal press-and-release
placements, held for about 1.5 to 2 seconds each.

Result: **15 of 15 stages passed**. Every press used the fallback; the
fixed threshold never fired. The selected frame differed from the
previous one by **9.8 to 11.3** grey levels (mean 10.8).

Log: `enroll-15-presses.log`.

## Test 2: verification with the enrolled finger

Command:

```
sudo env G_MESSAGES_DEBUG=all builddir/examples/verify
```

The right index finger used for enrollment was presented six times.

Result: **6 MATCH, 0 NO MATCH**. Every attempt used the fallback. The
selected frame differed by **10.1 to 11.8** grey levels. The time from
the first captured frame to the match result was **1.429 to 1.434 s**.

Log: `verify-same-finger.log`.

## Test 3: verification with a different finger

The same right-index print was loaded, and the **left index** finger was
presented four times.

Result: **0 MATCH, 4 NO MATCH**. Every attempt used the fallback. The
selected frame differed by **10.9 to 13.5** grey levels. The time from
the first captured frame to the result was **1.432 to 1.435 s**.

Log: `verify-different-finger.log`.

## Summary

| Test | Result | Fallback used | Best frame MAD | Time to result |
|---|---|---:|---:|---:|
| Enrollment | 15/15 stages | 15/15 | 9.8 - 11.3 | - |
| Verify, same finger | 6/6 MATCH | 6/6 | 10.1 - 11.8 | 1.429 - 1.434 s |
| Verify, different finger | 0/4 MATCH | 4/4 | 10.9 - 13.5 | 1.432 - 1.435 s |

The fallback makes enroll and verify work on this unit, and it still
rejects a different finger. The fixed threshold remains the fast path
but is not reached on this hardware.

Not tested: a very short tap that lifts within two frames, and a
deliberately rolling finger.

## Files

- `enroll-15-presses.log` - complete enrollment run, 15/15 stages.
- `verify-same-finger.log` - six attempts with the enrolled finger.
- `verify-different-finger.log` - four attempts with a different finger.
- `results.csv` - per-attempt result, MAD, timing, and lift frame.

This branch is for review evidence only and is not intended for merging.
