# Tab5 Retro Console — Implementation Plan

Source spec: [tab5-retroconsole.md](tab5-retroconsole.md) (v1.0)
Status: Draft rev 4, 2026-09-23 (rev 2: hardware findings and owner answers, §2a, D2, D9, D11–D13; rev 3: Phase 2 findings, see the end of §6; rev 4: USB controllers deferred to v2, D14)
Repository: `tab5emu/` (this repo is the spec's `tab5-retro/`)

---

## 1. Goal and scope

Ship **Release 1**: the requirements in spec §58, less R11 and R12 (USB controllers, deferred to v2 by D14). That means NES + standard LoROM/HiROM SNES on bare ESP-IDF, with a launcher, microSD library, touch input, SRAM saves and save states, screenshots, battery display, brightness control, headphone detection, the C6 powered down, and a clean shutdown.

Everything in spec §57 ("deliberately excluded") stays out of v1. Where the spec wants future extensibility (4-player input ABI, logical volumes, network transport abstraction, peripheral registration), v1 **defines the interface and ships one implementation**. Speculative drivers don't get built.

Success is measured by the spec §59 targets: 100% speed, 0 audio underruns, <2 frames input latency, <5 s boot-to-library, <2 s game launch.

---

## 2. Key technical decisions

These fill gaps in the spec or adjust it. Each needs sign-off before Phase 2.

| # | Decision | Rationale |
|---|---|---|
| D1 | **Pin one ESP-IDF version** (latest stable v5.x that the `m5stack_tab5` BSP supports) and manage the BSP, `esp_codec_dev`, the USB Host Library and `esp_lvgl_port` through the IDF Component Manager (`idf_component.yml`) with exact versions. | The BSP handles panel/touch revision differences (ILI9881C / ST7123 / GT911, spec §3). Unpinned updates are the most likely cause of mystery regressions. |
| D2 | **Landscape rotation is mandatory, not optional.** The Tab5 panel's native scan orientation is portrait, 720×1280 (**confirmed**: M5Unified sets up the Tab5 display as 1280 px tall). Every game frame has to be rotated 90° on its way out. Rotation gets fused into the scaler step (CPU blit or PPA SRM, which rotates and scales in one pass). | The spec (§9) lists rotation as "optional". If it isn't planned for up front, it becomes a second full-frame pass later. |
| D3 | **Audio is the master clock.** The emulator task blocks only on the audio ring's high-water mark. Video uses a *latest-frame-wins mailbox* and never blocks the emulator. A small dynamic-rate-control resampler (±0.5%) absorbs the drift between the emulated rate (NES 60.0988 Hz) and the I2S clock. | Directly implements spec §11, §15 and §60. DRC avoids the slow buffer creep that otherwise ends in periodic underruns. |
| D4 | **Pixel Perfect = CPU nearest-neighbour 3× blit with rotation**, written as an optimized RISC-V loop (4-byte stores, row-duplication with `memcpy` or 2D-DMA). **Smooth / Fit / 4:3 / Stretch = PPA SRM.** In Phase 1, test whether PPA at an exact integer scale produces sharp output. If it does, Pixel Perfect moves to the PPA too. | The spec says the PPA scaler is bilinear (§9). We verify that on hardware before designing around it. |
| D5 | **Extend `retro_core_t`** (spec §55) with: `get_av_info` (fb width/height/pitch, fps, sample rate, region), `get_sram` / `sram_size`, `state_size`, `get_memory_requirements`, and a `core_id` + `state_version` pair. | The spec interface has no way for the frontend to learn frame geometry, timing or SRAM location. All of those are needed for §8, §14 and §15. |
| D6 | **Crash-safe writes everywhere**: write `file.tmp` → `fsync` → `rename` over the target. SRAM is flushed ~2 s after the last cartridge-RAM write (only if dirty, debounced; see D12), on menu open and on exit. | Battery removal is the realistic failure mode on an NP-F550 device. Spec §51 and §60 require that saves are never corrupted. |
| D7 | **Settings in a JSON file on SD** (`/retro/config/settings.json`), plus a **minimal NVS mirror** for boot-critical values (boot behaviour, brightness, last game). | Settings survive an SD swap on the user's side while the device still boots sensibly without SD. |
| D8 | **Host (desktop) build** of cores + frontend logic, with an SDL2 HAL backend. Built with plain CMake next to the IDF build. | Core bring-up, save-state format, library scanning and launcher logic can then be debugged and unit-tested without flashing. It also makes regression tests with test ROMs possible in CI. This does not replace on-device perf testing. |
| D9 | **Recovery mode lives inside the main app.** There is no separate factory partition. It has three triggers; see §2a.3 for how they were chosen. (1) **Touch-hold**: two fingers held on the screen while the boot splash is up (~1.5 s window). (2) **Boot-loop guard**: a counter in NVS is incremented at boot and cleared once the launcher is ready. After 3 failed boots in a row the device enters recovery automatically. (3) **SD trigger file**: `/retro/recovery`. **No Wi-Fi OTA in v1.** `app0`/`app1` are kept anyway, so recovery can do a rollback-safe **firmware update from SD** (`/retro/update.bin` → `esp_ota_*` → reboot, with IDF app rollback). | The Tab5 has no user button that the app can read at boot (§2a.3). Touch works on every panel revision through the BSP. ROM download mode stays independent of the app: hold the reset button for about 2 s, and the power-management MCU drives GPIO35 (spec §35). |
| D10 | **SNES core source**: evaluate the RetroESP32-P4 SNES core (Snes9x-derived) first. **Licensing note:** Snes9x's licence prohibits commercial use and Nofrendo is GPLv2, so the firmware as a whole inherits GPL + a non-commercial restriction. Record this in README/LICENSE. **Decided:** personal / open-source distribution, so the Snes9x-based core is acceptable. | Avoids a licensing surprise late in the project. |
| D11 | **Deferred to v2 (D14).** Kept as the v2 design. **Xbox controllers are the first physical controller; PlayStation support comes after.** Xbox pads are **not USB HID**: they use vendor-specific interfaces, so `usb_host_hid` can't drive them. v2 ships a custom `xinput_host` class driver built on the ESP-IDF USB Host Library client API. It supports **Xbox 360 XInput** (interface class `0xFF/0x5D/0x01`, 20-byte input report) and **Xbox One / Series GIP** (interface `0xFF/0x47/0xD0`, which needs the power-on packet `05 20 00 01 00` before any reports arrive). Devices are matched by interface class, not a VID/PID list, so licensed third-party pads and 8BitDo-in-X-input also work. Wired USB only. Generic HID pads and PlayStation come after it. No specific test pads are fixed yet, so both protocols get built; GIP (One/Series) is tested first because it's the more common current pad. | Answer to Q3. The protocol references are Linux `xpad.c` (protocol reference only, because it's GPL) and TinyUSB's `tusb_xinput` (MIT). Matching on interface class means we don't maintain a PID table. |
| D12 | **Shutdown design** (full detail in §2a.1). The power button is handled entirely by a small power-management MCU, and **double-press cuts power without telling the P4**. So: (a) **every save path must survive a hard power cut** (D6, plus SRAM flushed ~2 s after the last cartridge-RAM write instead of on a 30 s timer); (b) a **software "Power off"** in the launcher and the in-game menu (plus a hold-Guide-3 s hotkey once pads arrive in v2) runs the spec §51 sequence, then asks the MCU to cut power by toggling `PWROFF_PLUSE` (IO expander 0x44 pin 4) 10× at 50 ms, the same method M5Unified uses; (c) **low battery** (INA226 threshold with hysteresis) triggers the same graceful path. | Answer to Q1. With only the hardware button, a graceful shutdown is impossible, so correctness can't depend on one. |
| D13 | **Charging needs the device to be awake.** Per M5Stack's docs, the IP2326 charge IC only charges while the Tab5 is powered on and initialised, so "Power off" while USB-C power is connected would stop charging. **Decided:** when external power is present, "Power off" enters a **charge mode** instead of cutting power: display, emulator and C6 off, `CHG_EN` on, P4 in light sleep, battery % shown on a single tap. | Otherwise a user who powers off to charge gets a device that never charges. |
| D14 | **v1 input is touch only; USB controllers (D11, spec R11 and R12) move to v2.** The USB host is not started in v1 and USB-A 5 V is switched off at boot. The `xinput_host` driver written in Phase 2 stays in the tree behind `CONFIG_RETRO_USB_PADS` (default off); the input ABI already has four players, so v2 adds sources without changing cores. | Owner decision (2026-09-23): get touch done first. It removes the USB host daemon and client task from CPU1, and the USB-A rail from the power budget. |

---

## 2a. Hardware findings (from the Tab5 docs, schematic and reference code)

Sources: [Tab5 docs](https://docs.m5stack.com/en/core/Tab5), [Tab5 schematic PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf), [ESPHome Tab5 config](https://devices.esphome.io/devices/m5stack-tab5/), [M5Unified](https://github.com/m5stack/M5Unified) (`Power_Class.inl`), [esp-bsp `m5stack_tab5`](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_tab5).

### 2a.1 Power button and power-off

- The power button (S1 → net `SW_PWR`) goes to **U28, a PMS150G-U06**, a tiny custom-programmed MCU running on the always-on `VDD_STBY` rail. The P4 does not see the button directly.
- U28 drives `MPWR_EN` (the main power enable) and implements the documented behaviour: single press = power on, double press = power off. **Double press is a hard power cut. No documented interrupt reaches the P4 first**, and M5Unified has no power-button support for Tab5.
- U28 pin PA5 is also wired to **`BOOT_GPIO35`**, the P4's boot strapping pin, which is how "hold reset ~2 s → download mode" works. U28 pin PA6 (`nINT_STAT_TRIG`) is its "please power off" input.
- **Software power-off**: IO expander PI4IOE5V6408 @ 0x44, pin 4 = `PWROFF_PLUSE` → Q3A → `nINT_STAT_TRIG`. M5Unified toggles this pin 10 times at 50 ms intervals to make U28 drop `MPWR_EN`.
- **Phase 1 experiment**: with GPIO35 set as an input with an interrupt, check whether U28 sends any edge on a single or double press before it cuts power. If it does, a single press becomes "open menu / sleep" and a double press gets a graceful shutdown. If it doesn't, D12 stands as written.

### 2a.2 IO expander map (PI4IOE5V6408 ×2, I²C on G31/G32)

| Expander | Pin | Direction | Function |
|---|---|---|---|
| 0x43 | 0 | O | Wi-Fi antenna: L = internal, H = external (MMCX) |
| 0x43 | 1 | O | Speaker amp enable (`SPK_EN`) |
| 0x43 | 2 | O | External 5 V (M5-Bus) enable (`EXT5V_EN`) |
| 0x43 | 4 / 5 / 6 | O | LCD / touch / camera reset |
| 0x43 | 7 | I | **Headphone detect** (`HP_DET`) → R18 |
| 0x44 | 0 | O | **ESP32-C6 power** (`WLAN_PWR_EN`) → R19 |
| 0x44 | 3 | O | **USB-A 5 V** (`USB5V_EN`, the BSP's `BSP_USB_EN`) |
| 0x44 | 4 | O | `PWROFF_PLUSE` (software power-off, §2a.1) |
| 0x44 | 5 | O | Quick-charge enable (inverted) |
| 0x44 | 6 | I | Charging status |
| 0x44 | 7 | O | Charge enable (`CHG_EN`) |

The BSP owns these expanders. `retro_power` must go through the BSP's handle, not open a second I²C driver on the same bus.

### 2a.3 Buttons and recovery entry

Physical buttons: power (through U28), reset, and download mode (reset held ~2 s, through U28 → GPIO35). **No button is free for the app to read during boot.** GPIO35 is a strapping pin also driven by U28, and holding it at reset enters ROM download mode rather than our app. That is why D9 uses touch-hold, the boot-loop guard and an SD trigger file.

### 2a.4 USB routing

- **USB-A (host) → P4 USB 2.0 HS OTG** (UTMI PHY; schematic net `USB_HOST_D±` → `USB2_OTG_D±`). This is the controller that `bsp_usb_host_start()` installs by default. VBUS is switched by 0x44 pin 3. HS also means hubs will work later (spec §19).
- **USB-C → P4 full-speed PHY** (`USB_DEVICE_D±` → `USB1P1_0±`, the pins used by USB-Serial-JTAG). It stays the flashing, console and JTAG port (spec §21).

### 2a.5 Other findings

- **Board variants**: M5Unified tells apart `Tab5` and `Tab5X` (the Tab5X has an extra rail on 0x43 pin 3). Panel/touch revisions: ILI9881C + GT911 (original), ST7123 (Oct 2025), ST7121 (Apr 2026). All go through the BSP. The hwtest screen logs which variant was detected.
- **Battery**: 2S pack; INA226 bus voltage. Battery % from a 2S Li-ion discharge curve with current compensation.
- **Charging** only while powered on (D13).

---

## 3. Architecture

### 3.1 Components (maps to spec §54)

```text
tab5emu/
├── main/main.c                     boot sequence, task creation, mode dispatch
├── components/
│   ├── retro_hal/                  PUBLIC HAL headers + Tab5 implementation
│   │   ├── include/retro_*.h       video, audio, input, storage, time, power, network, log
│   │   ├── tab5/                   ESP-IDF/BSP backend
│   │   └── host/                   SDL2 backend (host build only)
│   ├── retro_common/               ring buffer, crc32/sha1, json config, file utils (atomic write)
│   ├── video_pipeline/             scaler (CPU NN + PPA), display modes, overlays, fb mailbox
│   ├── audio_engine/               ring buffer, DRC resampler, I2S feeder task, mixer hooks
│   ├── controller_manager/         input sources → 4 logical players, mapping DB, hot-plug
│   ├── touch_overlay/              virtual gamepad layout, hit-testing, rendering
│   ├── rom_library/                scanner, index file, checksums, favourites/recents/playtime
│   ├── save_manager/               SRAM persistence, save-state container format, thumbnails
│   ├── emulator_manager/           core registry, lifecycle, frame loop, in-game menu, suspend
│   ├── launcher/                   LVGL UI: library, settings, controller config, recovery
│   ├── power_manager/              INA226, battery estimate, rails via IO expander, profiles, shutdown
│   ├── core_nes/                   Nofrendo + retro_core_t adapter
│   └── core_snes/                  SNES core + retro_core_t adapter
├── host/                           desktop CMake project (SDL2) for cores + frontend logic
├── test/                           unit tests (Unity on-target, plain C on host), test-ROM runners
├── tools/                          partition/asset scripts, perf log parser
├── partitions.csv
├── sdkconfig.defaults
├── idf_component.yml
└── CMakeLists.txt
```

Dependency rule, enforced through CMake `REQUIRES`: `core_*` depends **only** on `retro_hal` public headers and `retro_common`. Nothing in `core_*` may include BSP or IDF driver headers. That is the §4 and §63 principle, checked mechanically.

### 3.2 Task and CPU topology (spec §6)

| Task | Core | Priority | Notes |
|---|---|---|---|
| `emu` | CPU0 | high (e.g. 20) | Loop per frame: poll input → `run_frame` → push audio (may block at the ring high-water mark) → publish fb to the mailbox. Pinned. Nothing else runs on CPU0 during gameplay. |
| `audio_out` | CPU1 | highest on CPU1 (22) | Pulls from the ring, resamples (DRC), `i2s_channel_write`. On underrun it writes silence and counts the event. |
| `video_out` | CPU1 | 18 | Takes the latest fb from the mailbox → scale + rotate into the back display buffer → flip on vsync. Drops frames when it's behind. |
| `input` | CPU1 | 18 | Touch, read on the controller's interrupt (Phase 2 finding: polling at 250 Hz would saturate the I2C bus) → controller manager → `retro_input_state_t` snapshot. |
| `usb_host` | CPU1 | 15 | v2 only (D14): IDF USB host library daemon + `xinput_host` client. |
| `storage_io` | CPU1 | 8 | Deferred SRAM flushes, screenshot encode + write, index updates. |
| `lvgl` | CPU1 | 5 | Launcher only. Suspended (not deleted) during gameplay. |
| `power` | CPU1 | 4 | INA226 sampling at 1 Hz, rail control, shutdown handling. |

All task affinities and priorities are Kconfig options (spec §6: "should remain adjustable").

### 3.3 Memory placement (spec §7)

| Region | Location | Size (approx.) |
|---|---|---|
| Emulator hot state (CPU regs, NES RAM 2 KB, SNES WRAM 128 KB / VRAM 64 KB / APU RAM 64 KB if they fit) | Internal SRAM | ≤ ~280 KB |
| Native fb ×2 (256×240 RGB565 = 120 KB each) | Internal SRAM if the budget allows, otherwise PSRAM | 240 KB |
| Audio ring (~80 ms stereo s16 @ 48 kHz) | Internal SRAM, DMA-capable | ~16 KB |
| DMA descriptors, input state, LUTs | Internal SRAM | small |
| Display fb ×2 (1280×720 RGB565) | PSRAM, 64-byte aligned | 3.6 MB |
| ROM image (≤ 6 MB SNES) | PSRAM | ≤ 6 MB |
| Save-state scratch + suspended game | PSRAM | ≤ ~1 MB |
| LVGL draw buffers, artwork cache | PSRAM | ~4 MB cap |

`emulator_manager` asks each core for its memory requirements (D5) and allocates from tagged pools with `heap_caps_malloc(MALLOC_CAP_INTERNAL | …)` and a PSRAM fallback. Any fallback gets logged, so a core that loses its SRAM placement is noticed.

**PSRAM bandwidth budget.** DSI scan-out reads about 110 MB/s (1280×720×2×60). The scaler writes about 66 MB/s (only the 768×720 game region; borders are cleared once when the mode changes). That is well inside Octal PSRAM capacity, but it has to be measured in Phase 1, because cache thrashing against a ROM held in PSRAM is the real SNES risk.

### 3.4 Frame and timing flow

```text
emu (CPU0)                         audio_out (CPU1)            video_out (CPU1)
─────────                          ────────────────            ────────────────
input_snapshot()
core.run_frame()  ── writes ──►  native fb[n]
core audio  ───── push ──────►  ring ──► DRC ──► I2S DMA ──► ES8388
  (block only if ring > HWM)
mailbox_publish(fb[n]) ─────────────────────────────────────► take latest, scale+rotate,
n ^= 1                                                          flip @ vsync (drop if busy)
```

DRC adjusts the resample ratio by ±0.5% max, proportional to ring fill minus target (50%).

---

## 4. Phased plan

Phases follow spec §61 milestones A–G. They are reordered where a v1 requirement depends on a later milestone: basic scaling, from Milestone F, is needed in B. Each phase lists deliverables and **exit criteria**. `R#` refers to the spec §58 requirement numbers.

### Phase 0: Project foundation (≈1 week)

- Create the IDF project skeleton per §3.1, pin versions (D1), add `sdkconfig.defaults`: 360 MHz, PSRAM 200 MHz octal, caches, FreeRTOS 1 kHz tick, `CONFIG_SPIRAM_USE_MALLOC`, watchdog config that tolerates the emu task.
- `partitions.csv` (16 MB): `nvs` 24K, `otadata` 8K, `phy_init` 4K, `app0` 6M, `app1` 6M, `coredump` 64K, rest reserved. ROMs and assets never go in flash (§34).
- Host build scaffold (D8): SDL2 window, audio and keyboard as `retro_hal/host`.
- CI (GitHub Actions): `idf.py build` inside the pinned `espressif/idf` container, plus a host build and unit tests.
- `retro_log` with the §46 categories: USB-serial sink + in-RAM ring buffer (the SD sink comes later).

**Exit:** firmware builds and boots to a log line on the device; the host build opens a window; CI is green.

### Phase 1: Hardware bring-up (spec Milestone A, ≈2 weeks)

A `hwtest` mode in main (it later becomes Recovery → "Test hardware") that exercises each subsystem through the BSP:

1. **Display**: BSP init, draw test pattern, confirm native orientation (D2), measure the real refresh rate, confirm vsync/"trans done" callback and double-fb support (`num_fbs = 2`). Backlight PWM → brightness (R17).
2. **PPA**: time a 256×240 → 768×720 SRM with 90° rotation; check integer-scale sharpness (D4). Time the CPU NN blit for comparison. Record PSRAM bandwidth.
3. **SD**: SDMMC 4-bit mount through the BSP; measure sequential read (ROM load time), write + fsync latency; FATFS long filenames on.
4. **Audio**: ES8388 via `esp_codec_dev` → I2S at 48 kHz stereo; sine test; measure DMA buffer latency; speaker amp enable and headphone-detect GPIO via the IO expander (R18).
5. **USB / Xbox pad**: USB-A host on the HS controller (§2a.4), VBUS via 0x44 pin 3. Enumerate an Xbox 360 pad and an Xbox One/Series pad, dump their descriptors, send the GIP power-on packet, dump input reports. Hot-plug connect/disconnect. Measure how much current the pad draws with rumble on.
6. **Touch**: GT911 multitouch, 5 points, report rate.
7. **Power**: INA226 readings; IO-expander rail control for C6, USB-A 5 V, EXT 5 V and the speaker amp (§2a.2); confirm the C6 can be held off (R19). Software power-off via the `PWROFF_PLUSE` pulse train. Run the GPIO35 power-button experiment (§2a.1). Check whether charging continues in light sleep (D13).
8. **RTC**: RX8130CE read/set → `retro_time`.

**Exit:** every subsystem works in hwtest; a measured numbers table (DSI Hz, PPA ms, blit ms, SD MB/s, audio latency, touch Hz) is committed to `plan/bringup-results.md`.

### Phase 2: RetroHAL and runtime pipelines (≈2–3 weeks)

- Freeze the public HAL headers (`retro_video.h`, `retro_audio.h`, `retro_input.h`, `retro_storage.h`, `retro_time.h`, `retro_power.h`, `retro_network.h` as a stub, `retro_log.h`).
- **Input ABI**: `retro_pad_state_t` per spec §17 (as a bitfield plus the named-bool accessor), `retro_input_state_t { pads[4]; axes[4][4]; hotkeys; }`. Four players from day one (§44). Generic axes are there for the IMU later (§23).
- **video_pipeline**: fb mailbox, CPU NN 3× rotate-blit (optimized), PPA path, display modes Pixel Perfect / Original / 4:3 / Fit / Stretch (§10). Scanline overlay is a cheap alternate-row darken applied in the blit. Border clear only on a mode change.
- **audio_engine**: SPSC lock-free ring, DRC resampler (linear or cubic from the core's native rate to 48 kHz), underrun counter, `retro_audio_*` API. The codec stays shareable: no exclusive claim (§16).
- **controller_manager**: source registry (`TouchInput`; `XInputSource` built but off in v1, D14; later `USBHIDInput`, `PSInput`, `KeyboardInput`), player assignment in connect order (§43), and a remap table stored per VID:PID in `/retro/config/controllers.json`.
  - **`xinput_host` driver** (D11; built, disabled in v1 by D14): a USB Host Library client. It handles NEW_DEV/DEV_GONE events and claims the vendor interface. It parses XInput (360) and GIP (One/Series) reports, including the GIP Guide-button report (`0x07`), acks any GIP message that asks for one, and sets the player LED on 360 pads. Rumble goes out as a stub (the haptic hook, spec §22).
  - **Default mapping is by position, not label** (for pads, so v2; the touch zones map straight to Nintendo buttons): Xbox B→Nintendo A, A→B, Y→X, X→Y; LB/RB→L/R; View→Select; Menu→Start; left stick → D-pad (toggle in settings). **Guide button → in-game menu.**
- **storage**: logical volume table (`/storage/sd` in v1; §20), `retro_storage_*` wrappers, atomic write helper (D6).
- **Performance overlay** (§45): compiled in behind `CONFIG_RETRO_DEVMODE`. Shows FPS (emulated and rendered), frame time, per-core CPU %, heap by caps, audio fill and underruns, PPA/blit time, battery V/W.

**Exit:** a synthetic "core" (moving test pattern + tone) runs at 60 fps with 0 underruns for 30 min on device, with touch driving it (USB pads dropped from this criterion by D14).

**Status:** implemented; measurements in [phase2-results.md](phase2-results.md). Additions to the list above: `retro_os.h` (tasks, locks, memory placement, CPU load) so the pipelines run unchanged on the host, and `retro_core.h` (the D5 core interface) as part of the frozen public ABI. The 60 Hz retime and the interrupt-driven touch read from the Phase 1 results are done.

### Phase 3: NES core (spec Milestone B, ≈2 weeks), R4, R6–R10

- Import Nofrendo (referencing the existing ESP32-P4 port), strip its platform layer, write the `retro_core_t` adapter. Output RGB565 native fb; APU at its native rate → audio_engine.
- Place hot state and the palette LUT in internal SRAM (IRAM/DRAM attributes where they help).
- **Touch overlay** (§22): default NES layout, multitouch hit-testing, drawn into the game-region-adjacent border (the 256 px side bars are exactly where the controls go, so no blending over gameplay). (Hiding it while a USB pad is active comes with pads in v2.)
- Temporary hard-coded ROM path boot for testing.
- Correctness: run `nestest`, blargg CPU/PPU/APU tests on the host build; record results.

**Exit:** at least 10 common titles (SMB, Zelda, Metroid, MM2, SMB3, Kirby, Contra, Castlevania, Punch-Out, Tetris) at 100% speed, 0 underruns in 20-minute sessions, touch input latency measured < 2 frames (LED/photodiode or high-speed video).

### Phase 4: Launcher and library (spec Milestone C, ≈2–3 weeks), R1–R3, R16, R17

- **rom_library**: scan `/retro/roms/{nes,snes}` for `.nes`, `.sfc`, `.smc` (strip the 512-byte copier header when detecting/checksumming `.smc`). Binary index at `/retro/metadata/library.idx`, rescanned only when a directory mtime changes or the user asks (§49). CRC32 is computed lazily in `storage_io` and cached in the index. SHA-1 is optional, for metadata.
- Auto-create the `/retro` directory tree on first boot (§13).
- **launcher** (LVGL via `esp_lvgl_port`): system tabs, list + grid view, favourites, recently played, last played, A–Z sort, search (on-screen keyboard), play-time tracking (§12). Battery %, Wi-Fi state and a clock in the status bar.
- **Settings**: display mode, scanlines, brightness, volume, audio output, touch overlay size/opacity/positions per system, boot behaviour (§50), power profile, dev mode.
- **LVGL ↔ game handoff**: suspend the LVGL task and release the display fb before the game starts; restore on exit without rebuilding the screens (§47).
- Fast boot path: display init → SD mount → load index → launcher. Measure it.

**Exit:** cold boot to a usable library in < 5 s with 500 ROMs; game launch < 2 s.

### Phase 5: Persistence (spec Milestone D, ≈2 weeks), R13–R15

- **SRAM**: detect the battery-save flag per core, write to `/retro/saves/<sys>/<rom>.srm` using the D6 policy.
- **Save states** (§14): container = header `{magic "T5RS", fmt_version, core_id, core_state_version, rom_crc32, rtc_timestamp, thumb_w, thumb_h, thumb_fmt}` + thumbnail (160×120 JPEG via the HW encoder, or raw RGB565 fallback) + core blob + trailing CRC32. Quick slot + slots 1–5. Loading rejects a mismatch in core ID, state version or ROM CRC with a clear message.
- **In-game menu** (§47): opened by the touch "menu" button in the overlay, or SELECT+START held 1 s (the Xbox Guide button joins in v2). Emulation is paused, the core stays resident, and the menu draws through the video pipeline overlay (not a full LVGL rebuild).
- **Screenshots** (§31): SELECT+R → copy the native fb → HW JPEG encode in `storage_io` → `/retro/screenshots/<rom>_<rtc>.jpg`. It must not stall `emu`.
- Recents / last played / play-time persisted through the library index.

**Exit:** pulling the battery mid-game at random points (50 trials) never corrupts an `.srm` or a state; states round-trip on host and on device.

### Phase 6: SNES core (spec Milestone E, ≈4–6 weeks; highest risk), R5

- Integrate the chosen core (D10) behind `retro_core_t`. LoROM/HiROM detection from the internal header; 50 Hz PAL support.
- **Hi-res modes**: SNES modes 5/6 and pseudo-hi-res output 512 px width, and interlace outputs 448 lines. The pipeline scales 512×224 → 1024×672 at 2×H/3×V; the spec only covers 256×224, so this is an addition. 239-line overscan mode.
- **Enhancement chips** (§5): detect DSP-1/CX4/SA-1/SuperFX/S-DD1 from the header and show "unsupported chip" in the launcher and on launch, with no crash.
- Profile with the perf overlay; move WRAM/VRAM/APU RAM and dispatch tables into SRAM; consider running the SPC700/DSP on CPU1 if the core allows it (§6 "adjustable").
- Audio: SNES DSP at 32 kHz → DRC → 48 kHz.

**Exit:** a curated list of at least 15 LoROM/HiROM titles (e.g. SMW, ALttP, Super Metroid, DKC, F-Zero, Super Punch-Out, Mega Man X, Chrono Trigger, FF6, Street Fighter II Turbo, Contra III, Kirby's Dream Course, Secret of Mana, EarthBound, Super Castlevania IV) at 100% speed with 0 underruns in 20-minute sessions. Publish a compatibility table.

### Phase 7: Optimisation and power (spec Milestone F, ≈2 weeks), R18–R20

- Finalise the blit/PPA choice per mode from Phase 1/6 data; 2D-DMA for row duplication if it measures faster.
- **Power gating** (§27): USB-A 5 V off (always in v1, D14); the C6 off whenever Wi-Fi is disabled (the default in v1); EXT 5 V off; speaker amp off when headphones are detected (R18, R19).
- Performance / Balanced / Battery Saver profiles (§26). Emulator clocks are never scaled during gameplay.
- **Graceful shutdown** (§51): the exact ordered sequence from the spec, wired to the software "Power off" (launcher, in-game menu) and to low battery (INA226 threshold with hysteresis). It ends with the `PWROFF_PLUSE` train, or with charge mode when external power is present (D13). Target is < 2 s from request to power-off (R20). Hard-cut safety (D12): SRAM is flushed ~2 s after the last cartridge-RAM write, and nothing else writes to SD during gameplay.
- Boot behaviour: Launcher / Last game / Last game + autosave state (§50). Optional suspend-in-PSRAM on return to launcher (§48).
- Battery runtime benchmark (NES and SNES loops at 50% brightness), results recorded.

**Exit:** all 20 R-requirements are demonstrable on device; the battery runtime has been measured.

### Phase 8: v1 hardening and release (≈1–2 weeks)

- Recovery mode (D9, §35): boot firmware, reset settings, hardware test (Phase 1 hwtest), mount SD, view the in-RAM/SD log, firmware update from `/retro/update.bin` on SD.
- **No Wi-Fi OTA in v1** (owner decision). Firmware updates are USB flashing or `/retro/update.bin` from recovery (D9). Image signature verification for the SD path is nice to have.
- Log levels set for production; dev overlay compiled out by default (§45).
- 8-hour soak test: attract-mode loop across NES/SNES titles, launcher cycling, shutdown/boot cycling.
- README: SD layout, touch controls, compatibility lists, licensing (D10), flashing instructions.

**Exit:** the §58 checklist is signed off against the traceability matrix (§5 below); the release is tagged.

### Phase 9: Expansion APIs, post-v1 (spec Milestone G)

- `retro_peripheral_register()` with capability flags (§37), separate GPIO/UART/I²C extension interfaces (§38).
- Tab5 Keyboard input source over I²C (GPIO0/1/50) (§36); IMU axes source (§23).
- USB MSC volumes (§20), multi-pad through a hub (§19), `NetworkManager` with a Wi-Fi transport (§28/§42), metadata fetch (§33).
- **v2 input**: wired Xbox 360 / One / Series pads (D11; the Phase 2 `xinput_host` driver, enabled with `CONFIG_RETRO_USB_PADS`, plus hot-plug mid-game, per-VID:PID remaps in the settings and the Guide hotkeys), PlayStation controllers (DS4/DualSense are HID: `usb_host_hid` + report parser), generic HID gamepads + the first-connect mapping wizard (§18).
- Wi-Fi OTA with signed images (§34/§52).
- Phase 2+ cores (§56), which should only need a new `core_*` component.

---

## 5. v1 requirement traceability

| R# | Requirement | Phase | Verified by |
|---|---|---|---|
| 1 | Boot directly into launcher | 4 | boot timing test |
| 2 | Mount microSD | 1, 4 | hwtest + launcher |
| 3 | Discover .nes/.sfc/.smc | 4 | library scan test (host + device) |
| 4 | Launch NES games | 3 | title list |
| 5 | Launch standard SNES games | 6 | title list |
| 6 | Correct game speed | 3, 6 | perf overlay, 20-min runs |
| 7 | Synchronized audio | 2, 3, 6 | underrun counter = 0 |
| 8 | Render via MIPI-DSI | 1, 2 | — |
| 9 | Pixel-perfect scaling | 2 | visual check, screenshot diff |
| 10 | Touchscreen | 3 | multitouch in-game |
| 11 | USB controller (**Xbox**, D11; amended from "HID") | **v2 (D14)** | Xbox 360 + Xbox Series pads, wired |
| 12 | Hot-plug controllers | **v2 (D14)** | plug/unplug mid-game |
| 13 | Save cartridge SRAM | 5 | battery-pull test |
| 14 | Save/load states | 5 | round-trip test |
| 15 | Screenshots | 5 | no emu stall (perf overlay) |
| 16 | Battery state | 4, 7 | INA226 vs multimeter |
| 17 | Display brightness | 1, 4 | settings |
| 18 | Headphone detection | 1, 7 | plug/unplug switches amp |
| 19 | Power down wireless | 1, 7 | current draw delta |
| 20 | Shutdown without corruption | 5, 7 | battery-pull, hardware double-press and software power-off tests |

---

## 6. Risks and open questions

**Risks**

| Risk | Impact | Mitigation |
|---|---|---|
| SNES core can't hold 100% on the heavier titles | R5/R6 | Pick the core by measuring early (spike in Phase 1–2, in parallel); SRAM placement; split the APU onto CPU1; per-title compatibility list instead of a blanket claim. |
| PSRAM contention (ROM fetches + scan-out + scaler) | Frame drops / emu slowdown | Measure in Phase 1; keep the native fb in SRAM; write only the game region; PPA offloads CPU cache pressure. |
| Mandatory 90° rotation slows the CPU blit (column-order writes are cache-unfriendly) | Pixel Perfect cost | Tile-based rotate-blit (e.g. 8×8 source tiles), or PPA rotation if integer scaling is sharp. |
| BSP API churn between versions | Build breaks | Pinned versions (D1); wrap all BSP calls in `retro_hal/tab5`. |
| Hardware double-press cuts power with no warning (§2a.1) | R20 | D12: atomic writes, SRAM flushed ~2 s after the last write, software power-off path. The worst case loses the last ~2 s of in-game saving. |
| Xbox GIP quirks (firmware-dependent init, Series controllers needing extra handshakes) | R11 (v2) | Test several controller firmwares when pads return in v2; follow the `xpad.c` init sequences. |
| Licensing (GPL + Snes9x non-commercial) | Distribution limits | Documented up front (D10). |

**Resolved (rev 2)**

- Power button / shutdown → §2a.1, D12, D13.
- Recovery entry → §2a.3, D9.
- USB-A routing → HS OTG (§2a.4); controller → Xbox (D11); PlayStation deferred. USB controllers as a whole → v2 (D14).
- OTA → not in v1 (SD update through recovery instead).
- Generic HID pads → v2. Charge mode → yes (D13). Licensing → personal/open source (D10).

**Still open** (Phase 1 measurements: [bringup-results.md](bringup-results.md))

1. ~~Target display refresh~~ **Resolved in Phase 2:** the ST7123 measured 57.5 Hz with the BSP's timings; shortening the vertical front porch (220 → 165 lines) gives **59.96 Hz measured** and the panel accepts it. The ILI9881C (48.2 Hz computed) needs a faster pixel clock instead and keeps the BSP timing for now; no unit to test on.
2. Default audio sample rate: 48 kHz (proposed) or 44.1 kHz.
3. GPIO35 power-button experiment (§2a.1): the button and software power-off were confirmed on the device, but the GPIO35 edge log wasn't saved, so it's still unknown whether U28 warns the P4 before a double-press cut.

**Phase 1 findings that change decisions**

- D4: the PPA's 3× scale is bilinear (block centres exact, the rest blended), so Pixel Perfect stays on the CPU blit (10.9 ms/frame).
- D6: FAT `rename()` fails when the target exists, so the atomic write is `unlink` + `rename`, and a leftover `.tmp` must be promoted on recovery.
- D13: the IO-expander driver resets CHG_EN to low; `retro_tab5_board_init()` re-enables charging.

**Phase 2 findings that change decisions** ([phase2-results.md](phase2-results.md))

- D4: the CPU blit straight into the PSRAM frame buffer took ~15.5 ms per frame on the device with the emulator running (bound by cached PSRAM writes, which fill each line before writing it), too slow for 60 fps. Pixel Perfect now builds 24 native rows at a time in SRAM and a DMA engine copies each strip into the frame buffer while the CPU builds the next: GDMA memcpy for full-width strips (~10–12 ms per frame, 60 fps), the PPA at scale 1 for other widths. It stays nearest-neighbour. This was planned for Phase 7.
- D6: an atomic write is `write .tmp → fsync → rename to .new → unlink target → rename .new`. A leftover `.new` is always complete and gets promoted; a leftover `.tmp` is always partial and gets deleted. That's stricter than "promote a leftover .tmp", which could promote a half-written first save.
- D3: the DRC only slows playback when the emulator has stopped blocking on the ring. While it blocks, a low fill just reflects the time spent making a frame. Without this the DRC sat at about −1000 ppm.
- §3.3: only ~355 KB of SRAM is free at boot with the 256 KB L2 cache, in several regions. Three 120 KB native frame buffers (the latest-frame-wins mailbox needs three) don't fit next to the blit strips, so they sit in PSRAM for now. Revisit the L2 cache size, or a paletted native format, before the SNES core needs SRAM (Phase 6).
- The Tab5's audio output latency is ~36 ms (20 ms DMA + ~16 ms average ring), within the spec §15 target of < 40 ms. The host build needs a 40 ms ring under CoreAudio.

---

## 7. Test and measurement strategy

- **Host unit tests**: ring buffer, DRC resampler, CRC/SHA, library index, save-state container, mapping DB, config parser, XInput/GIP report parsers (fed with captured reports from Phase 1).
- **Core correctness**: NES test ROM suites (nestest, blargg) and SNES test ROMs (e.g. PeterLemon/krom suites) run headless on the host build, compared against golden frame hashes.
- **Replay-driven performance tests**: record input for a title's first N minutes, replay it on device with the dev overlay logging to serial, parse with `tools/` → FPS, frame-time p99, underruns. Rerun on every perf-sensitive change.
- **On-device soak and fault injection**: battery pull, SD removal mid-write, 8-hour soak (Phase 8).
- **Latency**: measure input-to-photon with a high-speed camera or LED harness once in Phase 3 and again before release.

---

## 8. Rough schedule

| Phase | Duration | Cumulative |
|---|---|---|
| 0 Foundation | 1 wk | 1 |
| 1 Bring-up | 2 wk | 3 |
| 2 HAL + pipelines | 2–3 wk | 6 |
| 3 NES | 2 wk | 8 |
| 4 Launcher | 2–3 wk | 11 |
| 5 Persistence | 2 wk | 13 |
| 6 SNES | 4–6 wk | 19 |
| 7 Optimisation/power | 2 wk | 21 |
| 8 Hardening | 1–2 wk | 23 |

These estimates assume a single developer. Phases 4 and 5 can overlap with Phase 6 if a second person takes the SNES work. A timeboxed SNES core spike during Phases 1–2 is strongly recommended, because it is the largest schedule risk.
