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

**The device declares no input capability at all.** There is no VIRTUAL_KEY
(0x07), no HID_REPORT (0x0b), no INPUT (0x20) and no HID descriptor. That is the
answer to "why does the volume dial do nothing on the host": there is no channel
over which a dial, a button, or the bass-boost switch could be reported. Those
controls are internal to the headset. Do not go looking for a message that
carries them.

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

This dongle only ever sends 0x00, and only during the handshake. Observed:

```text
macOS   00 04 60 00 64     mute=UNMUTED gain_out=96  out=0 in=100
Windows 00 04 00 00 64  -> 00 04 64 00 64            out=0 in=100
```

`out` is a constant 0 on this hardware. `gain_out` is the only field that ever
differs, and it changes only across sessions, never while the dial is turned —
a 90-second watch with the dial being moved recorded no volume packet at all.
Treat all of these as a one-shot handshake report, display-only.

Consequently **nothing scales the outgoing PCM by these values.** An earlier
attempt silenced the headset completely: the ring persisted a stale `vol_out`
of 0 across a bridge restart and every sample was multiplied by zero. Host-side
volume belongs in the HAL plug-in, where the macOS volume control drives it.

**A gain must never be applied from a status field without its `_seen` flag.**
`vol_out == 0` is a legitimate value and is indistinguishable from "never
reported". The same applies to `battery_level`/`battery_seen`.
`ring_reset_status()` clears these at bridge startup, since `ring_map` preserves
an existing ring and the status fields describe the headset in front of us, not
the last one.

## Battery

`GIP_CMD_STATUS`'s first payload byte packs the battery type (bits 2-3) and
level (bits 0-1); see `GIP_STATUS_BATT_TYPE`/`GIP_STATUS_BATT_LEVEL`. STATUS is
declared in the device's capability list, but **no STATUS packet appears in the
Windows capture or in any macOS session so far**, so the battery readout may
stay empty on this dongle. `battery_seen` is what distinguishes that from a
genuine "empty" reading; do not show a level without it.

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
starts and stops `gip-bridge` and shows battery, mic and volume state. Notes:

- `gip-bridge` is copied into `Contents/Resources`, so the bundle is
  self-contained. `make install-menubar` drops it into `/Applications`.
- The app talks to the ring through `menubar/ringshim.c`, a flat-snapshot C
  shim. Do not try to import `shared/ring.h` into Swift directly: `_Atomic` and
  the multi-megabyte payload do not bridge.
- The daemon is stopped with `SIGINT`, not `SIGKILL`, so its handler can unwind
  the USB transfers and close the interface.
- `BridgeController` also detects a `gip-bridge` started from a terminal and
  refuses to manage it.
- **Quit the app before rebuilding it.** The bundle is ad-hoc signed, so
  rewriting the executable under a running process invalidates that process's
  code signature and the kernel kills it silently — no crash report, the menu
  bar item just disappears. `make menubar-app` while it is running looks like
  the app crashing for no reason.

## Logging

`gip-bridge` prints handshake progress unconditionally — that is what makes a
failed pairing diagnosable — but the twice-a-second counter line is behind
`-v`/`--verbose`. It is redrawn with `\r`, which is useful at a terminal and
pure noise in a log file. The menu bar app runs the daemon without `--verbose`
and captures stdout to `~/Library/Logs/XboxHeadsetBridge.log`.

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
