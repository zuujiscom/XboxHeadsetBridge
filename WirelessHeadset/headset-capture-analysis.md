# PDP Gaming LVL50 Wireless for Xbox One — USB microphone capture analysis

Capture analyzed: `C:\captures.pcapng`

## Device

- PDP Gaming LVL50 Wireless for Xbox One (Dongle)
- USB VID:PID: `0E6F:0234`
- USB device address during this capture: `6`
- Microphone endpoint: `0x83` (IN, isochronous)

## Windows activation sequence

Windows sends these GIP commands over endpoint `0x01`:

| Capture frame | Time (CDT) | Bytes | Interpretation |
|---:|---|---|---|
| 6307 | 07:18:40.716 | `08 20 03 03 02 09 10` | Audio Control format configuration: upstream/capture `0x09` = 24 kHz mono; downstream/render `0x10` = 48 kHz stereo. |
| 7871 | 07:18:41.741 | `05 20 05 01 00` | Set Device State = Start. |
| 10266 | 07:18:43.626 | First endpoint `0x83` payload | Microphone stream begins, after the remaining GIP security exchange. |

## Endpoint 0x83 input framing

Each input packet is 54 bytes:

```text
60 20 SS 32 C0 00 [48 bytes microphone audio]
|  |  |  |  |---- flow rate, little-endian: 0x00c0 = 192 render bytes/message
|  |  |  +------- payload length: 50 bytes
|  |  +---------- incrementing audio sequence number
|  +------------- GIP system-packet flags
+---------------- GIP Audio Capture
```

The 48 microphone bytes per 1 ms packet match 24 kHz, mono, 16-bit audio: 24 samples x 2 bytes.

## Timing observed

- 39,335 microphone packets in 39.328 seconds
- One GIP audio packet per 1 ms USB isochronous slot
- USBPcap batches 8 packets per URB
- Normal URB size: 432 bytes (`8 x 54`)
- Observed URB interval: 8.0 ms median; 7.829–8.216 ms range
- Payloads contained non-zero microphone data; this was active streaming rather than empty polling.

## Useful Wireshark filter

```text
usb.device_address == 6 &&
usb.endpoint_address == 0x83 &&
usb.transfer_type == 0 &&
usb.data_len > 0
```

For USBPcap captures, `usb.transfer_type == 0` represents isochronous transfers. The endpoint-only filter includes zero-byte submit/completion records.

## Reference

Microsoft GIP USB protocol documentation:

- Audio Control Configuration: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/8f1efd18-aefe-4062-8627-9b7bc64de4c7
- Set Device State: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/8eaad00e-97e6-4ae0-86fa-471131649d70
- Audio Capture Data Message: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/85cc41a4-73bc-4eaa-9fe4-812ac75941c0
