# M5Stack Tab5 Retro Console Firmware

## Product Specification

Version: 1.0  
Target hardware: M5Stack Tab5 / Tab5 Kit  
Primary systems: Nintendo Entertainment System and Super Nintendo Entertainment System  
Platform: ESP32-P4 + ESP32-C6  
Framework: ESP-IDF

---

## 1. Product overview

The project converts the M5Stack Tab5 directly into a self-contained retro gaming console by replacing the normal Tab5 application firmware with a dedicated emulator firmware.

There is no intermediate general-purpose operating system.

The firmware boots directly into a lightweight game library, launches emulator cores natively on the ESP32-P4, renders directly to the Tab5's MIPI-DSI display, outputs audio through the onboard ES8388 codec, and accepts input from USB controllers or the touchscreen.

The initial release targets:

- NES
- SNES

The architecture must allow additional systems, input devices, storage devices, displays and communication peripherals to be added without redesigning the emulator cores.

The fundamental architecture is:

```text
┌──────────────────────────────────────────────┐
│                  Tab5                       │
│                                              │
│  ESP32-P4                                    │
│  ┌────────────────────────────────────────┐ │
│  │ Retro Console Firmware                 │ │
│  │                                        │ │
│  │ Launcher                               │ │
│  │ Emulator Manager                       │ │
│  │   ├── NES core                         │ │
│  │   └── SNES core                        │ │
│  │                                        │ │
│  │ Platform Abstraction Layer             │ │
│  │   ├── Video                            │ │
│  │   ├── Audio                            │ │
│  │   ├── Input                            │ │
│  │   ├── Storage                          │ │
│  │   ├── Network                          │ │
│  │   └── Power                            │ │
│  └────────────────────────────────────────┘ │
│                                              │
│  ESP32-C6                                    │
│        Wireless coprocessor                  │
└──────────────────────────────────────────────┘
```

The firmware should behave like an appliance rather than a development board.

Power on should lead to a usable game library within a few seconds.

---

# 2. Hardware platform

The Tab5 is particularly suitable for this project because it combines a relatively fast microcontroller with dedicated multimedia accelerators and considerably more external memory than conventional ESP32 boards.

The main processor is an ESP32-P4NRW32 containing two high-performance 32-bit RISC-V cores running at 360 MHz and a separate low-power RISC-V core running at up to 40 MHz. The Tab5 provides 16 MB flash and 32 MB Octal PSRAM.

The P4 itself includes:

- 768 KB HP SRAM
- cache for external PSRAM
- scratchpad/TCM memory
- 2D DMA
- GDMA
- MIPI DSI
- MIPI CSI
- PPA pixel accelerator
- JPEG hardware codec
- H.264 encoder
- I2S
- SD/MMC
- USB 2.0 HS and FS controllers
- Ethernet MAC
- multiple UART/I²C/SPI peripherals
- hardware crypto
- low-power processor



This hardware should be explicitly exploited instead of treating the Tab5 like a generic ESP32 board.

---

# 3. Software platform

Use ESP-IDF rather than Arduino.

Recommended baseline:

```text
ESP-IDF
    │
    ├── Espressif M5Stack Tab5 BSP
    │
    ├── RetroHAL
    │
    ├── Launcher
    │
    ├── NES core
    │
    ├── SNES core
    │
    ├── Filesystem
    │
    ├── Input manager
    │
    ├── Audio engine
    │
    └── Optional network services
```

The official Espressif `m5stack_tab5` BSP already provides support for the Tab5 display, touchscreen, audio codec, microphone system, camera, IMU and SD card. Espressif also ships a USB HID example supporting the Tab5.

The project should depend on these drivers wherever possible instead of duplicating them.

This is especially important because different Tab5 hardware revisions may use combinations of:

- ILI9881C
- ST7123
- ST7121
- GT911

for display/touch duties.

The emulator should therefore never directly assume one specific panel revision.

---

# 4. Core architecture

A hardware abstraction interface should separate emulator implementations from the Tab5.

Example:

```text
retro_video
retro_audio
retro_input
retro_storage
retro_time
retro_network
retro_power
```

An emulator core should interact with APIs conceptually similar to:

```c
retro_video_submit(framebuffer);
retro_audio_submit(samples, count);
retro_input_poll(&state);
retro_storage_read(...);
retro_save_write(...);
```

It should not know that the underlying system uses:

- ST7123
- MIPI DSI
- ES8388
- USB HID
- microSD
- ESP32-C6

This makes future ports and peripherals substantially easier.

---

# 5. Emulator cores

## NES

Recommended initial core:

**Nofrendo**

