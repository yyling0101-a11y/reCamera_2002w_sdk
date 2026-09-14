# reCamera ONVIF / RTSP example

Run `recamera-onvif` after installation. The program listens on ONVIF port
8000 and RTSP port 8554, using the configured `admin` / `recamera.1` account.

This package is for the reCamera 2002W (SG2002, RISC-V/musl) firmware. It
contains the matching `librecamera_sdk.so.0` and modified `libcvi_rtsp.so`;
the board-provided ISP, media, OpenSSL, audio, and runtime libraries remain
firmware dependencies and are intentionally not included.
