# Tab5 Phase 2 results: RetroHAL and runtime pipelines

Measured with the pipeline test mode (synthetic core, see README) on 2026-09-23, same unit as
Phase 1: Tab5, ESP32-P4 rev v1.3, ST7123 panel, on USB-C power. Numbers come from the 10-second
summary line the firmware logs over serial.

Status legend: **done** = measured on the device; **host** = verified in the host build and unit
tests only; **needs a person** = needs someone at the device (touching the screen, plugging in a
pad).

## Exit criterion

> A synthetic "core" (moving test pattern + tone) runs at 60 fps with 0 underruns for 30 min on
> device, with touch driving it.

(Originally "with USB pad and touch both driving it"; USB controllers moved to v2 by plan D14.)

| Part | Result |
|---|---|
| 60 fps, 30 min | **Complete (owner's call)**: stopped after 7.4 min of logging (44 samples) at the owner's request; every sample showed output at the panel rate (59.9 fps) with only the expected drops |
| 0 underruns, 30 min | **Complete (owner's call)**: 0 underruns in all 44 samples; DRC stayed at 0 ppm |
| USB pad driving it | **Deferred to v2** (plan D14). The driver is built and unit tested but off (`CONFIG_RETRO_USB_PADS`); USB-A 5 V is off at boot |
| Touch driving it | **needs a person**: interrupt-driven read in place; not yet touched on this firmware |

## Measurements

| Measurement | Result | Notes |
|---|---|---|
| Display refresh | **59.96 Hz** measured (57.50 before) | Vertical front porch 220 → 165 lines; panel accepts it |
| Emulated rate | 59.9–60.1 fps | Paced by audio (plan D3): 60.0988 Hz × the I2S clock's +525 ppm |
| Frames shown | = panel rate | Drops only from 60.1 Hz content on a 60.0 Hz panel: ~1 per 7 s, as expected |
| Pixel Perfect 3x, CPU into PSRAM | 15.5 ms/frame, 54–56 fps | CPU1 ~90 %; bound by cached PSRAM writes |
| Pixel Perfect 3x, CPU strips + PPA copy | 13–14.7 ms, 52–54 fps | 32 PPA transactions per frame, ~0.2 ms overhead each |
| **Pixel Perfect 3x, CPU strips + GDMA memcpy** | **9.4–10.8 ms** (overlay off), **11–12 ms** (overlay on) | 60 fps; CPU1 57–68 % in total. Now the default |
| Pixel Perfect + scanlines | 9.7–10.8 ms | Same path |
| Pixel Perfect, SNES geometry (256×224 → 768×672 at y = 24) | 10.1–11.5 ms | Same path: full-width strips with black border columns |
| Fit (PPA, bilinear) | 6.6–7.2 ms | CPU1 ~5 % |
| Stretch (PPA) | 8.7–10.7 ms | |
| Audio output | 48 kHz, DMA 4 × 240 frames (20 ms), ring high-water mark 24 ms | ~36 ms total latency (spec §15: < 40 ms) |
| Audio underruns | 0 | |
| DRC | 0 ppm while the emulator keeps up | Was ~−1000 ppm before the "producer blocked recently" rule |
| Synthetic core | 3–5 ms/frame on CPU0 (CPU0 ~17–20 %) | Not representative of a real core |
| SRAM | 355 KB free at boot, 212 KB with everything running | Native frame buffers ended up in PSRAM (below) |

## Findings

### Display (done)

- The 60 Hz retime works: 1455 total lines instead of 1510 at the BSP's 70 MHz pixel clock and
  802-pixel lines. It's done by rewriting the DSI host and bridge vertical timing registers right
  after the BSP creates the panel (`CONFIG_RETRO_TAB5_DISPLAY_60HZ`), and measured at boot.
- The ILI9881C panel (48.2 Hz with BSP timings) can't reach 60 Hz by shortening the porch; it keeps
  the BSP timing and logs a warning. No unit to test on.
- Flips: after `present`, the old buffer is free once the next frame-done interrupt has run (the
  DPI driver restarts its DMA there with the latest buffer). A present that lands during that
  interrupt could be missed by it, so presents wait out ±0.2 ms around the expected boundary.

### Pixel Perfect blit (done): decision D4 revisited

- With a PSRAM source, the emulator writing PSRAM on CPU0, and scan-out reading PSRAM, the Phase 1
  CPU blit (10.9 ms on an idle system) took ~15.5 ms, too slow for 60 fps once the overlay and the
  other CPU1 tasks are added.
- The CPU writes are what's slow: each PSRAM line is read into the cache before it's written.
  The blit now builds 24 native rows (8 source columns × 3) at a time in an SRAM strip, and a DMA
  engine copies each finished strip to the frame buffer while the CPU builds the next (two strips,
  2 × 34.5 KB SRAM). Strips span the full panel width with black border columns, so every copy
  is contiguous: GDMA memcpy (AXI, 64-byte bursts). The PPA copy at scale 1 remains as a fallback.
  Output is still exact nearest-neighbour.

### Memory (done)

- With the 256 KB L2 cache, SRAM has ~355 KB free at boot, split across regions (the largest is
  256 KB). The two blit strips go in first, and after them no 123 KB block is left, so all three
  native frame buffers of the latest-frame-wins mailbox are in PSRAM. With the DMA strips the
  PSRAM source costs little. Revisit before the SNES core needs SRAM (Phase 6): a 128 KB L2 cache,
  or an 8-bit paletted native format for NES.

### Audio (done)

- The audio engine owns the I2S channel now (the BSP hard-codes a 6 × 240 DMA queue); 4 × 240
  frames at 48 kHz.
- DRC keeps the ring near its target when the producer can't keep up. When the producer blocks on
  the ring (it's keeping up), a low fill reading only reflects the time it takes to make a frame,
  so the DRC doesn't slow playback then.
- Host build: under CoreAudio a 24 ms ring underruns (the OS pulls in larger bursts); 40 ms doesn't.
  The host default is 40 ms.

### Touch (needs a person)

- ST712x boards: the controller is read directly at 400 kHz (falls back to 100 kHz after read
  errors), and the reports are skipped when the status register has no coordinates, so an idle
  poll is one 1-byte read. Reads are triggered by the GPIO23 interrupt, with polling every 10 ms
  until the first interrupt is seen, and a 30 ms safety poll while fingers are down.
- Not yet verified: whether the ST7123 raises the interrupt per report, and the read time with
  fingers down at 400 kHz. Check with the overlay while touching the D-pad zone.

### Storage (host)

- The card mounts at its logical path, `/storage/sd` (spec §20).
- Crash-safe writes follow D6 as revised: `.tmp` → fsync → rename to `.new` → unlink → rename. All
  recovery cases are covered by host tests; on-device battery-pull testing is Phase 5.

### Input mapping (host; USB parts are v2)

- Default mapping is by position (Xbox B → A, A → B, Y → X, X → Y), with the left stick also
  driving the D-pad (hysteresis 50 % on / 37 % off). Guide → MENU; Guide held 3 s → POWER;
  SELECT+START held 1 s → MENU. Per-VID:PID remaps load from `/retro/config/controllers.json`.
- XInput and GIP parsers are unit tested with synthetic reports built from the protocol layout;
  no captured reports from real pads yet (Phase 1 didn't save any). `usb` in hwtest writes
  them to `/retro/hwtest/usb_reports.txt`. All of this waits for v2 (plan D14).
- v1 input is touch only: the touch zones map to the same positional controls, so MENU comes from
  the top-right corner zone (held 3 s: POWER) or SELECT+START held 1 s.

## Still to do for the exit criterion

1. Touch the D-pad and button zones and check the button indicators and the log: a
   `touch: … controls` line per change, `touch interrupt seen` once, the `touch` read count
   rising, and still no xruns or drops beyond the expected.

For v2: capture real Xbox 360 and One/Series reports (hwtest `usb`) for the parser tests.
