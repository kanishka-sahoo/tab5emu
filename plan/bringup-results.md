# Tab5 bring-up results (Phase 1)

Measured with the `hwtest` mode (`components/hwtest`, see README) on 2026-09-23.

**Unit:** Tab5 (not Tab5X), ESP32-P4 rev v1.3, 32 MB PSRAM, **ST7123** panel + touch (the
Oct 2025 revision). Powered from USB-C; readings suggest no NP-F550 pack was attached (see
Power). microSD: 32 GB "BB1QT", FAT32.

Status legend: **done** = measured on the device; **confirmed** = checked on the device by
the owner (works), without a logged measurement. **All Phase 1 items are complete.**

## Exit-criteria numbers

| Measurement | Result | Notes |
|---|---|---|
| DSI refresh rate | **57.50 Hz** | BSP 1.3.1 timings for the ST7123 predict 57.80 Hz. Frame interval 17.31 ms, no jitter. **Not 60 Hz** (open question 1) |
| PPA 256×240 → 720×768, 3× + 90° | **6.23 ms** (SRAM source), 6.52 ms (PSRAM source) | Hardware, no CPU. 4:3 mode (3.75×/3×): 7.75 ms |
| CPU nearest-neighbour 3× rotate-blit | **10.9 ms** (SRAM source), 13.3 ms (PSRAM source) | `retro_blit_rot_nn3`, one core, straight into the PSRAM frame buffer; +0.8 ms cache write-back |
| PSRAM bandwidth (CPU, through cache) | write 60, read 87, copy 71 MB/s | Scan-out takes ~111 MB/s at the same time |
| SD raw sequential read | **15.2 MB/s** (64 KB requests), 11.5 (16 KB), 7.2 (4 KB) | SDMMC 4-bit @ 40 MHz, bypassing FAT |
| SD FAT read / write | 1.8 / 0.3 MB/s | Limited by this card's **512-byte clusters**, see below |
| SD 512 B write + fsync | 7.7 ms avg, 9.8 ms max | |
| 4 MB ROM load (FAT → PSRAM) | 2.4 s on this card | Would be ~0.3 s at raw speed |
| Audio output latency (I2S DMA queue) | **30 ms** (6 × 240 frames @ 48 kHz) | BSP default channel config |
| Actual I2S rate | 48025 Hz (+525 ppm vs esp_timer) | Well inside the ±0.5 % DRC range (D3) |
| Touch | 5 simultaneous points, **53 Hz** updates while moving | A poll costs 0.55 ms idle but **~7.9 ms with fingers down** |
| Battery idle draw | 206 mA at 7.67 V (1.6 W), screen at 60 % | On battery, hwtest idle |
| Display flip | 0.86 ms full-frame cache write-back; new buffer shows from the next frame | Double buffering works (`num_fbs = 2`) |

## Findings by subsystem

### Display (done)

- The frame-done interrupt works and both DPI frame buffers flip cleanly.
- **Refresh is 57.5 Hz, not 60.** With the BSP's ST7123 timings (70 MHz pixel clock, 802 × 1510
  total) the panel runs at 57.8 Hz nominal, 57.5 Hz measured. At that rate NES content at 60.1 Hz
  would drop about 2.6 frames per second. **Phase 2 action:** retime the DPI output to 60 Hz,
  either with a pixel clock of ≈73 MHz or a vertical front porch of ≈165 instead of 220
  (70 MHz / (802 × 1455) = 60.0 Hz), then check that the panel accepts it. The ILI9881C timings
  work out to 48.2 Hz, so older units need the same fix.
