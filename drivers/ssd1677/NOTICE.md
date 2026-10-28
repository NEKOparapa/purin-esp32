# SSD1677 source attribution

The panel driver, command definitions and three waveform lookup tables were
ported from the local `Metalio-E-INK4-main` source tree:

- `main/boards/metalio-e-ink-4/display/esp_lcd_panel_ssd1677.c`
- `main/boards/metalio-e-ink-4/display/esp_lcd_panel_ssd1677.h`
- `main/boards/metalio-e-ink-4/display/esp_lcd_ssd1677_commands.h`
- `main/display/LUT/epd_lut_fastupdate.h`
- `main/display/LUT/epd_lut_partial.h`
- `main/display/LUT/epd_lut_4gray.h`

The board power, shared I2C and SPI setup in `board/`, together with the
IO expander driver in `drivers/tca9555/`, is adapted from
`main/boards/metalio-e-ink-4/config.h`, `bus/metalio_i2c_bus.cc`,
`bus/metalio_io_expander.cc`, `display/metalio_epd.cc`, and
`main/boards/common/bus/IOExpander.hpp` in that source tree (root MIT license).
The board definitions are centralized in `config/board_config.h`. The touch
reset sequence and native-to-landscape mapping follow upstream
`main/boards/metalio-e-ink-4/display/metalio_touch.c` and
`main/display/display_orient.cc`. The touch controller implementation itself
is the managed Espressif `esp_lcd_touch_cst816s` component, with its own
Apache-2.0 license retained in the downloaded component.
The SD pin assignment and 1-bit wiring follow upstream
`main/boards/common/storage/SdCardManager.hpp` and
`main/boards/metalio-e-ink-4/periph/metalio_board_periph.cc`; the local
`drivers/sdcard/` uses ESP-IDF's SDMMC and FatFS implementations.

Upstream's root license is MIT, copyright (c) 2026 CloudZao, reproduced in
`LICENSE-Metalio-MIT`. The driver, command definitions, fast and partial lookup
tables additionally carry Apache-2.0 SPDX headers, retained verbatim. The
Apache 2.0 license text is reproduced in `LICENSE-Apache-2.0` (copied from the
installed ESP-IDF v5.5.4 license). The grayscale lookup table has no per-file
license header; upstream's root MIT notice is retained for it.

Local changes: bounded BUSY waits, an explicit wait-ready API, synchronous
partial refresh when no callback is registered, lazy BUSY interrupt setup,
initialization failure cleanup, and validation of bitmap geometry. The
upstream waveform bytes and panel initialization commands are unchanged.
