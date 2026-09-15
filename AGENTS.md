# AGENTS.md

Guidance for coding agents working in this repository.

## Project Overview

`camera_detection` is a C++17 Raspberry Pi camera application. It captures frames through GStreamer/OpenCV, detects a blue ball, overlays detection information, serves a browser preview page, and publishes frames through a WebRTC streamer.

Key areas:

- `src/main.cpp`: application entrypoint, CLI parsing, capture loop, detection overlay, WebRTC publishing.
- `src/camera/` and `include/camera/`: GStreamer camera pipeline construction.
- `src/detection/` and `include/detection/`: OpenCV ball detection.
- `src/streaming/` and `include/streaming/`: WebRTC streaming backend plus `index.html`.
- `src/hardware/` and `include/hardware/`: HTTP server and legacy PCA9685 servo driver code.
- `src/utils/` and `include/utils/`: file, HTTP, logging, and signal helpers.
- `scripts/`: deployment and debugger helper scripts.
- `tests/`: currently reserved; no automated tests are present yet.

## Build And Run

This project is intended to be cross-compiled for aarch64 Raspberry Pi from Linux.

Configure and build:

```bash
cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake
cmake --build build-cross -j
```

The build expects the local Raspberry Pi sysroot and ARM toolchain paths currently encoded in `toolchain.cmake` and `CMakeLists.txt`, including OpenCV and GStreamer WebRTC development headers. If configure fails on missing headers or libraries, verify the sysroot before changing source code.

The CMake post-build step copies `src/streaming/index.html` to `build-cross/index.html`; deployment depends on both `build-cross/camera_detection` and `build-cross/index.html`.

Run on the Pi after deployment:

```bash
./scripts/deploy_run_camera.sh mypi --run
```

Deployment reads `.env.local`, which is user-specific and must not be committed. Use `.env.example` as the committed template.

## Verification

Preferred verification after C++ changes:

```bash
cmake --build build-cross -j
```

If changing CMake, toolchain paths, dependency discovery, or generated compile commands, also re-run configure:

```bash
cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake
```

There are no project tests at the moment. If adding tests, keep them under `tests/` and wire them into CMake rather than using ad hoc commands.

## Coding Conventions

- Use C++17 and keep code compatible with the cross compiler in `toolchain.cmake`.
- Prefer existing subsystem boundaries: public headers in `include/<subsystem>/`, implementations in `src/<subsystem>/`.
- Match the current formatting style: tabs for indentation in C++ files, braces on the same line for functions and control blocks, and `snake_case` for free functions and local variables.
- Keep OpenCV-facing code explicit with `cv::` qualification. Avoid adding broad `using namespace` directives in headers.
- Avoid expensive per-frame work in the capture loop unless it is necessary; this application is latency- and FPS-sensitive.
- Be conservative with logging in the frame loop. Periodic status output is fine, per-frame noisy logs are not.
- Keep hardware, streaming, detection, and utility responsibilities separated. Do not put new hardware control or streaming behavior directly into unrelated modules.

## Runtime And CLI Notes

Current runtime options are parsed in `src/main.cpp`:

- `--display` / `--no-display`
- `--width=N`
- `--height=N`
- `--fps=N`
- `--preview-width=N`
- `--display-every=N`
- `--http-port=N`
- `--webrtc-encoder=auto|h264-hw|openh264`

When adding or changing options, update the parser, usage string, relevant defaults in `include/utils/app_types.h`, and any documentation that mentions invocation.

## Secrets And Local State

- Do not commit `.env.local`, build directories, binaries, sysroot contents, SSH details, or machine-specific generated files.
- Do not commit camera recordings or other video files. Keep local replay media under `test_data/`; only `test_data/README.md` is versioned. Do not bypass `.githooks/pre-commit`; enable it with `git config core.hooksPath .githooks` when needed.
- Treat paths in `toolchain.cmake`, `.clangd`, and CMake cache files as local development assumptions. Avoid broad path rewrites unless the task specifically asks for portability work.
- The working tree may contain user changes. Do not revert unrelated modifications.
