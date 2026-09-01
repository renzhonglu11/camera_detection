# camera_detection

WebRTC camera preview application for Raspberry Pi with a modular project layout.

## Layout

- `include/`: public headers grouped by subsystem.
- `src/main.cpp`: application entrypoint.
- `src/camera/`: camera capture pipeline code.
- `src/control/`: runtime options and CLI parsing.
- `src/hardware/`: low-level HTTP server and legacy hardware driver code.
- `src/streaming/`: WebRTC streaming implementation and frontend asset.
- `src/utils/`: shared helpers for files, HTTP, and signal handling.
- `scripts/`: deployment helpers.
- `tests/`: reserved for tests.

## Build

```bash
cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake
cmake --build build-cross -j
```

## Deploy

```bash
./scripts/deploy_run_camera.sh mypi --run
```

## Streaming

Camera capture and ball detection run at 30 FPS by default. WebRTC publishing
uses an independent paced output thread at 20 FPS by default, so transient
detection or servo-control delays do not directly change the browser's frame
cadence.

Configure the two rates independently:

```bash
./camera_detection --fps=30 --stream-fps=20 --no-display
```

`--stream-fps` must not exceed `--fps`. The WebRTC path keeps only the latest
camera frame and uses small leaky queues, favoring bounded latency over
accumulating stale frames. The preview page reports actual receive FPS,
bitrate, packet loss, jitter, and browser-dropped frames.

## Pan Tracking

Horizontal ball tracking is opt-in and uses the PCA9685 pan servo channel:

```bash
./camera_detection --pan-tracking --no-display
```

Pan direction is inverted by default. To use the non-inverted direction:

```bash
./camera_detection --pan-tracking --no-pan-invert --no-display
```

To verify PCA9685 wiring and channel selection without relying on ball detection:

```bash
./camera_detection --pan-test-sweep --no-display
```

Test-sweep mode does not start the camera or ball detector. It returns to the
configured center and exits automatically.

Use a longer hold time if the sweep is too fast:

```bash
./camera_detection --pan-test-sweep --pan-test-hold-ms=2500 --no-display
```

Use slower incremental movement for gentler hardware testing:

```bash
./camera_detection --pan-test-sweep --pan-test-step=1 --pan-test-step-ms=60 --no-display
```

After the initial center command, the application waits 1000 ms by default
before continuing. Override the settling delay with
`--pan-startup-hold-ms=N`. This delay does not slow the servo's first movement
because its physical starting angle is unknown.

During a normal shutdown, the pan servo returns to center in 1-degree steps
with an 80 ms delay by default. Configure this with `--pan-return-step=N` and
`--pan-return-step-ms=N`, or disable it with
`--no-pan-return-center-on-exit`.

Servo movement commands are logged to `pan_motion.csv` by default. Override the path with:

```bash
./camera_detection --pan-tracking --pan-log-csv=/tmp/pan_motion.csv --no-display
```
