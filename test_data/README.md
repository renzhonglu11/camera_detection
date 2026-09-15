# test_data

This folder is for local offline replay samples used by T01. Video files are
intentionally ignored by Git and must not be committed. Copy or record a local
sample before running replay or benchmark commands. Enable the repository's
pre-commit guard with `git config core.hooksPath .githooks` after cloning.

Expected sample for the first round:

- `blue_ball_basic.mp4`
- observed frame size: 1280×720
- source rate reported by OpenCV: 30 FPS
- expected frame count: 593
- capture device/date and audio-track status: not documented

Usage for quick checks:

```bash
./build-cross/camera_detection \
  --input=./test_data/blue_ball_basic.mp4 \
  --replay=unpaced \
  --no-display \
  --no-stream
```

For T01 reproducibility checks, use the helper script:

```bash
./scripts/run_t01_replay.sh --input=./test_data/blue_ball_basic.mp4
```

For WebRTC replay, the caps must match the sample's native resolution:

```bash
./build-cross/camera_detection \
  --input=./test_data/blue_ball_basic.mp4 \
  --replay=realtime \
  --width=1280 \
  --height=720 \
  --no-display \
  --stream
```

The file-input path does not resize frames. Using the 512×384 defaults for this
sample corrupts the WebRTC image because the declared BGR caps no longer match
the `cv::Mat` layout.
