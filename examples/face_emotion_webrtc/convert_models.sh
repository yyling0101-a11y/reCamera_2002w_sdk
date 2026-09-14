#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MODEL_DIR="${SCRIPT_DIR}/models/reference"
RELEASE_BASE="https://github.com/RobotXTeam/sscma-example-sg200x/releases/download/v1.0.1"

mkdir -p "${MODEL_DIR}"

download() {
    local name="$1"
    [[ -s "${MODEL_DIR}/${name}" ]] || curl -fL --retry 5 --retry-all-errors \
        --connect-timeout 20 -o "${MODEL_DIR}/${name}" "${RELEASE_BASE}/${name}"
}

# Reuse the validated CV181x conversions from the SSCMA reference. Their
# combined ION footprint is known to fit SG2002W; converting every stage to
# BF16 independently can exceed the device's ION pool.
download yolo-face_mixfp16.cvimodel
download age_gender_race_bf16.cvimodel
download emotion_bf16.cvimodel

(
    cd "${MODEL_DIR}"
    sha256sum *.cvimodel > SHA256SUMS
)
