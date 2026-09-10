# XboxHeadsetBridge agent notes

This is a macOS userspace bridge for the PDP LVL50 Wireless for Xbox dongle
(`0e6f:0234`). Read this file before changing protocol, USB scheduling, or
authentication code.

## Current known-good state

- `make -B build/gip-bridge` builds successfully with Apple Clang and OpenSSL 3.
- `./build/gip-bridge` authenticates the dongle, streams playback, and captures
  microphone audio. Healthy counters show `in_usb` and `audio` increasing.
- The USB audio interface is alt 1: OUT `0x02` (224 bytes), IN `0x83` (128-byte
  advertised maximum). Actual mic packets are 54 bytes.
- Do not require `sudo make install-plugin` to test the bridge; that is only for
  installing the HAL plug-in.

## Protocol facts

- GIP control traffic is interrupt interface 0; audio is interface 1 alt 1.
- Audio format is input 24 kHz mono and output 48 kHz stereo.
- Mic packets are one `AUDIO_SAMPLES` (`0x60`) packet per 1 ms frame: 6-byte
  GIP header plus 48 bytes S16 PCM.
- The v1 auth exchange reaches host finish. This dongle acknowledges request
  `0x08` but does not emit a client-finish payload in the supplied Windows
  capture. Treat that acknowledgement as completion and send auth control
  complete, as implemented in `src/gip_auth.c`.
- `0xe00002ee` is `kIOReturnIsoTooOld`, a stale isochronous frame. During auth,
  reset the IN pipe and rebase frames; do not interpret an intentional
  `kIOReturnAborted` restart callback as a device disconnect.

## Host-side control surface

- `plugin/XboxHeadset.c` publishes a **volume and a mute control** on the output
  scope (`kObjectID_Volume_Output`, `kObjectID_Mute_Output`). Without them the
  device reports no volume at all, and the keyboard volume keys and the menu bar
  slider are inert whenever the headset is the default output. The gain is
  applied in `DoIOOperation`'s `WriteMix` before the samples reach the ring.
- Scalar 0...1 maps linearly onto `kVolumeMinDB`...`kVolumeMaxDB` (-40...0 dB),
  so 50% lands near -20 dB. Scalar 0 is a true zero rather than -40 dB.
- The plug-in mirrors its state into `host_vol_out`/`host_muted`. Keep that
  separate from `vol_out`/`vol_in`, which are the *headset's own* dial values as
  parsed off the GIP control stream — two independent numbers that must not
  overwrite each other.

## What the dongle actually supports

Reassembled from the device's own chunked IDENTIFY response in
`WirelessHeadset/captures.pcapng` (174 bytes, three 58-byte chunks). The
offsets in `gip_pkt_identify` are relative to the **end of its 16-byte unknown
block**, not to the start of the payload.

```text
Device class            Windows.Xbox.Input.Headset
Audio formats  (1)      in 0x09 (24 kHz mono)  out 0x10 (48 kHz stereo)
System commands OUT (7) 01 ACKNOWLEDGE  02 ANNOUNCE  03 STATUS  04 IDENTIFY
                        06 AUTHENTICATE 08 AUDIO_CONTROL 60 AUDIO_SAMPLES
System commands IN  (6) 01 ACKNOWLEDGE  04 IDENTIFY  05 SET_DEVICE_STATE
                        06 AUTHENTICATE 08 AUDIO_CONTROL 60 AUDIO_SAMPLES
Interface GUIDs         {9776ff56-9bfd-4581-ad45-b645bba526d6}
                        {bc25d1a3-c24e-4992-9dda-ef4f123ef5dc}
HID descriptor          none (offset 0)
```

The device declares no *input* capability: no VIRTUAL_KEY (0x07), no HID_REPORT
(0x0b), no INPUT (0x20) and no HID descriptor. That is true, but do **not**
conclude from it that physical controls cannot reach the host — the volume dial
reports live over AUDIO_CONTROL (0x08), which is declared. See below. The
bass-boost button has not been observed on any command.

The single audio format pair is exactly what `bridge.c` negotiates. There is no
alternative rate to select.

## Volume reporting

The two volume subcommands have **different field orders**:

```c
struct gip_pkt_audio_volume_chat {  /* 0x00 */
	u8 subcommand, mute, gain_out, out, in;
};
struct gip_pkt_audio_volume {       /* 0x03 */
	u8 subcommand, mute, out, chat, in, unknown1, unknown2[2];
};
enum { GIP_AUD_VOLUME_UNMUTED = 0x04, GIP_AUD_VOLUME_MIC_MUTED = 0x05 };
```

`mute` is an **enum, not a bitmask** — testing it with `& 0x04` reports unmuted
for the muted value too.

This dongle only ever sends 0x00, and **both of its physical dials report
live**. The mapping was established by sweeping each dial in isolation while the
other was left alone, which is the only reliable way to tell them apart:

