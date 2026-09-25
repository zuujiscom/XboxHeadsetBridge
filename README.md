# XboxHeadsetBridge

Making Xbox GIP wireless headsets work on macOS — starting with the
**PDP LVL50 Wireless for Xbox** (`0e6f:0234`).

macOS enumerates the LVL50 dongle but binds nothing to it, so no audio device
ever appears. The dongle is not a USB Audio Class device: it speaks Microsoft's
**GIP** (Gaming Input Protocol), the same protocol Xbox One accessories use.
Nothing in macOS knows that protocol.

Status: **bidirectional audio is working.** Speaker playback works through a
real Core Audio device that any macOS app can select, and the bridge now receives
24 kHz mono microphone packets from the USB IN endpoint and forwards them to
the HAL ring. The bridge daemon auto-reconnects on USB disconnect and forwards
headset volume/mute state to the HAL ring.

The protocol code is ported from [xone](https://github.com/medusalix/xone), the
Linux driver for Xbox accessories. See [Credit and license](#credit-and-license).

## Getting started

### What you need

- A Mac running **macOS 14 (Sonoma) or later**.
- The **PDP LVL50 Wireless for Xbox** headset and its USB dongle (`0e6f:0234`).
  Other Xbox wireless headsets that use the same protocol may work, but only
  the LVL50 has been tested.
- An Apple silicon or Intel Mac. Development and testing were done on Apple
  silicon.
- **Xcode Command Line Tools** (for `clang`, `swiftc` and `make`):
  ```bash
  xcode-select --install
  ```
- **[Homebrew](https://brew.sh)** and OpenSSL 3, which the headset's
  authentication handshake needs:
  ```bash
  brew install openssl@3
  ```

No kernel extension, no DriverKit, no entitlements and no SIP changes are
needed. Everything runs as a normal app; `sudo` is only needed once, to install
the audio plug-in.

### Install

```bash
git clone https://github.com/zuujiscom/XboxHeadsetBridge.git
cd XboxHeadsetBridge
make                      # build everything
sudo make install-plugin  # install the audio device (restarts Core Audio)
make install-menubar      # copy the menu bar app to /Applications
open /Applications/XboxHeadsetMenu.app
```

`sudo make install-plugin` copies `XboxHeadset.driver` into
`/Library/Audio/Plug-Ins/HAL` and restarts `coreaudiod`, so any audio that is
playing will cut out for a second.

### Use it

1. Plug the dongle into the Mac and turn the headset on.
2. Click the headphones icon in the menu bar. It should say **Connected**. If
   it says "Bridge stopped", choose **Start Bridge**.
3. Open **System Settings → Sound** and pick **Xbox Wireless Headset** as both
   the output and the input. Any app can also select it directly (Discord, Zoom,
   OBS and so on).

The volume keys, the menu bar volume slider and the headset's own wheel all
work. The menu also has two toggles:

- **Start bridge when app opens.** Starts audio as soon as the app launches.
- **Open at Login.** Launches the app when you log in. This only works when the
  app is in `/Applications`, which is why `make install-menubar` exists.

Unplugging the dongle or turning the headset off is fine. The bridge
reconnects on its own when the headset comes back.

### Update

```bash
git pull
make
sudo make install-plugin   # only needed if plugin/ or shared/ changed
```

Quit the menu bar app, then run `make install-menubar` and reopen it. Quit
before you rebuild: rewriting the app while it is running makes macOS kill it
without a crash report, and it looks as if the app vanished.

### Uninstall

```bash
make uninstall-plugin                   # remove the audio device, restart Core Audio
rm -rf /Applications/XboxHeadsetMenu.app
```

Also turn off **Open at Login** first, or remove the app under System Settings →
General → Login Items.

### Troubleshooting

- **"Xbox Wireless Headset" is not in the Sound settings.** The plug-in is not
  installed or Core Audio has not reloaded it. Run `sudo make install-plugin`
  again.
- **The device is listed but there is no sound.** The bridge is not running, or
  it has not finished pairing. Check the menu, and use **Open Log** to see the
  handshake. A healthy start ends with authentication completing and audio
  streaming.
- **The bridge will not find the dongle.** Check that macOS sees it:
  ```bash
  system_profiler SPUSBDataType | grep -A4 -i "0234"
  ```
  If nothing shows up, try another USB port or cable, or plug the dongle in
  directly rather than through a hub.
- **The menu says "Connected (started elsewhere)".** A `gip-bridge` started
  from a terminal holds the dongle. That works, but the app cannot run its own
  bridge until it exits. Stop it with ctrl-c, or with **Stop Bridge** in the menu.
- **Building fails with a missing `openssl` header or `libcrypto`.** Run
  `brew install openssl@3`. On an Intel Mac, Homebrew lives in `/usr/local`,
  which the Makefile finds through `brew --prefix`.
- **Reporting a problem.** Choose **Copy Diagnostics** in the menu and paste the
  result into a GitHub issue. The log is at `~/Library/Logs/XboxHeadsetBridge.log`.

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
| `build/gip-status` | One-shot or continuous (`-c`) display of volume, mute and stream stats |

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
- **Authentication is required before audio streams are useful.** The PDP
  dongle performs the v1 GIP RSA exchange (host hello, client hello,
  certificate, encrypted secret, host finish). This firmware acknowledges the
  client-finish request but does not return a client-finish payload; matching
  the Windows capture, the bridge treats the GIP acknowledgement for request
  `0x08` as completion and sends the 2-byte auth-complete control message.
- **Mic packets are 54 bytes, not 128 bytes.** Each packet is a 6-byte GIP
  header plus 48 bytes of 24 kHz mono S16. Requesting the endpoint's advertised
  128-byte maximum caused macOS to complete reads with zero bytes; the bridge
  requests 54 bytes per frame.
- **Restart the isochronous IN pipe after authentication.** Reads submitted
  during the RSA exchange can become stale (`0xe00002ee`,
  `kIOReturnIsoTooOld`). The bridge aborts those reads, rebases USB frames, and
  submits fresh capture transfers after sending device Start. An aborted read
  during that deliberate restart is not a physical disconnect.

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
- [x] **Milestone 3** — capture the microphone from the iso IN endpoint
      (`src/mic.c`, 24 kHz mono with 2x linear interpolation to 48 kHz).
- [x] **Milestone 4** — expose both as a real Core Audio device via an
      AudioServerPlugin in `/Library/Audio/Plug-Ins/HAL`, so every app can
      select the headset (`plugin/XboxHeadset.c` + `src/bridge.c`).
- [x] Volume/mute wheel handling (live `AUDIO_CONTROL` packets forwarded to
      HAL ring)
- [x] Auto-reconnect on USB disconnect (device pull, headset power-off)
- [ ] Battery reporting (the parser exists, but this dongle has never sent a `GIP_CMD_STATUS` packet)
- [ ] Hot-plug detection for dynamic plug/unplug without bridge restart

## Credit and license

The GIP protocol constants, header codec, and chunking semantics are ported
from [**xone**](https://github.com/medusalix/xone) by Severin von
Wnuck-Lipinski, the Linux kernel driver for Xbox accessories. xone already
matches this dongle generically (vendor `0x0e6f`, interface `ff/47/d0`) and
implements headset audio over ALSA; this project is the macOS counterpart.

xone is licensed GPL-2.0-or-later, which allows code derived from it to be
released under a later version. This project is released under the
**GNU General Public License v3.0 or later**. See [`LICENSE`](LICENSE).

## Menu bar app

`make menubar-app` builds `build/XboxHeadsetMenu.app`, a menu-bar-only app that
starts and stops the bridge so it does not have to live in a terminal. The menu
shows the headset's microphone mute state and volume. The dongle has never been
seen to report battery level, so there is no battery reading.

The bridge runs inside the app on its own thread, so there is no second process
to manage. Its output goes to `~/Library/Logs/XboxHeadsetBridge.log` ("Open Log"
in the menu). If the standalone `gip-bridge` is already running from a terminal
it holds the USB device, so the app reports that and leaves it alone rather than
fighting over the device.

### Running the bridge by hand

The bridge does not need `sudo`. Quit the menu bar app's bridge first, then:

```sh
./build/gip-bridge            # handshake logging only
./build/gip-bridge --verbose  # plus the live packet counters
```

The counter line is redrawn twice a second and is opt-in: it is useful at a
terminal and pure noise in a log file. Rebuild just the bridge after source
changes with `make -B build/gip-bridge`.

## Volume control

The HAL plug-in publishes a volume and a mute control, so the keyboard volume
keys, the menu bar slider and Audio MIDI Setup all work while the headset is the
default output. Reinstall the plug-in after building it:

```sh
sudo make install-plugin
```
