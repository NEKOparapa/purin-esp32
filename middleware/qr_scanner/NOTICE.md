# QR scanner attribution

`quirc/` contains Daniel Beer's `quirc` QR-code recognition library
(`https://github.com/dlbeer/quirc`), distributed under the ISC license in
`quirc/LICENSE`.

`qr_scanner.c` decodes UVC MJPEG frames via the ESP32-S3 ROM `tjpgd`
decompressor (injecting the standard ITU-T.81 Annex K Huffman segment when a
UVC camera omits `DHT`), generates optional 1-bit dithered preview bitmaps, and
extracts QR payloads with mirrored-symbol fallback.