```text
main dial swept, chat parked:   00 04 18 64 64 ... 00 04 64 64 64   p[2] moves
chat dial swept, main parked:   00 04 4a 00 64 ... 00 04 4a 64 64   p[3] moves
```

| Byte | xone name  | What it actually is on this device |
|------|------------|------------------------------------|
| p[2] | `gain_out` | **Main volume dial**, 0-100        |
| p[3] | `out`      | **Chat dial**, 0-100               |
| p[4] | `in`       | Constant 100 — not a dial          |

Note that xone's field names do **not** describe this hardware: the chat level
arrives in `out`, and `in` is inert. Trust the sweep, not the name — wiring the
"Chat" readout to `in` because of its name produced a bar that never moved.

**The headset applies both dials itself.** Turning either audibly changes the
audio with no host involvement, so these are *reports*, for display only. Do not
scale the outgoing PCM by it — that would attenuate a second time on top of the
hardware. An earlier attempt to apply a volume field to the PCM silenced the
headset outright: the ring persisted a stale `vol_out` of 0 across a bridge
restart and every sample was multiplied by zero.

**A gain must never be applied from a status field without its `_seen` flag.**
0 is a legitimate value and is indistinguishable from "never reported". The same
applies to `battery_level`/`battery_seen`. `ring_reset_status()` clears these at
bridge startup, since `ring_map` preserves an existing ring and the status
fields describe the headset in front of us, not the last one.

### A warning about diagnosing this hardware

An earlier round of investigation concluded, and briefly documented, that the
dial produced *no* GIP traffic whatsoever — based on a 90-second watch during
which the dial was turned and nothing arrived, plus handshake packets that never
varied. That was wrong. The connection was already degraded at the time and
failed completely a little later; after a headset power-cycle and a re-pair, the
same dial reports perfectly.

When this dongle is in a half-connected state it still enumerates, still accepts
every packet you send, and still completes some of the handshake, while sending
almost nothing back. Before concluding that the hardware *cannot* do something,
confirm the link is healthy: `in_usb` and `audio` climbing together, and volume
packets arriving when the dial moves. A silent device is far more often a sick
link than a missing capability.

## Battery

`GIP_CMD_STATUS`'s first payload byte packs the battery type (bits 2-3) and
level (bits 0-1). The bridge still parses it and the ring still carries
`battery_level`/`battery_type`/`battery_seen`, but **no STATUS packet has ever
been observed** — not in the Windows capture, not in any macOS session. The menu
bar app therefore does not show a battery reading at all, and `gip-status` no
longer prints one. Do not re-add a battery UI without first confirming a STATUS
packet actually arrives.

## The underruns counter

`underruns` counts short reads from the playback ring, but only while the HAL
plug-in is actually running IO (`out_io_running`). Without that condition the
number is meaningless: when nothing is playing, coreaudiod stops the plug-in's
IO and the ring stops being written, while the bridge keeps sending its
isochronous stream at 125 transfers a second. That produced 8,743,902
"underruns" on one long session, which is exactly 19.4 hours of an idle device
(8.7M / (3600/0.008)) and not a single real dropout.

A genuine underrun now means the ring ran dry with audio playing. The backlog
is thin by design -- it sawtooths with coreaudiod's buffer size, because the HAL
writes in that size while the bridge reads exactly 384 frames (8 ms) per
transfer. That oscillation is normal; a sustained backlog below 384 frames is
not.

## Playback latency is bounded

`ring_out_read`/`ring_in_read` cap the queued-but-unplayed backlog at
`RING_TARGET_BACKLOG` (1536 frames, 32 ms), trimming `RING_TRIM_FRAMES` (64,
~1.3 ms) per read once it is exceeded.

The emergency skip only fires when the writer has lapped the entire ring
(683 ms), so **any smaller backlog used to persist forever**. An interruption
that let the writer run ahead -- a bridge restart while the HAL kept writing --
permanently bought that much latency; one recovery left 96 ms of it, and it
would not have cleared until coreaudiod next restarted the plug-in's IO
(`StartIO` resyncs the read pointer).

Trim is a ceiling, not a target: normal operation never reaches it and nothing
is trimmed. Correcting gradually matters -- discarding the whole excess at once
is an audible gap, whereas 1.3 ms per 8 ms read reads as slight time
compression. A 96 ms backlog converges in ~336 ms of audio, the 683 ms worst
case in ~3.9 s.

Do not verify this from a live backlog measurement taken after a restart: a
restart resets the pointers on its own, so the number looks good whether or not
the trim works. Exercise `ring_trim()` directly instead.

## Reference material

- `WirelessHeadset/captures.pcapng` — Windows USBPcap capture (connection and
  microphone; no volume changes were performed during it). **Not tracked in
  git** — it is 18 MB of binary, so it is gitignored and lives only alongside
  the working copy. Everything derived from it is written down here.
