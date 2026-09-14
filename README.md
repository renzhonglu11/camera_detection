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

### Minimal local replay build (no Raspberry Pi modules)

```bash
cmake -S . -B build -DCAMERA_DETECTION_BUILD_PI_APP=OFF
cmake --build build -j
```

## Deploy

```bash
./scripts/deploy_run_camera.sh mypi --run
```

## Offline replay (T01)

The local/CI-safe path is to use a recorded MP4 file and run in replay mode.

```bash
./build/camera_detection \
  --input=/home/rz/cam_proj/PCA9685/example/camera_detection/test_data/blue_ball_basic.mp4 \
  --replay=unpaced \
  --no-display \
  --no-stream
```

Options:

- `--input=PATH` selects a file instead of camera input.
- `--replay=unpaced` outputs frames as fast as possible.
- `--replay=realtime` preserves source timestamps when available.
- `--no-stream` skips WebRTC output.
- `--no-display` skips GUI display.

If the same file is stable/reusable, run three times to match the T01 acceptance
expectation for reproducible frame count:

```bash
./scripts/run_t01_replay.sh --input=/home/rz/cam_proj/PCA9685/example/camera_detection/test_data/blue_ball_basic.mp4
```

If you only need a one-shot check, add `--runs=1`.

## Pipeline metrics (T02)

Enable bounded per-frame metrics by selecting an output directory:

```bash
./build/camera_detection \
  --input=./test_data/blue_ball_basic.mp4 \
  --replay=unpaced \
  --no-display \
  --no-stream \
  --metrics-dir=./results/t02-example

python3 ./scripts/analyze_metrics.py ./results/t02-example
```

Run the repeatable unpaced/realtime baseline with:

```bash
./scripts/run_t02_benchmark.sh
```

See `docs/benchmark.md` for timestamp boundaries, CSV schemas, Raspberry Pi
commands, and measurement limitations.

## Streaming

Camera capture and ball detection run at 30 FPS by default. WebRTC publishing
uses an independent paced output thread at 20 FPS by default, so transient
detection or servo-control delays do not directly change the browser's frame
cadence.

Configure the two rates independently:

```bash
./camera_detection --fps=30 --stream-fps=20 --no-display
```

When streaming a file, `--width` and `--height` must match the file's actual
frame size. File input is not automatically resized, while these options are
used for the WebRTC raw-video caps. For the included 1280×720 sample:

```bash
./camera_detection \
  --input=./test_data/blue_ball_basic.mp4 \
  --replay=realtime \
  --width=1280 \
  --height=720 \
  --no-display \
  --stream \
  --metrics-dir=./results/t02-webrtc-1280x720
```

A mismatch such as a 1280×720 Mat with 512×384 caps produces horizontal stripes
and repeated image regions even when WebRTC reports no packet loss. See
`docs/benchmark.md` for the diagnostic details and acceptance checks.

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
