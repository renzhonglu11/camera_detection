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

## Pan Tracking

Horizontal ball tracking is opt-in and uses the PCA9685 pan servo channel:

```bash
./camera_detection --pan-tracking --no-display
```

If the camera turns away from the ball, reverse the direction:

```bash
./camera_detection --pan-tracking --pan-invert --no-display
```

To verify PCA9685 wiring and channel selection without relying on ball detection:

```bash
./camera_detection --pan-tracking --pan-test-sweep --no-display
```

Use a longer hold time if the sweep is too fast:

```bash
./camera_detection --pan-tracking --pan-test-sweep --pan-test-hold-ms=2500 --no-display
```

Use slower incremental movement for gentler hardware testing:

```bash
./camera_detection --pan-tracking --pan-test-sweep --pan-test-step=1 --pan-test-step-ms=60 --no-display
```

Servo movement commands are logged to `pan_motion.csv` by default. Override the path with:

```bash
./camera_detection --pan-tracking --pan-log-csv=/tmp/pan_motion.csv --no-display
```
