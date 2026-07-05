# LLK Remote

LLK Remote is a lightweight Windows LAN remote-control prototype written in
C++20. It connects one controller machine to one controlled machine with three
separate channels:

- low-latency video from host to viewer over UDP
- mouse and keyboard control from viewer to host over UDP
- one-shot text, file, and directory transfer from viewer to host over TCP

The project is intentionally small. It uses Win32, Winsock, D3D11, DXGI, and an
external `ffmpeg.exe` process instead of a large UI or remote-desktop framework.

## Status

This repository is being prepared as an open-source release of the original LLK
implementation. It is suitable for LAN experiments, learning, and further
development. It is not a hardened remote-administration product.

Important current limits:

- LAN-focused; no NAT traversal or relay server
- no authentication or encryption layer yet
- Windows-only
- video path depends on `ffmpeg.exe`
- the default encoder path uses `ddagrab` and `h264_nvenc`, so NVIDIA hardware
  is recommended on the host

Do not expose the ports used by this project to the public Internet.

## Architecture

`llk_host.exe` runs on the controlled Windows machine.

It:

- starts an `ffmpeg` sender for screen capture and H.264 encoding
- receives pointer and keyboard packets
- applies input with Win32 APIs
- receives clipboard text and file-transfer requests

`llk_viewer.exe` runs on the controller Windows machine.

It:

- starts a local `ffmpeg` receiver
- reads decoded BGRA frames from `ffmpeg` stdout
- renders frames with D3D11
- sends pointer and keyboard events back to the host
- sends pasted text, files, or directories over the transfer channel

Default ports:

- `52334/UDP` for video
- `52333/UDP` for input control
- `52335/TCP` for transfer

More detail is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Repository Layout

```text
.
|-- CMakeLists.txt
|-- build.ps1
|-- include/
|   |-- llk_protocol.h
|   `-- llk_transfer.h
`-- src/
    |-- host_main.cpp
    `-- viewer_main.cpp
```

## Build

Requirements:

- Windows 10 or newer
- Visual Studio 2022 Build Tools with the C++ workload
- CMake 3.20 or newer

Build with PowerShell:

```powershell
.\build.ps1
```

or directly with CMake:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binaries are generated under:

```text
build/Release/
```

## Run

Install or place `ffmpeg.exe` where the programs can find it. The simplest path
is to put `ffmpeg.exe` next to the built binaries or make it available on `PATH`.

On the controlled machine:

```powershell
.\llk_host.exe --viewer-host <viewer-ip> --video-port 52334 --control-port 52333 --transfer-port 52335 --fps 30 --bitrate-mbps 8 --ffmpeg ffmpeg.exe
```

On the controller machine:

```powershell
.\llk_viewer.exe --agent-host <host-ip> --video-port 52334 --control-port 52333 --transfer-port 52335 --width 1920 --height 1080 --fps 30 --ffmpeg ffmpeg.exe
```

Open the required ports in Windows Firewall for your LAN test environment.

## Protocols

The control protocol is defined in [include/llk_protocol.h](include/llk_protocol.h).
It contains packet types for pointer events, keyboard events, viewer-host sync,
and host sync-state responses.

The transfer protocol is defined in [include/llk_transfer.h](include/llk_transfer.h).
It supports one-shot clipboard text, file, directory, commit, and reset messages.

## Roadmap

Good first areas for contributors:

- document repeatable setup on fresh Windows machines
- add a software encoder fallback for machines without NVIDIA NVENC
- add optional authentication for LAN sessions
- add integration tests for protocol parsing
- make the viewer display connection and transfer state more clearly
- package `ffmpeg` discovery and error reporting better

## Security

This project currently trusts the local network. See [SECURITY.md](SECURITY.md)
before testing it outside a private lab.

## License

MIT. See [LICENSE](LICENSE).