There is already an ESP32-P4 port of Nofrendo capable of rendering directly to a MIPI-DSI framebuffer rather than passing every frame through LVGL. That project also demonstrates multitouch virtual controls.

This should serve as one technical reference for the Tab5 port.

NES target:

```text
CPU          6502-compatible
Resolution   commonly 256 × 240
Frame rate   ~60 Hz NTSC
Audio        5 NES APU channels
ROM format   .nes
```

Target performance:

```text
60 emulated frames/s
60 rendered frames/s
no frame skipping under normal titles
audio underruns = 0
```

---

## SNES

The starting point should be the SNES implementation used by RetroESP32-P4 or a similarly optimized embedded SNES core.

RetroESP32-P4 currently reports SNES execution at 60 FPS on the ESP32-P4 and specifically optimizes the heavier 16-bit systems for this processor.

Supported cartridge types should initially prioritize ordinary LoROM and HiROM games.

Later compatibility targets can include enhancement chips where practical:

```text
DSP-1
CX4
SA-1
Super FX
S-DD1
```

These should not be considered necessary for the first release.

The firmware should report unsupported enhancement chips cleanly rather than crashing.

---

# 6. CPU utilization

The dual high-performance cores should be deliberately partitioned.

Suggested topology:

```text
P4 CPU 0
├── emulator CPU/PPU/APU
└── timing-critical emulation

P4 CPU 1
├── video submission
├── audio feeding
├── storage
├── USB input
└── background services

LP core
└── low-power / wake functionality where worthwhile
```

This should remain adjustable because some emulator cores may benefit more from running their own CPU and audio tasks independently.

The priority is deterministic emulation timing rather than maximum average throughput.

---

# 7. Memory architecture

The P4's internal SRAM is valuable because it is considerably faster and more predictable than PSRAM.

Use internal SRAM for:

```text
hot emulator state
CPU register state
audio ring buffers
input state
DMA descriptors
timing-critical lookup tables
small caches
```

Use the 32 MB Octal PSRAM for:

```text
framebuffers
ROM images
large SNES memory regions
save states
textures
launcher artwork
ROM metadata
large emulator tables
future cores
```

The emulator manager should allocate core memory dynamically so that future larger systems can reuse the same PSRAM pool.

---

# 8. Display pipeline

The Tab5 provides a 5-inch 1280×720 IPS display connected to the ESP32-P4 through a 2-lane MIPI-DSI interface.

Games should never be emulated at 1280×720.

The emulator generates the native console framebuffer.

For NES:

```text
Native:
256 × 240

Pixel-perfect:
768 × 720
3× scaling

Horizontal border:
256 pixels each side
```

For normal SNES modes:

```text
Native:
256 × 224

Pixel-perfect:
768 × 672
3× scaling

Horizontal border:
256 pixels each side

Vertical border:
24 pixels top and bottom
```

This results in near-perfect integer scaling on the Tab5 display.

---

# 9. Pixel Processing Accelerator

The ESP32-P4 contains a dedicated Pixel Processing Accelerator.

The PPA supports hardware:

- scaling
- rotation
- mirroring
- blending
- filling
- RGB565
- RGB888
- ARGB8888
- various YUV formats



The renderer should use the PPA where doing so improves throughput.

Pipeline:

```text
Emulator
    ↓
native RGB565 framebuffer
    ↓
PPA
    ├── scale
    ├── optional rotate
    └── optional overlay blend
    ↓
720p display framebuffer
    ↓
MIPI DSI
```

One important exception is pixel-perfect integer scaling.

Espressif documents the PPA scaler as bilinear.

Therefore:

**Pixel Perfect mode**

Use nearest-neighbour integer replication, potentially using DMA/PPA fill strategies or optimized CPU routines.

**Smooth mode**

Use the PPA's bilinear scaler.

This gives the user a choice between authentic sharp pixels and smooth scaling.

---

# 10. Display modes

Provide:

### Pixel Perfect

Integer nearest-neighbour scaling.

Best default.

### Original

1× native framebuffer centered on screen.

Useful for debugging.

### 4:3

Scale output to the largest correct 4:3 region.

### Fit

Maintain aspect ratio while maximizing screen use.

### Stretch

1280×720 fullscreen.

Not default.

### Scanline

Optional lightweight scanline overlay.

### CRT

Future feature only if performance permits.

Filters must never reduce emulation speed.

---

# 11. Framebuffer strategy

Use double or triple buffering.

Recommended:

```text
Framebuffer A
    emulator writes

Framebuffer B
    renderer/PPA reads

Display framebuffer
    MIPI DSI scans
```

Prefer asynchronous processing.

