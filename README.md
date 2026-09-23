# Tab5 Retro Console

NES and SNES emulation firmware for the M5Stack Tab5 (ESP32-P4), built on bare ESP-IDF.

- Product spec: [plan/tab5-retroconsole.md](plan/tab5-retroconsole.md)
- Implementation plan: [plan/implementation-plan.md](plan/implementation-plan.md)

Status: **Phase 0 (project foundation)**. The firmware boots and logs platform info; the host build opens a window with a test pattern.

## Layout

```text
main/                  firmware entry point
components/
  retro_common/        platform-neutral helpers (no IDF, no SDL)
  retro_hal/           RetroHAL public headers + backends
    include/           retro_log.h, retro_time.h (stable); video/audio/input/platform (provisional until Phase 2)
    tab5/              ESP-IDF / BSP backend (the only code allowed to use the BSP)
    host/              SDL2 backend for the desktop build
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

Expected boot output:

```text
I (…) CORE: Tab5 Retro Console 0.1.0 (IDF v5.5.5, built …)
I (…) CORE: chip rev v1.x, 2 cores, flash 16 MB
I (…) CORE: PSRAM 32768 KB; heap free: …
I (…) CORE: running from app0 @ 0x20000, reset reason: …
I (…) CORE: boot complete
```

## Host build

Needs CMake, Ninja and SDL2 (`brew install cmake ninja sdl2`, or `apt install cmake ninja-build libsdl2-dev`).

```sh
cmake -S host -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
./build-host/tab5emu_host
```

Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q/W = L/R, Enter = Start, Right Shift = Select, F1 = Menu, Esc = quit.
Hold A or B for a test tone. For headless runs: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build-host/tab5emu_host --frames 120`.

## Licensing

The SNES core is planned to be Snes9x-derived (non-commercial licence) and the NES core is Nofrendo (GPLv2),
so the firmware as a whole is GPL with a non-commercial restriction (decision D10). It is intended for
personal / open-source distribution only.
