# Model artifacts

This directory is populated by `convert_models.sh`. Large ONNX, CVIMODEL, MLIR,
and compiler work files are intentionally not committed.

The application uses the validated CV181x models under `reference/`:

- `yolo-face_mixfp16.cvimodel` for face detection;
- `age_gender_race_bf16.cvimodel` for attributes;
- `emotion_bf16.cvimodel` for facial emotion.

See `convert_models.sh` for the pinned release URLs and SHA-256 verification
file. Local conversion experiments may be kept under `work/`; that directory is
ignored by Git.