CPU should never block waiting for the display if a completed frame can safely be dropped.

Audio timing takes precedence over visual frame delivery.

---

# 12. Launcher

The launcher can use LVGL because performance is irrelevant outside gameplay.

During gameplay, LVGL should either:

- stop rendering, or
- remain initialized without owning the game framebuffer.

This pattern has already been demonstrated by the ESP32-P4 Nofrendo implementation.

Main launcher:

```text
┌─────────────────────────────────────────────────┐
│  NES     SNES                         Settings  │
├─────────────────────────────────────────────────┤
│                                                 │
│   Super Mario Bros.                             │
│                                                 │
│   Zelda                                         │
│                                                 │
│   Metroid                                       │
│                                                 │
│   Mega Man 2                                    │
│                                                 │
│                                                 │
│                    Battery 82%    Wi-Fi         │
└─────────────────────────────────────────────────┘
```

Support:

- list view
- grid view
- favourites
- recently played
- last played
- alphabetical sorting
- system filtering
- ROM search
- play-time tracking

---

# 13. Storage

The microSD slot should be the primary content storage.

The Tab5 exposes it through six dedicated P4 GPIOs and supports SDIO mode as well as SPI mode.

Prefer SD/MMC rather than SPI wherever driver reliability permits.

Directory layout:

```text
/retro
├── roms
│   ├── nes
│   └── snes
│
├── saves
│   ├── nes
│   └── snes
│
├── states
│   ├── nes
│   └── snes
│
├── artwork
├── metadata
├── screenshots
├── config
└── logs
```

ROM formats:

```text
NES
.nes

SNES
.sfc
.smc
```

Potential later support:

```text
.zip
```

Archive support should be evaluated against RAM and decompression overhead rather than assumed.

---

# 14. Save system

Support two separate save mechanisms.

### Cartridge saves

Automatic SRAM/EEPROM persistence where the original cartridge supported it.

Write periodically and on clean game exit.

### Save states

Allow complete emulator snapshots.

Provide at least:

```text
Quick Save
Quick Load
Slot 1
Slot 2
Slot 3
Slot 4
Slot 5
```

Save state files should include:

```text
magic
version
core ID
ROM checksum
timestamp
screenshot thumbnail
core state
```

This prevents loading a state with an incompatible firmware/core version.

---

# 15. Audio

The Tab5 contains:

- ES8388 audio codec
- ES7210 AEC front end
- dual microphones
- NS4150B amplifier
- 1 W / 8 Ω speaker
- 3.5 mm headphone output



Gameplay output should use the ES8388 directly through I2S.

Pipeline:

```text
Emulator APU
    ↓
native sample generation
    ↓
resampler
    ↓
ring buffer
    ↓
I2S DMA
    ↓
ES8388
    ├── speaker
    └── headphone
```

Audio timing should be treated as the master pacing signal wherever practical.

Target:

```text
sample rate: 44.1 or 48 kHz
output: stereo
latency target: <40 ms
underruns: zero during normal gameplay
```

Headphone detection should automatically switch output if supported cleanly by the BSP.

The Tab5 exposes a headphone-detect signal through its GPIO expander.

---

# 16. Microphones

The dual-microphone ES7210/AEC hardware is not required for conventional NES/SNES emulation but should remain accessible.

Potential future uses:

```text
voice game search
voice commands
multiplayer voice chat
recording clips with commentary
streaming
game-specific microphone emulation
```

The emulator core API should therefore not claim exclusive access to the audio subsystem.

---

# 17. USB controllers

The USB-A port should be the primary physical controller interface.

The ESP32-P4 USB host subsystem supports:

- low-speed USB
- full-speed USB
- high-speed USB
- control transfers
- bulk transfers
- interrupt transfers
- isochronous transfers
- multiple class drivers simultaneously
- USB hubs



ESP-IDF already provides a USB HID host driver, and Espressif specifically provides a Tab5 USB HID example.

Initial input compatibility should include generic HID controllers.

Controller manager architecture:

```text
Input Manager
│
├── TouchInput
├── USBHIDInput
├── KeyboardInput
└── Future
    ├── BluetoothInput
    ├── GPIOInput
    └── NetworkInput
```

Normalize everything into:

```c
typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;

    bool a;
    bool b;
    bool x;
    bool y;

    bool l;
    bool r;

    bool start;
    bool select;
} retro_pad_state_t;
```

Emulator cores never interact directly with USB.

---

# 18. Controller mapping

On first connection:

```text
Press UP
Press DOWN
Press LEFT
Press RIGHT
Press A
Press B
Press X
Press Y
Press L
Press R
Press START
Press SELECT
```

