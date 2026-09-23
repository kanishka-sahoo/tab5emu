# Tab5 Retro Console

NES and SNES emulation firmware for the M5Stack Tab5 (ESP32-P4), built on bare ESP-IDF.

- Product spec: [plan/tab5-retroconsole.md](plan/tab5-retroconsole.md)
- Implementation plan: [plan/implementation-plan.md](plan/implementation-plan.md)

Status: **Phase 2 (RetroHAL and runtime pipelines)**. The firmware boots into the pipeline test:
a synthetic core running on the full frontend (audio engine, video pipeline, controller manager),
see below. The Phase 1 hardware test is still available (`CONFIG_RETRO_HWTEST_BOOT`).
Measurements are in [plan/bringup-results.md](plan/bringup-results.md) (Phase 1) and
[plan/phase2-results.md](plan/phase2-results.md) (Phase 2). The host build runs the same frontend
in a window.

## Layout

```text
main/                  firmware entry point, pipeline test mode
components/
  retro_common/        platform-neutral helpers (no IDF, no SDL): blits, landscape drawing, SPSC
                       ring, CRC-32, JSON, battery curve, log ring
  retro_hal/           RetroHAL public headers + backends
    include/           frozen headers: video, audio, input, storage, time, power, network (stub),
                       log, platform, os (tasks/locks/memory), core (retro_core_t)
    src/               shared code: logger, logical volumes and crash-safe files
    tab5/              ESP-IDF / BSP backend (the only code allowed to use the BSP)
      include/         retro_tab5.h: Tab5 board layer (display, touch, audio, SD, USB, rails, INA226)
    host/              SDL2 / pthreads backend for the desktop build
  video_pipeline/      frame mailbox, display modes, scalers (CPU, DMA strips, PPA), video_out task
  audio_engine/        SPSC ring, DRC resampler, audio_out task
  controller_manager/  input sources -> 4 players, mapping DB, touch zones; Xbox pad driver
                       (built but off: USB controllers are v2, plan D14)
  emulator_manager/    frontend bring-up, frame loop, statistics / performance overlay
  core_synth/          synthetic test core (Phase 2 exit test)
  hwtest/              Phase 1 hardware test mode (later Recovery -> "Test hardware")
host/                  desktop CMake project (plan D8)
test/host/             host unit tests (CTest)
partitions.csv         16 MB layout: nvs, otadata, phy_init, app0/app1 (6 MB each), coredump
sdkconfig.defaults     360 MHz, PSRAM 200 MHz, 1 kHz tick, USB-Serial-JTAG console
```

## Firmware build

Requires **ESP-IDF v5.5.5** exactly (decision D1). Component versions are pinned in
`components/retro_hal/idf_component.yml` and locked in `dependencies.lock`.

Builds run in the official `espressif/idf:v5.5.5` container, the same image CI uses
(pinned by digest in `.github/workflows/ci.yml`):

```sh
# Mount at the same path so build/project_description.json is valid on the host too.
docker run --rm -v "$PWD":"$PWD" -w "$PWD" \
    espressif/idf:v5.5.5@sha256:a9231d0697ab8f7517cc072e93b7c83e04907bfbfba80b6440d7dbbf90665cf2 \
    idf.py build
```

Flash and monitor from the host with a native ESP-IDF v5.5.5 (containers on macOS can't open USB
serial ports). Don't use `idf.py flash` on a container-built `build/`: it re-runs CMake with host
tools. Use the bundled esptool instead:

```sh
. ~/esp/esp-idf-v5.5.5/export.sh
(cd build && esptool.py --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args)
idf.py -p /dev/cu.usbmodemXXXX monitor
```

Expected boot output (abridged):

```text
I (…) CORE: Tab5 Retro Console 0.1.0 (IDF v5.5.5, built …)
I (…) CORE: boot complete
I (…) VIDEO: retimed to 1455 lines (front porch 165): 59.99 Hz nominal
I (…) VIDEO: display 720x1280, 1000 Mbps/lane, 59.96 Hz measured, …
I (…) AUDIO: engine up: 48000 Hz out, DMA 4 x 240 frames (20.0 ms), ring HWM 24 ms
I (…) CORE: Synthetic test core running: 256x240 @ 60.0988 Hz, audio 44100 Hz
I (…) CORE: pipeline test running; type "help" for commands
I (…) CORE: emu 59.90 fps, out 59.90 fps, lcd 59.90 Hz | … | xrun 0, … | dropped 9 | …
```