- Backlight PWM sweeps 0–100 %, and on battery 100 % draws 179 mA more than 0 %.
- **Orientation confirmed from a photo of the test pattern:** with `CONFIG_RETRO_TAB5_LANDSCAPE_CW`
  (the default) the text reads upright with the front camera on the left edge. Native row 0
  (red band) is the landscape left edge and native column 0 (green band) the bottom edge, as the
  mapping predicts. The colour bars show correct RGB order (red and blue not swapped), the
  5/6/5-bit ramps have no missing bits, and the 1-pixel checkerboard shows no moiré. Both bands sit flush against the edges of the lit area, so the
  full 720×1280 is visible with nothing cropped. During the
  first run the log recorded button-bar taps in the bar's left-to-right order, which fits a
  correct touch-to-landscape mapping.

### PPA vs CPU (done): decision D4

- **At 3× the PPA scales bilinearly, not nearest-neighbour.** Every 3×3 block centre matches
  the source exactly and the other 8 pixels are blended (88.7 % of pixels differ). **Pixel
  Perfect therefore stays on the CPU blit**, and the PPA handles Smooth / Fit / 4:3 / Stretch
  as planned.
- PPA rotation is counter-clockwise: `PPA_SRM_ROTATION_ANGLE_270` gives the same orientation as
  `RETRO_ROT_CW`.
- The CPU blit costs 10.9 ms per frame, 63 % of a frame on one core. It writes 1.1 MB into
  PSRAM and appears bound by cached PSRAM write bandwidth (about 100 MB/s effective). That fits
  on CPU1 but competes with the emulator for PSRAM. **Phase 2/7 actions:** keep the native
  frame buffer in SRAM (the PSRAM source costs 2.4 ms more), and try building one output row
  with the CPU and duplicating it with 2D-DMA.
- Cached PSRAM throughput looks low for 200 MHz x16 PSRAM. Worth revisiting (burst/cache
  settings, non-temporal copies) before SNES work.

### microSD (done): decision D6

- SDMMC 4-bit at 40 MHz mounts through the BSP. Raw reads reach 15 MB/s with 64 KB requests.
- **This card is formatted with 512-byte clusters**, and FatFs reads at most one cluster per
  request, so FAT I/O runs at 1.8 MB/s whatever the request size. A 32 KB-cluster card should
  be near the raw rate. **Actions:** document "format FAT32 with 32 KB clusters" (README), and
  have the launcher warn about small clusters (Phase 4). hwtest reports this as
  `sd.cluster_size`.
- Long file names work (63-character name round-trips).
  The BSP's "Long filenames are disabled" warning is spurious: it checks a Kconfig symbol that
  doesn't exist.
- **`rename()` does not replace an existing file on FAT (EEXIST).** The D6 pattern
  `write tmp → fsync → rename` therefore needs `unlink(target)` before the rename, which opens a
  window where only `.tmp` exists. **Phase 2 action:** the atomic-write helper must recover at
  boot, and on open, by promoting a leftover `.tmp` when the target is missing.
- One small write plus fsync takes 8–10 ms.

### Audio (done)

- ES8388 through `esp_codec_dev`, 48 kHz stereo s16. The I2S DMA queue is 30 ms, measured by
  counting frames accepted before the first blocking write. Phase 2's audio engine should own
  the I2S channel config to tune this.
- The I2S clock runs 525 ppm fast against esp_timer. DRC (D3) absorbs that easily.
- HP_DET (0x43 P7) reads 0 with nothing plugged in.
- **Confirmed by ear:** channel order is correct (440 Hz left, 660 Hz right), the speaker is
  silent with SPK_EN low, and HP_DET changes when headphones are plugged in and out. HP_DET
  read 1 during one audio run, so 1 = plugged is likely. Headphone detection is **confirmed**
  working on the device; the polarity wasn't logged.

### USB / Xbox pads (done)

- The USB host library comes up on the HS controller with USB-A VBUS on (0x44 P3).
- **Confirmed** on the device: pad enumeration, input reports, hot-plug and rumble. No descriptor
  dumps or captured reports were saved from that session. The Phase 2 parser tests need them, so
  capture them then (`usb` with a pad plugged in writes `/retro/hwtest/usb_reports.txt`).

### Touch (done)