Save mappings by USB VID/PID.

Provide sensible automatic mappings for recognized common controllers.

Support hot-plugging.

---

# 19. USB hubs

Because ESP-IDF supports USB hubs on the P4, future versions should support multiple wired controllers connected through a powered USB hub.

This enables multiplayer without changing the Tab5.

Potential configuration:

```text
Tab5
  │
USB-A
  │
USB Hub
  ├── Controller 1
  ├── Controller 2
  ├── Controller 3
  └── Controller 4
```

NES should ultimately support two controllers.

SNES should support at least two initially, with multitap support considered later.

---

# 20. USB storage

ESP-IDF also supplies a USB Mass Storage Class host driver for the P4.

Later releases should permit ROMs to be loaded from:

```text
microSD
USB flash drive
USB SSD
```

The storage layer should therefore expose logical volumes instead of hard-coding `/sd`.

Example:

```text
/storage/sd
/storage/usb0
/storage/usb1
```

---

# 21. USB-C port

The USB-C OTG connection should primarily provide:

```text
firmware flashing
debugging
serial console
development
possible USB device functionality later
```

Potential later USB device modes:

```text
MTP-like ROM transfer
USB storage gadget
serial management
network gadget
controller emulation
```

The P4 has separate high-speed and full-speed USB OTG controllers, and Espressif supports simultaneous host functionality at the silicon level.

The exact Tab5 physical routing must be respected when assigning roles.

---

# 22. Touch controls

The 5-inch capacitive touchscreen should permit controller-free operation.

Example:

```text
┌──────────────────────────────────────────────────────┐
│                                                      │
│                   GAME AREA                          │
│                                                      │
│                                                      │
│                                                      │
│  ┌─────┐                                  Y    X     │
│ ┌┘  ↑  └┐                              B    A        │
│ │ ←   → │                                           │
│ └┐  ↓  ┌┘                      SELECT   START        │
│  └─────┘                                             │
└──────────────────────────────────────────────────────┘
```

Required:

- multitouch
- configurable button positions
- adjustable size
- adjustable opacity
- haptic hooks for future peripherals
- hide overlay when USB controller is active
- customizable layouts per system

The existing P4 Nofrendo project demonstrates simultaneous touch direction/button handling, so this is technically realistic.

---

# 23. IMU

The Tab5 includes a BMI270 six-axis accelerometer/gyroscope with interrupt wake support.

Use it where it provides meaningful functionality rather than as a gimmick.

Possible functions:

### Orientation detection

Automatically rotate launcher UI when appropriate.

### Shake actions

Optional:

```text
shake → open emulator menu
```

Disabled by default.

### Tilt input

Expose the IMU as an optional analog input source for future games or systems.

### Wake

Movement can wake the device from low-power mode.

The emulator input system should support generic axes so IMU control does not require changing emulator cores.

---

# 24. RTC

The Tab5 includes an RX8130CE real-time clock backed by a supercapacitor and capable of interrupt wake.

Use it for:

```text
save timestamps
play history
screenshots
logs
RTC-aware games
scheduled wake
usage statistics
```

SNES/NES generally have little need for RTC emulation, but future handheld cores may.

---

# 25. Power monitoring

The Tab5 exposes an INA226 voltage/current monitoring IC.

Use it to calculate:

```text
battery voltage
current draw
estimated battery percentage
estimated remaining runtime
power consumption
```

The launcher should display battery status.

The emulator menu can expose a performance panel:

```text
Battery          76%
Voltage          7.61 V
Current          690 mA
Power            5.25 W
Temperature      42°C
FPS              60.0
Audio buffer     62%
```

This can be hidden by default.

---

# 26. Battery operation

The Tab5 Kit uses a removable NP-F550 7.4 V 2000 mAh battery, approximately 14.8 Wh.

M5Stack reports roughly six hours under its reference workload with screen brightness at 50%, Wi-Fi enabled and background tasks running. Emulator runtime should be benchmarked separately because sustained P4 emulation load differs substantially.

Provide performance profiles:

```text
Performance
Balanced
Battery Saver
```

Performance:

```text
CPU 360 MHz
normal brightness
wireless as configured
```

Battery Saver:

```text
reduce launcher brightness
disable Wi-Fi when unused
disable camera
power down unused expansion rails
disable unnecessary services
```

Do not reduce emulator clocks dynamically if doing so causes unstable frame pacing.

---

# 27. Power gating

The Tab5's onboard GPIO expanders can separately control several device power paths.

Notably:

- ESP32-C6 wireless power
- USB-A 5 V
- external 5 V expansion bus
- speaker enable
- charging controls



