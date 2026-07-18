#include "display.hpp"
#include "driver/spi_master.h"   // SPI3_HOST, SPI_DMA_CH_AUTO

// Pin wiring matches main/LCD_Driver/ST7789.h:
//   SCLK 40, MOSI 45, MISO -1, DC 41, CS 42, RST 39, 240x320, 80 MHz.
LGFX::LGFX()
{
    {   // SPI bus
        auto cfg = _bus.config();
        cfg.spi_host    = SPI3_HOST;
        cfg.spi_mode    = 0;
        cfg.freq_write  = 80000000;
        cfg.freq_read   = 16000000;
        cfg.spi_3wire   = false;
        cfg.use_lock    = true;
        cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk    = 40;
        cfg.pin_mosi    = 45;
        cfg.pin_miso    = -1;
        cfg.pin_dc      = 41;
        _bus.config(cfg);
        _panel.setBus(&_bus);
    }
    {   // ST7789 panel
        auto cfg = _panel.config();
        cfg.pin_cs           = 42;
        cfg.pin_rst          = 39;
        cfg.pin_busy         = -1;
        cfg.memory_width     = 240;
        cfg.memory_height    = 320;
        cfg.panel_width      = 240;
        cfg.panel_height     = 320;
        cfg.offset_x         = 0;
        cfg.offset_y         = 0;
        cfg.offset_rotation  = 0;
        cfg.readable         = false;
        cfg.invert           = true;   // ST7789 on this board needs inversion on (old init sent INVON/0x21)
        cfg.rgb_order        = false;  // false = BGR (matches old rgb_endian=BGR); verify colors on screen
        cfg.dlen_16bit       = false;
        cfg.bus_shared       = false;
        _panel.config(cfg);
    }
    setPanel(&_panel);
}

LGFX display;

void Display_Init()
{
    display.init();
    display.setRotation(0);      // verify orientation on screen; adjust 0..3 if needed
    display.setColorDepth(16);
    display.fillScreen(0x0000);  // black
}
