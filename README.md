# XboxHeadsetBridge

Making Xbox GIP wireless headsets work on macOS — starting with the
**PDP LVL50 Wireless for Xbox** (`0e6f:0234`).

macOS enumerates the LVL50 dongle but binds nothing to it, so no audio device
ever appears. The dongle is not a USB Audio Class device: it speaks Microsoft's
**GIP** (Gaming Input Protocol), the same protocol Xbox One accessories use.
Nothing in macOS knows that protocol.

Status: **playback works; microphone capture is still blocked.** Speaker
playback works through a real Core Audio device that any macOS app can select.
The bridge daemon auto-reconnects on USB disconnect and forwards headset
volume/mute state to the HAL ring. The standalone full-duplex microphone test
still receives zero bytes from the USB IN endpoint, so the HAL microphone is
not yet functional.

## Architecture

```
                 macOS CoreAudio / System Apps
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  Core Audio HAL Plug-In (plugin/XboxHeadset.c)               │
│  - Runs inside sandboxed coreaudiod                         │
│  - Exposes "Xbox Wireless Headset" virtual audio device     │
│  - Captures 48 kHz stereo Float32 audio from the OS mix     │
│  - Presents 48 kHz mono Float32 mic input to the OS         │
└─────────────────────────────┬───────────────────────────────┘
                              │
                              ▼ Lock-free SPSC Ring Buffer
                 (shared/ring.h — shm_open / mmap)
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  Bridge Daemon (src/bridge.c -> build/gip-bridge)            │
│  - Userspace process owning IOKit USB interfaces            │
│  - Drains ring buffer, converts Float32 -> S16 PCM          │
│  - Wraps audio in GIP 0x60 (AUDIO_SAMPLES) packets          │
│  - Streams 8-frame isochronous transfers to USB OUT ep 0x02 │
│  - Captures ISO IN ep 0x83, upsamples 24→48 kHz, fills ring │
│  - Auto-reconnects on USB disconnect                        │
└─────────────────────────────┬───────────────────────────────┘
                              │
                              ▼
               PDP LVL50 Wireless USB Dongle
```

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

## Tools

| Tool | Description |
|---|---|
| `build/gip-probe [secs]` | Claim the device, run GIP handshake, dump advertised audio formats and capabilities |
| `build/gip-tone [secs]` | Stream a 440 Hz sine wave to the headset (milestone 2 test) |
| `build/gip-mic [secs] [file.wav]` | Capture mic audio, display VU meter, optionally record to WAV |
| `build/gip-bridge` | Full bidirectional bridge daemon — auto-reconnects on disconnect |
| `build/gip-status` | One-shot or continuous (`-c`) display of battery, volume, mute, stream stats |

## Building and installing

```bash
make                                # build everything
sudo make install-plugin            # install HAL plug-in + restart coreaudiod
./build/gip-bridge                  # start the bridge (runs until ctrl-c)
```

Then select "Xbox Wireless Headset" as both output and input in any macOS
audio app.

To uninstall:
```bash
make uninstall-plugin               # remove HAL plug-in + restart coreaudiod
```

No kernel extension, no DriverKit, no entitlements, no SIP changes — a plain
userspace process claims the vendor-class interface through IOKit. Runs without
`sudo` (except for `make install-plugin` which needs root to write to
`/Library/Audio/Plug-Ins/HAL`).

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
- **`CFRunLoopRunInMode` must be called with `returnAfterSourceHandled: false`.**
  With `true` it returns after a single completion callback, which throttles the
  isochronous stream to roughly one transfer per loop iteration — audio starts
  and then starves. This looks exactly like a USB problem and is not one.
- **The audio interface must be set to alt 0 before alt 1.** xone calls this
  mandatory for third-party devices, and the LVL50 is one.

## Audio stream layout

### Output (48 kHz stereo S16)

```
buffer_size   = 48000 * 2ch * 2B * 8ms / 1000 = 1536 B per 8 ms
fragment_size = 1536 / 8 packets              =  192 B per USB frame
packet_size   = 6 B GIP header + 192 B        =  198 B  (endpoint max 224)
```

### Input (24 kHz mono S16, upsampled to 48 kHz mono float)

```
buffer_size   = 24000 * 1ch * 2B * 8ms / 1000 =  384 B per 8 ms
fragment_size = 384 / 8 packets               =   48 B per USB frame
packet_size   = 6 B GIP header + 48 B         =   54 B  (endpoint max 128)
```

Each 1 ms USB frame carries one `AUDIO_SAMPLES` (`0x60`) packet: a GIP header
with an incrementing sequence number, followed by raw S16 PCM. Transfers cover
8 frames each, with 4 in flight in each direction.

## Roadmap

- [x] **Milestone 1** — claim the device, run the GIP handshake, read the
      advertised audio formats (`src/probe.c`)
- [x] **Milestone 2** — negotiate the format via `AUDIO_CONTROL`, switch
      interface 1 to alt 1, stream isochronous audio out (`src/tone.c`).
      Confirmed audible in the headset.
- [ ] **Milestone 3** — capture the microphone from the iso IN endpoint
      (`src/mic.c`, 24 kHz mono with 2x linear interpolation to 48 kHz).
- [ ] **Milestone 4** — expose both as a real Core Audio device via an
      AudioServerPlugin in `/Library/Audio/Plug-Ins/HAL`, so every app can
      select the headset (`plugin/XboxHeadset.c` + `src/bridge.c`).
- [x] Volume/mute wheel handling (live `AUDIO_CONTROL` packets forwarded to
      HAL ring)
- [x] Auto-reconnect on USB disconnect (device pull, headset power-off)
- [ ] Battery reporting (needs `GIP_CMD_STATUS` parsing or HID report)
- [ ] Hot-plug detection for dynamic plug/unplug without bridge restart

## Credit and license

The GIP protocol constants, header codec, and chunking semantics are ported
from [**xone**](https://github.com/medusalix/xone) by Severin von
Wnuck-Lipinski, the Linux kernel driver for Xbox accessories. xone already
matches this dongle generically (vendor `0x0e6f`, interface `ff/47/d0`) and
implements headset audio over ALSA; this project is the macOS counterpart.

xone is GPL-2.0-or-later, so this project is too. See `LICENSE`.