The firmware should exploit this.

For example:

```text
No USB controller:
USB-A 5V can be disabled

Offline mode:
ESP32-C6 can be powered down

No expansion peripheral:
external 5V rail can be disabled

Headphones attached:
speaker amplifier can be disabled
```

This is one of the major advantages of writing a dedicated firmware rather than leaving every subsystem permanently powered.

---

# 28. Wireless subsystem

The Tab5 uses an ESP32-C6-MINI-1U as a separate wireless processor connected to the P4 over SDIO. The Tab5 documentation specifies 2.4 GHz Wi-Fi 6, Thread and Zigbee support.

Wireless should be optional, not required for playing games.

Initial networking uses:

```text
Wi-Fi setup
firmware update
time synchronization
metadata downloading
```

Later uses:

```text
ROM transfer
network shares
RetroAchievements integration
multiplayer experiments
remote debugging
web management interface
```

The C6 should be powered off when wireless networking is disabled.

---

# 29. Antenna system

The Tab5 provides an internal antenna and external MMCX antenna interfaces, with an electronically controlled antenna path.

No special emulator integration is needed.

Settings may eventually expose:

```text
Wi-Fi antenna
• Internal
• External
```

This is useful if the Tab5 becomes a fixed living-room console or remote streaming client.

---

# 30. Camera

The Tab5 contains a roughly 2 MP MIPI-CSI camera connected directly to the P4.

M5Stack lists an SC2356 1600×1200 camera in current product documentation, while the Espressif BSP documentation references another camera part in some revisions. Camera access must therefore go through the supported camera framework rather than assume a single sensor model.

The P4 also contains:

```text
MIPI CSI
ISP
JPEG codec
H.264 encoder
```



The camera has little purpose for NES/SNES v1, but keeping the subsystem available enables future features:

```text
QR-code Wi-Fi configuration
profile/avatar capture
camera-enabled console emulation
streaming
room camera overlay
gameplay recording with face camera
computer vision input experiments
```

Camera initialization should be lazy so it consumes no resources during normal gameplay.

---

# 31. Screenshot capture

Allow an input combination such as:

```text
SELECT + R
```

to capture the current framebuffer.

Save:

```text
/retro/screenshots/
```

Use the P4 hardware JPEG codec where appropriate rather than spending emulator CPU time compressing screenshots. The ESP32-P4 includes hardware JPEG encode/decode support.

---

# 32. Gameplay recording

Not required for v1.

However, the P4 contains a hardware H.264 encoder rated by Espressif for multimedia workloads up to 1080p-class encoding capabilities depending on configuration.

A later feature could record scaled gameplay.

Potential pipeline:

```text
Emulator framebuffer
        ↓
PPA scaling
        ↓
H.264 encoder
        ↓
microSD
```

Audio can be muxed separately.

This should only be implemented after verifying that simultaneous emulation, scaling, encoding and SD writes do not compromise game timing.

---

# 33. Networking and game metadata

Optional Wi-Fi metadata service may obtain:

```text
game title
release year
publisher
genre
cover image
system
checksum mappings
```

The firmware should identify ROMs using a checksum rather than relying solely on filenames.

Suggested:

```text
CRC32
SHA-1
```

The metadata database should remain optional and cached on microSD.

---

# 34. Firmware updates

Support two methods.

### USB flashing

Always available through normal ESP-IDF tooling.

### OTA

Optional through Wi-Fi.

Use two OTA application partitions if practical within the 16 MB internal flash.

Example:

```text
bootloader
partition table
NVS
OTA data
app A
app B
recovery/config
```

Large ROMs and assets must not live in internal flash.

They belong on removable storage.

---

# 35. Recovery mode

Provide a minimal recovery path.

Holding a defined input during boot should enter:

```text
Recovery

• boot firmware
• reset settings
• test hardware
• mount SD
• view logs
• Wi-Fi recovery
• firmware update
```

The normal ESP32 download mode must remain accessible regardless of application state.

---

# 36. Physical keyboard support

M5Stack now offers a dedicated Tab5 Keyboard.

It contains:

- 70 keys
- STM32F030 controller
- I²C communication
- interrupt line
- RGB LEDs
- normal/HID/character modes

and connects using GPIO 0, GPIO 1 and GPIO 50 for SDA, SCL and interrupt.

The input abstraction should therefore allow the keyboard to become:

```text
launcher search keyboard
emulator hotkey device
computer-emulator keyboard
debug terminal input
future DOS/8-bit computer input
```

This will become particularly useful if systems such as MSX, ZX Spectrum or DOS-era software are added.

---

# 37. M5-Bus expansion