- `WirelessHeadset/headset-capture-analysis.md` — its analysis.
- Microsoft's GIP is a published open standard: **[MS-GIPUSB]** on
  learn.microsoft.com, which supersedes the community reverse-engineering
  notes. It defines Audio Control Configuration, Audio Control Volume Extended,
  Set Device State, and the Extended Status/battery messages.
- `windows-driver-10.0.26100.9444/` is Microsoft's *generic USB Audio 2.0 class*
  driver, bound by `Class_01`, not by VID/PID. It never binds to this
  vendor-class dongle and tells you nothing about it.

The capture has no Wireshark dependency: `linktype 249` (USBPcap) parses with a
27-byte packed header, and GIP lengths and chunk offsets are **varints**, not
plain bytes.

## Menu bar app

`menubar/` builds `build/XboxHeadsetMenu.app`, an `LSUIElement` SwiftUI app that
runs the bridge and shows mic and volume state. `make install-menubar` drops it
into `/Applications`.

**The bridge runs inside the app, on a thread — not as a child process.**
`src/bridge.c` is compiled once into objects that are linked both into the app
and into the `gip-bridge` CLI. The app drives it through `src/bridge.h`
(`bridge_run` / `bridge_stop` / `bridge_is_running`).

This works only because of two existing properties, so preserve them:

- `run_session()` polls with `CFRunLoopRunInMode(..., timeout, false)` rather
  than blocking in `CFRunLoopRun()`, so `stop` is checked frequently.
- `gipusb.c` attaches its event sources to `CFRunLoopGetCurrent()`, so the
  bridge simply needs a thread it can keep for the whole session. Open, run and
  close must all happen on that one thread.

There is one bridge per process: the implementation keeps its state in
file-scope globals.

Other notes:

- The CLI holds the USB device exclusively while it runs. `BridgeController`
  detects a standalone `gip-bridge` and refuses to start its own — do not
  "fix" this by killing it, since it is usually someone's debugging session.
- A menu bar app has no stdout, so `BridgeController` redirects stdout/stderr
  to `~/Library/Logs/XboxHeadsetBridge.log` once, before first start.
- The app talks to the ring through `menubar/ringshim.c`, a flat-snapshot C
  shim. Do not try to import `shared/ring.h` into Swift directly: `_Atomic` and
  the multi-megabyte payload do not bridge.
- `libcrypto` is copied into `Contents/Frameworks` with both install names
  rewritten to `@rpath`, because it would otherwise be linked by an absolute
  Homebrew path that exists on no other machine.
- Merging the bridge in gave up crash isolation: a fault in the USB layer now
  takes the whole app down rather than just the bridge.
- **Quit the app before rebuilding it.** The bundle is ad-hoc signed, so
  rewriting the executable under a running process invalidates that process's
  code signature and the kernel kills it silently — no crash report, the menu
  bar item just disappears. `make menubar-app` while it is running looks like
  the app crashing for no reason.

## Logging

`gip-bridge` prints handshake progress unconditionally — that is what makes a
failed pairing diagnosable — but the twice-a-second counter line is behind
`-v`/`--verbose`. It is redrawn with `\r`, which is useful at a terminal and
pure noise in a log file. The menu bar app leaves `--verbose` off and redirects
stdout to `~/Library/Logs/XboxHeadsetBridge.log`.

## Important files

- `src/bridge.c`: CoreAudio ring ↔ GIP audio, auth sequencing, isochronous I/O.
- `src/gip_auth.c`: v1/v2 auth packet construction and RSA/ECDH helpers.
- `src/gipusb.c`: IOKit interface, interrupt and isochronous pipe operations.
- `src/mic.c`: standalone WAV microphone diagnostic.
- `plugin/XboxHeadset.c`: AudioServerPlugin HAL device.
- `shared/ring.h`: shared-memory SPSC ring between HAL and bridge. New status
  fields are carved out of `_reserved` so the struct size never changes and a
  new bridge can attach to a ring an older plug-in already mapped. Preserve that
  property.
- `menubar/`: the menu bar app and its C shim.
- `Tools/makeicon.swift`: draws the app icon. It is generated, not committed as a binary — the script renders each size natively so nothing is upscaled, and `make menubar-app` runs it into `build/XboxHeadsetMenu.icns` and copies that into the bundle. Edit the script rather than dropping in a pre-made `.icns`.
- `windows-driver-10.0.26100.9444/`: reference Windows USB Audio driver files.

## Verification

After changes, rebuild with `make -B build/gip-bridge`, run the bridge with the
dongle paired (add `--verbose` to see the counters), and verify `in_usb` and
`audio` become nonzero. After plug-in changes, `sudo make install-plugin`, then
confirm the device still appears in Audio MIDI Setup *and* that its volume
slider moves — a malformed control object can make coreaudiod drop the device
entirely. Do not “fix” the
working path by adding speculative USB Audio class control requests: the Windows
capture contains no such request for device address 6; the relevant setup is GIP
authentication plus Set Device State Start.