## Pipeline test mode (Phase 2)

The default boot mode runs a synthetic core (`components/core_synth`) through the whole frontend:
scrolling colour bars, one box per player moved by its D-pad, a row of indicators per player for
every button, MENU/POWER hotkey lamps, the frame counter, a 1-pixel checkerboard and an RGB565 ramp.
It ticks at 880 Hz every emulated second; holding A or B plays a tone (left / right channel). The
performance overlay (spec §45) sits in the right-hand border.

Input is the touch screen (v1 has no USB controllers; see below). The touch controls are drawn in
the side borders and light up green while held: L, the D-pad, SEL and MENU on the left; R, X/Y/A/B
and START on the right. Where they overlap the game image (4:3, Stretch) they're drawn as outlines.
This is a fixed layout until the Phase 3 touch overlay makes it configurable. MENU (or SELECT+START
held 1 s) cycles the display mode; holding MENU 3 s lights POWER (the shutdown itself is Phase 7).
Each touch down/up and each change in the held controls is logged, and the summary line counts
touch reads.

A summary line is logged every 10 s. Serial commands: `mode <0-4>` (Pixel Perfect, Original, 4:3,
Fit, Stretch), `scan on|off`, `overlay on|off`, `bright <0-100>`, `vol <0-100>`,
`res <w> <h>` (synthetic frame size, e.g. `res 256 224` for SNES geometry), `stats`.

### USB controllers (v2)

USB controllers are deferred to v2 (plan D14; spec R11/R12). The USB-A port is unused in v1 and its
5 V rail is switched off at boot. The Xbox 360 / One / Series driver (`xinput_host.c`, plan D11), the
positional default mapping (Xbox B is Nintendo A) and per-`vid:pid` remaps in
`/retro/config/controllers.json` stay in the tree, unit tested, and are switched on with
`CONFIG_RETRO_USB_PADS` (Controller manager menu) for v2 work.

## Hardware test mode

Set `CONFIG_RETRO_HWTEST_BOOT` (Hardware test menu) to boot into `hwtest` instead. It runs the automatic tests once at boot (`CONFIG_RETRO_HWTEST_AUTORUN`,
about 50 s, with short test tones), then waits for commands. The screen shows the log, a status line
(battery, clock, SD, USB pad) and a button bar with every interactive test (OFF must be held 2 s). The serial console takes
commands too: `help` lists them, and `results` prints the Markdown results table. The table is
also saved to `/retro/hwtest/results.md` on the SD card after every command, and each boot keeps the
previous file as `results-N.md`, so results from a session on battery survive the reset caused by
opening the serial port. `cat <path>` prints a saved file.

Interactive checks (touch, headphones, Xbox pads (v2), power button, power-off, light sleep) need someone
at the device; [plan/bringup-results.md](plan/bringup-results.md) lists the remaining ones and how to run them.

Opening the USB serial port resets the board. Output printed during light sleep only appears after
the port is reopened.

## microSD card

FAT32 with **32 KB clusters**. FatFs reads at most one cluster per request, so a card formatted with
small clusters is slow: 512-byte clusters measured 1.8 MB/s against 15 MB/s raw. hwtest warns about
this (`sd.cluster_size`).

## Host build

Needs CMake, Ninja and SDL2 (`brew install cmake ninja sdl2`, or `apt install cmake ninja-build libsdl2-dev`).

```sh
cmake -S host -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
./build-host/tab5emu_host [--mode 0-4] [--scanlines] [--overlay]
```

The host build runs the same pipeline test as the firmware on an emulated 720x1280 portrait panel,
shown rotated in a 1280x720 window. Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q/W = L/R,
Enter = Start, Right Shift = Select, F1 = Menu (cycles the display mode), Esc = quit. The mouse is the
touch screen. The `sd` volume is `./sdcard` (or `$RETRO_SD_ROOT`). For headless runs:
`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build-host/tab5emu_host --frames 120`
(`--max-underruns N` makes it fail on audio underruns; SDL's dummy audio driver runs slow, so the
emulated rate is lower there).

## Licensing

The SNES core is planned to be Snes9x-derived (non-commercial licence) and the NES core is Nofrendo (GPLv2),
so the firmware as a whole is GPL with a non-commercial restriction (decision D10). It is intended for
personal / open-source distribution only.