The Tab5 exposes M5-Bus and a controllable external 5 V rail.

Peripheral drivers should use a registration model:

```c
retro_peripheral_register(&driver);
```

A device can advertise capabilities:

```text
INPUT
AUDIO
STORAGE
NETWORK
DISPLAY
HAPTIC
SENSOR
```

This allows M5Stack modules to be added without coupling them to individual emulator cores.

Potential future modules:

```text
gamepad dock
GPIO controller adapter
Ethernet
LoRa
additional storage
rumble module
external DAC
MIDI
IR
```

---

# 38. HY2.0-4P / Grove expansion

The Tab5 exposes a HY2.0-4P port with:

```text
GND
5V
GPIO53
GPIO54
```



Potential uses include:

```text
physical buttons
rotary encoder
rumble motor controller
LED lighting
external sensors
arcade controls
```

Do not assume every Grove peripheral is I²C.

The peripheral manager should define GPIO, UART and I²C extension interfaces separately.

---

# 39. GPIO_EXT

The side expansion connector should be treated as a generic extension path.

A later dedicated controller board could attach:

```text
D-pad
A/B/X/Y
L/R
Start
Select
dual analog sticks
rumble
battery/accessory detection
```

This could eventually turn the Tab5 into a proper handheld without requiring USB.

Input abstraction makes this transparent to emulator cores.

---

# 40. Stamp expansion pads

M5Stack provides reserved Stamp expansion pads intended for additional communication modules including:

```text
Cat-M
NB-IoT
LoRaWAN
```



These have little direct relevance to NES/SNES but reinforce the requirement that networking be abstracted.

Future specialist uses could include:

```text
remote telemetry
mesh multiplayer experiments
location-independent management
custom radio controllers
```

They should not affect v1 complexity.

---

# 41. RS-485

The Tab5 contains a SIT3088 RS-485 transceiver with switchable 120 Ω termination and accepts 6 to 24 V through the associated industrial interface.

This is unusual for a gaming device but potentially useful.

Possible future applications:

```text
arcade cabinet controls
custom wired multiplayer
industrial button panels
external lighting
remote controller nodes
installation exhibits
```

The firmware should simply expose an optional generic serial transport.

No RS-485 functionality is required for release 1.

---

# 42. Wired networking

The ESP32-P4 itself contains an Ethernet MAC.

Although the stock Tab5 does not expose an integrated RJ45 Ethernet port, future M5-Bus or GPIO hardware could provide the necessary PHY/interface.

The networking abstraction therefore should not assume Wi-Fi.

Conceptually:

```text
NetworkManager
├── WiFiTransport
├── EthernetTransport
└── FutureTransport
```

This could become useful for low-latency network features.

---

# 43. External controller protocol

Provide a simple internal driver model.

```text
device connected
        ↓
identify transport
        ↓
driver probe
        ↓
enumerate controls
        ↓
assign player
        ↓
map buttons
        ↓
RetroInput events
```

Potential future sources:

```text
USB HID
Tab5 touchscreen
Tab5 Keyboard
GPIO
I²C controllers
Bluetooth controllers
RS-485 controllers
network controllers
```

The emulator sees no distinction between them.

---

# 44. Multiplayer architecture

Input state should support at least four logical players even if initial emulator cores only consume two.

```text
Player 1
Player 2
Player 3
Player 4
```

This avoids changing the input ABI later.

USB hubs and future wireless input can populate these logical slots.

---

# 45. Performance overlay

Developer mode should provide a real-time overlay containing:

```text
Game FPS
Rendered FPS
Frame time
CPU0 usage
CPU1 usage
PSRAM usage
internal RAM usage
audio buffer depth
audio underruns
SD throughput
PPA time
USB polling latency
battery voltage
power consumption
```

This will be important while tuning SNES titles.

Disable all instrumentation in production unless explicitly enabled.

---

# 46. Logging

Logs should support:

```text
USB serial
in-memory circular log
optional SD log
```

Subsystem categories:

```text
CORE
VIDEO
AUDIO
INPUT
USB
SD
NETWORK
POWER
EMU-NES
EMU-SNES
```

Persistent logging should normally be disabled during gameplay to avoid SD contention.

---

# 47. Emulator menu

Pressing a configurable controller combination should suspend emulation and open:

```text
Resume

Save State
Load State

Controller
Display
Audio

Screenshot

Reset Game
Quit Game
```

The emulator remains allocated in memory while the menu is visible.

The launcher UI should not be reconstructed unnecessarily.

---

# 48. Game suspend

When returning to the launcher, optionally maintain one suspended game in PSRAM if memory permits.

