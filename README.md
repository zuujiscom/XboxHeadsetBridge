# XboxHeadsetBridge

Making Xbox GIP wireless headsets work on macOS — starting with the
**PDP LVL50 Wireless for Xbox** (`0e6f:0234`).

macOS enumerates the LVL50 dongle but binds nothing to it, so no audio device
ever appears. The dongle is not a USB Audio Class device: it speaks Microsoft's
**GIP** (Gaming Input Protocol), the same protocol Xbox One accessories use.
Nothing in macOS knows that protocol.

Status: **the dongle talks to us.** Full announce/identify handshake works from
userspace, and the device has told us its audio format. Audio streaming is not
implemented yet.

## What the hardware actually is

`system_profiler` shows the device with class `0xFF` / subclass `0x47` /
protocol `0xD0` — the GIP accessory signature. Its config descriptor:

| Interface | Alt | Endpoint | Type | Max packet | Interval |
|---|---|---|---|---|---|
| 0 | 0 | `0x81` IN  | Interrupt | 64 B | 4 |
| 0 | 0 | `0x01` OUT | Interrupt | 64 B | 4 |
| 1 | 0 | — | — | (bandwidth-free idle) | — |
| 1 | **1** | `0x02` OUT | **Isochronous** | 224 B | 1 |
| 1 | **1** | `0x83` IN  | **Isochronous** | 128 B | 1 |

Interface 0 carries GIP control traffic; interface 1 alt 1 carries audio.
Full-speed device, so one packet per 1 ms frame.

The important part: **the dongle impersonates a wired GIP headset.** It handles
the 2.4 GHz link to the earcups internally, so there is no radio protocol to
reverse — only GIP over USB.

## What the device told us

Verified against real hardware by `gip-probe`:

```
device 0e6f:0234  fw 1.0.1.4  hw 1.1.1.1
device classes: count=1
  [0] Windows.Xbox.Input.Headset
audio formats: count=1
  [0] 09 10   in=24 kHz mono  out=48 kHz stereo
capabilities out: 01 02 03 04 06 08 60
capabilities in:  01 04 05 06 08 60
```

`0x08` is `AUDIO_CONTROL` and `0x60` is `AUDIO_SAMPLES`, in both directions.
The advertised format matches the endpoint sizes: 48 kHz stereo S16 is
192 B/ms against a 224 B OUT endpoint, leaving room for the GIP header.

The dongle also emits live `AUDIO_CONTROL` packets carrying volume and mute
state as you turn the wheel on the headset.

## Protocol notes learned the hard way

Things that cost time and are not obvious from the Linux source:

- **Interrupt pipes do not support `ReadPipeTO` / `WritePipeTO`.** Those return
  `kIOReturnBadArgument` (`0xe00002c2`). Use `ReadPipeAsync` with a
  `CFRunLoop`, which is what the isochronous path will need anyway.
- **The dongle only announces on fresh enumeration.** Once configured it stays
  quiet, so the probe calls `USBDeviceReEnumerate` first. It will also answer a
  bare `IDENTIFY` at any time.
- **On a chunk-start packet, `chunk_offset` is the *total* length**, and it must
  be zeroed before building the acknowledgement. Acking with the total still in
  place makes the device abandon the transfer.
- **A chunked transfer completes on an empty chunk**, not when the byte count is
  reached.
- **Identify info-element offsets are relative to the offset table**, i.e. 16
  bytes into the payload — not to the payload start. Everything decodes as
  garbage if you assume otherwise.

## Building

```bash
make
./build/gip-probe 10      # listen for 10 seconds
```

No kernel extension, no DriverKit, no entitlements, no SIP changes — a plain
userspace process claims the vendor-class interface through IOKit. Runs without
`sudo`.

## Roadmap

- [x] **Milestone 1** — claim the device, run the GIP handshake, read the
      advertised audio formats (`src/probe.c`)
- [ ] **Milestone 2** — negotiate the format via `AUDIO_CONTROL`, switch
      interface 1 to alt 1, and stream isochronous audio out; prove sound
      reaches the headset
- [ ] **Milestone 3** — capture the microphone from the iso IN endpoint
- [ ] **Milestone 4** — expose both as a real Core Audio device via an
      AudioServerPlugin in `/Library/Audio/Plug-Ins/HAL`, so every app can
      select the headset
- [ ] Volume/mute wheel handling, battery reporting, hot-plug

For a milestone-2 prototype the HAL plugin can be skipped entirely — point the
bridge at BlackHole or a loopback device first.

## Credit and license

The GIP protocol constants, header codec, and chunking semantics are ported
from [**xone**](https://github.com/medusalix/xone) by Severin von
Wnuck-Lipinski, the Linux kernel driver for Xbox accessories. xone already
matches this dongle generically (vendor `0x0e6f`, interface `ff/47/d0`) and
implements headset audio over ALSA; this project is the macOS counterpart.

xone is GPL-2.0-or-later, so this project is too. See `LICENSE`.
