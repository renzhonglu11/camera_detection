# test_data

This folder is for offline replay samples used by T01.

Expected sample for the first round:

- `blue_ball_basic.mp4`
- absolute path currently used in notes:
  `/home/rz/cam_proj/PCA9685/example/camera_detection/test_data/blue_ball_basic.mp4`

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

Record here:

- recorded_with (camera/lens/lighting/phone + date)
- resolution and fps
- number of frames
- checksum/hash (e.g. `sha256sum`)
- whether audio track is present