Flow:

```text
Game
 ↓
Suspend
 ↓
Launcher
 ↓
Resume instantly
```

If insufficient memory exists, serialize a temporary save state instead.

---

# 49. Fast boot

Target boot process:

```text
ROM bootloader
      ↓
application
      ↓
display init
      ↓
SD mount
      ↓
load cached library index
      ↓
launcher
```

Do not rescan the complete SD card every boot.

Store an index and update it when directory metadata changes or when the user requests rescan.

---

# 50. Power-on resume

Optional setting:

```text
Boot behavior

○ Launcher
● Last game
○ Last game + state
```

"Last game + state" automatically restores an autosave generated during shutdown.

---

# 51. Graceful shutdown

Before power-off:

```text
pause emulator
flush SRAM save
write autosave if enabled
flush filesystem
persist settings
disable speaker
unmount storage
power down peripherals
shutdown
```

Avoid unnecessary writes during gameplay.

---

# 52. Security

Because networking and OTA may eventually be available, use the ESP32-P4's security capabilities where useful.

The P4 provides hardware:

```text
AES
SHA
ECC
HMAC
secure boot
flash encryption support
hardware key management
TRNG
```



For personal-development firmware this can remain optional, but OTA packages should at least support cryptographic signature verification.

---

# 53. Development environment

Primary:

```text
ESP-IDF
C
C++
CMake
idf.py
```

Avoid unnecessary runtime layers.

Recommended repositories/components:

```text
Espressif esp-bsp
m5stack_tab5 BSP
ESP-IDF USB Host
ESP-IDF SDMMC
ESP Codec Dev
PPA driver
MIPI DSI driver
```

M5Stack's own Tab5 user demo currently builds directly using ESP-IDF and provides source demonstrating the board's display, audio, camera, SD and other hardware integrations.

---

# 54. Repository architecture

```text
tab5-retro/
│
├── main/
│   └── main.c
│
├── components/
│   │
│   ├── retro_hal/
│   │   ├── video/
│   │   ├── audio/
│   │   ├── input/
│   │   ├── storage/
│   │   ├── network/
│   │   └── power/
│   │
│   ├── launcher/
│   ├── rom_library/
│   ├── save_manager/
│   ├── controller_manager/
│   │
│   ├── emulator_manager/
│   │
│   ├── core_nes/
│   └── core_snes/
│
├── assets/
├── partitions.csv
├── sdkconfig.defaults
├── CMakeLists.txt
└── README.md
```

---

# 55. Core API

Every emulator should implement a standard interface.

Conceptually:

```c
typedef struct {
    const char *name;

    bool (*probe)(const char *path);
    int  (*load)(const char *path);
    void (*reset)(void);

    void (*run_frame)(void);

    void (*input)(const retro_pad_state_t *pads);

    int (*save_state)(void *buffer, size_t size);
    int (*load_state)(const void *buffer, size_t size);

    void (*unload)(void);
} retro_core_t;
```

This makes adding systems straightforward.

---

# 56. Future emulator expansion

Once NES/SNES are stable, the same firmware can progressively incorporate cores already demonstrated on ESP32-P4-class projects.

RetroESP32-P4 currently demonstrates:

```text
NES
SNES
Game Boy
Game Boy Color
Master System
Game Gear
Genesis / Mega Drive
Atari 2600
Atari 7800
Atari 8-bit
Lynx
Neo Geo
PC Engine
ColecoVision
ZX systems
```

and even native ports such as Doom and Quake.

This does not mean every system should immediately be included.

Suggested progression:

```text
Phase 1
NES
SNES

Phase 2
GB
GBC
Master System
Game Gear

Phase 3
Genesis
PC Engine

Phase 4
Atari
ZX
Coleco

Phase 5
Neo Geo
native games/apps
```

---

# 57. Features deliberately excluded from v1

Do not allow peripheral possibilities to delay the basic emulator.

Release 1 does not require:

```text
camera features
game recording
network multiplayer
RetroAchievements
Bluetooth controllers
USB mass storage
Ethernet
RS-485
LoRa
voice commands
cloud synchronization
advanced shaders
SNES enhancement-chip completeness
```

The architecture supports these features, but the release does not depend on them.

---

# 58. Version 1 requirements

Release 1 is complete when the device can:

1. Boot directly into the retro launcher.
2. Mount a microSD card.
3. Discover `.nes`, `.sfc` and `.smc` ROMs.
4. Launch NES games.
5. Launch standard SNES games.
6. Maintain correct game speed.
7. Output synchronized audio.
8. Render directly through MIPI-DSI.
9. Use pixel-perfect scaling.
10. Use the built-in touchscreen.
11. Use a USB HID controller.
12. hot-plug controllers.
13. Save cartridge SRAM.
14. Create/load save states.
15. Take screenshots.
16. Show battery state.
17. control display brightness.
18. detect headphones.
19. power down unused wireless hardware.
20. shut down without corrupting saves.

