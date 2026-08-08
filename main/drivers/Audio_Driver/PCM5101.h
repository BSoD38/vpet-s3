#pragma once

// PCM5101A audio DAC: board pinout only.
//
// This used to also contain a single-file music player built on chmorgan/esp-audio-player.
// That player was removed along with its component: it claimed the I2S channel exclusively
// and decoded exactly one stream, which a game cannot use -- an effect has to land on top of
// whatever is already playing. game/engine/audio/ replaces it with a 4-voice mixer that owns
// the same channel and does its own decoding (see engine/audio/audio.hpp).
//
// What is genuinely board knowledge -- which pins the DAC is wired to -- stays here, because
// that is a property of the Waveshare ESP32-S3-Touch-LCD-2.8 and not of the mixer.
//
// The board provides NO MCLK. The PCM5101A derives its internal clock from BCK in that
// configuration, which is a documented mode of the part and what the stock driver relied on
// too; it is the reason the mixer sets mclk = I2S_GPIO_UNUSED rather than picking a pin.

#include "driver/gpio.h"

#define BSP_I2S_PORT_NUM   1              // I2S_NUM_1

#define BSP_I2S_SCLK       (GPIO_NUM_48)  // BCK
#define BSP_I2S_LCLK       (GPIO_NUM_38)  // WS / LRCK
#define BSP_I2S_DOUT       (GPIO_NUM_47)  // DATA to the DAC
