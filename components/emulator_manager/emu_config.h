/*
 * Task layout and pipeline tuning (plan §3.2, spec §6: "should remain
 * adjustable"). Kconfig options on the Tab5; the same defaults on the host.
 */
#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef CONFIG_RETRO_USB_PADS
#define CONFIG_RETRO_USB_PADS 0 /* deferred to v2 (plan D11) */
#endif

#ifndef CONFIG_RETRO_TASK_EMU_CORE
#define CONFIG_RETRO_TASK_EMU_CORE 0
#define CONFIG_RETRO_TASK_EMU_PRIO 20
#define CONFIG_RETRO_TASK_AUDIO_CORE 1
#define CONFIG_RETRO_TASK_AUDIO_PRIO 22
#define CONFIG_RETRO_TASK_VIDEO_CORE 1
#define CONFIG_RETRO_TASK_VIDEO_PRIO 18
#define CONFIG_RETRO_TASK_INPUT_CORE 1
#define CONFIG_RETRO_TASK_INPUT_PRIO 18
#define CONFIG_RETRO_TASK_USB_PRIO 15
#define CONFIG_RETRO_TASK_MONITOR_PRIO 4
#define CONFIG_RETRO_AUDIO_RATE 48000
#define CONFIG_RETRO_AUDIO_PERIOD_FRAMES 240
#define CONFIG_RETRO_AUDIO_PERIODS 4
/* Desktop audio stacks pull in larger bursts than the Tab5's I2S DMA; 24 ms
 * (the Tab5 default) underruns under CoreAudio, 40 ms doesn't. */
#define CONFIG_RETRO_AUDIO_LATENCY_MS 40
#endif
