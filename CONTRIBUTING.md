# Contributing

Thanks for considering a contribution to LLK Remote.

## Development Setup

1. Install Visual Studio 2022 Build Tools with the C++ workload.
2. Install CMake 3.20 or newer.
3. Make `ffmpeg.exe` available on `PATH` or pass it with `--ffmpeg`.
4. Build with:

```powershell
.\build.ps1
```

## Pull Requests

Keep changes focused. Good pull requests usually include:

- a short explanation of the behavior being changed
- manual test notes for host and viewer flows
- protocol compatibility notes if packet structures change
- documentation updates when commands or ports change

## Areas That Need Help

- better setup documentation
- non-NVIDIA encoder fallback
- protocol parsing tests
- optional authentication
- packaging and release automation
