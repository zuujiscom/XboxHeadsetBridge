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

## Important files

- `src/bridge.c`: CoreAudio ring ↔ GIP audio, auth sequencing, isochronous I/O.
- `src/gip_auth.c`: v1/v2 auth packet construction and RSA/ECDH helpers.
- `src/gipusb.c`: IOKit interface, interrupt and isochronous pipe operations.
- `src/mic.c`: standalone WAV microphone diagnostic.
- `plugin/XboxHeadset.c`: AudioServerPlugin HAL device.
- `shared/ring.h`: shared-memory SPSC ring between HAL and bridge.
- `windows-driver-10.0.26100.9444/`: reference Windows USB Audio driver files.

## Verification

After changes, rebuild with `make -B build/gip-bridge`, run the bridge with the
dongle paired, and verify `in_usb` and `audio` become nonzero. Do not “fix” the
working path by adding speculative USB Audio class control requests: the Windows
capture contains no such request for device address 6; the relevant setup is GIP
authentication plus Set Device State Start.
