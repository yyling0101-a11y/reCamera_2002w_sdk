# CVI RTSP source notice

This directory contains the CVI RTSP core derived from Seeed Studio's
`sscma-example-sg200x` repository, licensed under Apache-2.0. The upstream
license is available at `../sscma/LICENSE`.

Local modifications:

- optional Live555 Digest authentication configuration;
- authentication database lifetime management;
- H.264/H.265 auxiliary SDP generation for live pushed streams;
- H.264 `fmtp`, `profile-level-id`, and `sprop-parameter-sets` compatibility
  required by NVRs such as Hikvision.
