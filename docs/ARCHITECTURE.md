# Architecture

LLK Remote splits remote-control behavior into three independent channels.

## Video Channel

Direction: host to viewer

Transport: UDP

Default port: `52334`

The host launches `ffmpeg.exe` to capture the desktop with `ddagrab`, encode the
stream with H.264, and send MPEG-TS packets over UDP. The viewer launches its
own `ffmpeg.exe` to receive and decode the stream into BGRA raw video. The viewer
then uploads frames to a D3D11 texture and presents them in a Win32 window.

## Control Channel

Direction: viewer to host

Transport: UDP

Default port: `52333`

Pointer and keyboard events are serialized with the control protocol in
`include/llk_protocol.h`. The host receives those packets and applies the input
with Win32 APIs.

## Transfer Channel

Direction: viewer to host

Transport: TCP

Default port: `52335`

Text, files, and directories are sent with the transfer protocol in
`include/llk_transfer.h`. The transfer channel is separate from input control so
large file copies do not block mouse and keyboard packets.

## Why Separate Channels

Video, input, and transfer have different latency and reliability needs:

- video needs steady throughput
- input needs short delay
- file transfer needs reliable ordered delivery

Keeping the channels separate makes failures easier to diagnose and avoids
coupling large transfers to the input path.

## Main Binaries

`llk_host.exe`

- parses host settings
- manages video sender lifecycle
- receives control packets
- accepts transfer connections

`llk_viewer.exe`

- parses viewer settings
- displays decoded video frames
- sends control packets
- sends clipboard and file-transfer requests