---

# 59. Performance targets

NES:

```text
Emulation rate       100%
Target FPS           60
Frameskip            0 normally
Audio underruns      0
Input latency        <2 frames
```

SNES:

```text
Emulation rate       100% for supported titles
Target FPS           60 NTSC / 50 PAL
Audio underruns      0
Input latency        <2 frames
```

Launcher:

```text
Target UI FPS        60
Boot-to-library      <5 seconds target
Game launch          <2 seconds target
```

These are product targets rather than guarantees from the underlying hardware.

---

# 60. Latency priorities

When the system is overloaded, sacrifice work in this order:

```text
1. cosmetic launcher/background tasks
2. network tasks
3. metadata
4. screenshot/recording work
5. rendered video frames
```

Never intentionally sacrifice:

```text
emulated CPU timing
input polling
audio continuity
save integrity
```

The game should continue at correct speed even if an occasional rendered frame must be skipped.

---

# 61. Development milestones

## Milestone A

Bring up:

```text
display
SD card
speaker
USB HID
touch
power monitoring
```

using the official Tab5 BSP.

## Milestone B

NES:

```text
Nofrendo core
direct framebuffer
audio
USB controls
touch controls
```

## Milestone C

Launcher:

```text
ROM scanning
game selection
settings
controller configuration
```

## Milestone D

Persistence:

```text
SRAM
save states
recent games
screenshots
```

## Milestone E

SNES:

```text
core integration
performance profiling
audio tuning
video timing
```

## Milestone F

Optimization:

```text
PPA scaling
DMA transfers
memory placement
power gating
fast boot
```

## Milestone G

Expansion APIs:

```text
pluggable input drivers
pluggable storage
network abstraction
future core interface
```

---

# 62. Hardware feature utilization summary

The intention is to leave as little of the Tab5 hardware wasted as practical.

| Tab5 hardware | Emulator use |
|---|---|
| Dual 360 MHz P4 cores | Emulator + system workloads |
| LP core | wake/power tasks |
| 32 MB PSRAM | ROMs, framebuffer, save states |
| 16 MB flash | firmware |
| 5" 1280×720 display | game output and launcher |
| MIPI DSI | high-bandwidth direct display |
| PPA | scaling, rotation, compositing |
| 2D DMA / GDMA | framebuffer and peripheral transfers |
| ES8388 | game audio |
| speaker | built-in playback |
| headphone jack | private low-latency audio |
| ES7210 + dual mic | future voice/capture |
| USB-A Host | controllers/hubs/storage |
| USB-C OTG | programming/future USB functions |
| microSD | ROMs/saves/assets |
| touchscreen | controller-free gameplay |
| BMI270 | orientation/tilt/wake |
| RTC | timestamps/history/wake |
| INA226 | battery/power telemetry |
| NP-F550 | portable operation |
| ESP32-C6 | optional networking |
| internal/MMCX antenna | wireless deployments |
| camera | future QR/stream/vision features |
| JPEG codec | screenshots/artwork |
| H.264 encoder | future recording |
| M5-Bus | expansion accessories |
| GPIO_EXT | custom controller hardware |
| HY2.0/Grove | buttons/sensors/haptics |
| Tab5 Keyboard | text/computer emulator input |
| Stamp pads | alternate communication hardware |
| RS-485 | arcade/install/custom wired devices |
| 1/4"-20 mount | tabletop/arcade mounting |

---

# 63. Final product behavior

The intended experience is:

```text
Press power
    ↓
Retro launcher appears
    ↓
Choose game
    ↓
Game starts immediately
    ↓
USB controller or touchscreen works
    ↓
Audio plays through speaker/headphones
    ↓
Menu button opens save/settings overlay
    ↓
Quit returns instantly to library
```

Everything else should remain invisible unless needed.

The Tab5's unusual advantage is that it combines a microcontroller-style environment with display, audio, storage, USB host, substantial PSRAM and dedicated multimedia hardware in one device. The firmware should therefore behave more like embedded console firmware than like an application running on a tablet.

The core design principle is:

```text
Emulators remain portable.
Tab5-specific optimization lives below them.
Peripheral support lives beside them.
```

That allows the first version to remain small and focused while leaving a clean path toward additional systems, USB devices, custom controllers, network functionality, storage devices and M5Stack expansion modules later.