- The ST7123 touch controller responds (firmware 3, 10 points). All 5 simultaneous points were
  tracked, and coordinates update at **53 Hz** while a finger moves.
- **A poll costs ~7.9 ms while fingers are down** (0.55 ms idle). The BSP's ST7123 driver reads a
  10-touch report over I2C at 100 kHz. Polling at the plan's ≥250 Hz would saturate the
  I2C bus shared with the expanders, INA226 and codec. **Phase 2 action:** drive touch from its
  interrupt pin (GPIO23 on the ST712x boards) so it's read only when a report is ready, raise
  the touch I2C clock to 400 kHz if the controller allows, and read only the reported number of
  points.

### Power (done)

- **Board-init bugs found and fixed (important for D13):**
  - The PI4IOE5V6408 driver resets both expanders at init. Every pin becomes a pulled-down
    input, which **disables charging (CHG_EN low)** and USB-A 5 V. The chip's power-on defaults
    are the same. The Phase 0 firmware never configured the expanders, so after a power cycle it
    wouldn't have charged. `retro_tab5_board_init()` now restores M5Unified's defaults (charging
    on, USB-A 5 V on, EXT 5 V on) and holds the C6 off.
  - The driver refuses to set the level of a pin that is still an input, so outputs must switch
    direction before their level is set.
  - The PI4IOE's input-status register reads 0 on output pins. Rail state is read from OUT_SET.
- **The INA226 is in the battery branch.** On USB-C power it reads exactly 0 mA, so current
  deltas are only meaningful on battery. Before a battery was fitted, the bus voltage wandered
  (6.59 V, 8.39 V, 4.3 V) because it was measuring the charger output.
- **On battery:** 7.67 V (3.83 V/cell), 206 mA idle (1.6 W) with the screen at 60 %.
- **Rail deltas (on battery):** the C6 draws **21 mA** when powered, and WLAN_PWR_EN low removes
  it (**R19 passes**). The speaker amp idles at 7 mA. USB-A 5 V with nothing plugged in and
  EXT 5 V draw ~0 mA.
- **Backlight (on battery):** 100 % draws **179 mA** more than 0 %, so the PWM control works (R17).
- **Charging works with CHG_EN restored:** on USB-C the pack took **+961 mA** at 7.95 V.
- **Light sleep works:** the board woke after 3004 ms. The USB-Serial-JTAG console drops during
  sleep and comes back only when the host reopens the port, so hwtest stores the sleep result
  in NVS and reports it at the next boot. Charging in light sleep (D13) is **confirmed** on the
  device; the sleep current wasn't logged.
- **Confirmed** on the device: the power-button test (GPIO35, §2a.1) and software power-off
  (PWROFF_PLUSE pulse train, D12). The GPIO35 edge log wasn't saved, so it's still open whether
  U28 signals the P4 before a double-press cut. That decides whether D12's graceful double-press
  shutdown is possible. Re-check with BTN if Phase 7 needs it.

### RTC (done)

- The RX8130CE had lost its time (VLF set) on the first run. It is now set and advancing. It
  holds local time with no time zone.

### Board inventory (done)

- I2C devices: 0x10 ES8388, 0x28 **unidentified**, 0x32 RX8130CE, 0x40 ES7210, 0x41 INA226,
  0x43/0x44 PI4IOE5V6408, 0x55 ST7123 touch, 0x68 BMI270.
- Panel detection needs both the LCD and touch out of reset: on the ST7123 the touch controller
  lives inside the display chip. The BSP waits 500 ms for this, and our board init waits another
  500 ms. **Phase 4 action:** cache the variant, or reorder, to save ~0.5 s of boot.
- The first power-on after flashing once saw a NAK from the 0x43 expander reset. Board init now
  retries.

## Re-running

Every test has an on-screen button (OFF must be held for 2 s), and the serial console accepts
the same commands (`help`). Results are saved to `/retro/hwtest/results.md` at boot and after
every test; each boot keeps the previous file as `results-N.md`, and `cat <path>` prints one.
