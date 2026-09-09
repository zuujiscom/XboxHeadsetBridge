# XboxHeadsetBridge reverse-engineering memory

## Hardware

PDP LVL50 Wireless for Xbox dongle: VID/PID `0e6f:0234`, firmware `1.0.1.4`,
hardware `1.1.1.1`. It is a vendor-class GIP device, not a normal USB Audio
Class device. Interface 0 carries interrupt GIP control traffic. Interface 1
alternate setting 1 carries isochronous audio: OUT `0x02`, IN `0x83`.

## Working audio format

The dongle reports input `0x09` (24 kHz mono) and output `0x10` (48 kHz stereo).
Output uses 192 PCM bytes per 1 ms packet. Input uses 48 PCM bytes per 1 ms
packet, with a 6-byte GIP header, so the actual input packet is 54 bytes even
though the endpoint advertises a 128-byte maximum.

## Authentication

The v1 sequence is:

1. Host hello (`0x01`)
2. Request/client hello (`0x02`)
3. Request/client certificate (`0x03`, 815 bytes in the Windows capture)
4. Host encrypted secret (`0x05`, chunked)
5. Host finish (`0x07`)
6. Request client finish (`0x08`)

This PDP firmware acknowledges request `0x08` but does not provide a client
finish payload in the supplied `captures.pcapng`. The Windows-compatible bridge
therefore treats the GIP acknowledgement as completion and sends auth control
complete (`{0x01, 0x00}`).

## Windows capture conclusions

The supplied capture contains no hidden USB Audio class request (`0x21`, `0xa1`,
or vendor control) for the PDP dongle address. The relevant downstream setup is
GIP Audio Control format followed by GIP Set Device State Start. Do not add
unverified “magic” class transfers.

## macOS USB behavior

`0xe00002ee` is `kIOReturnIsoTooOld`: the requested first isochronous frame is
already in the past. Authentication and CoreAudio callbacks share a run loop,
so long auth exchanges can stale queued transfers. The bridge deliberately
restarts the IN pipe after authentication and treats the resulting aborted
callbacks as recoverable.

## Known-good validation

Successful bridge output has the following shape:

```text
AUTH request 0x08 ACK; completing authentication
AUDIO capture state: Start (post-auth)
bridge running - bidirectional audio active
in_usb=... audio=...
```

`in_usb` and `audio` should increase together; `other` should remain zero. A few
startup `in_err` values are expected from stale frames, but the process should
remain running and continue counting input packets.
