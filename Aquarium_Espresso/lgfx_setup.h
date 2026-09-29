#ifndef PIXAQ_LGFX_SETUP_H
#define PIXAQ_LGFX_SETUP_H
// ---------------------------------------------------------------------------
// LovyanGFX configuration for the 320x240 ST7789 panel.
// Same wiring as FREEWAY_S3_OPUS / STL_Viewer_S3 in this sketchbook:
//   SCLK 12 / MOSI 11 / DC 9 / CS 10 / RST -- (tie to 3V3 or set PIN_TFT_RST)
// Panel options: BGR order + inversion on, as on the other ST7789 boards here.
// ---------------------------------------------------------------------------
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define PIN_TFT_SCLK 12
#define PIN_TFT_MOSI 11
#define PIN_TFT_DC    9
#define PIN_TFT_CS   10
#define PIN_TFT_RST  -1     // set to 8 if RST is wired
#define PIN_TFT_BLK  -1     // set to a GPIO if the backlight is switched

// 80MHz halves the full-frame transfer (31ms -> 15ms). Drop back to 40000000
// if a longer/looser harness starts showing torn or speckled pixels.
#define TFT_SPI_FREQ 40000000

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;

public:
    LGFX()
    {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = TFT_SPI_FREQ;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = PIN_TFT_SCLK;
            cfg.pin_mosi = PIN_TFT_MOSI;
            cfg.pin_miso = -1;
            cfg.pin_dc   = PIN_TFT_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs   = PIN_TFT_CS;
            cfg.pin_rst  = PIN_TFT_RST;
            cfg.pin_busy = -1;
            cfg.panel_width     = 240;   // portrait native; setRotation(1) -> 320x240
            cfg.panel_height    = 320;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.readable        = false;
            cfg.invert          = true;
            cfg.rgb_order       = false; // BGR
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

#endif // PIXAQ_LGFX_SETUP_H
