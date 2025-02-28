#ifndef IOPINS_H
#define IOPINS_H

#include "platform_config.h"

#define VGA_DMA_CHANNEL 2 // requires 2 channels
#define AUD_DMA_CHANNEL 4 // requires 1 or 3 channels
#define PSR_DMA_CHANNEL 7 // requires 2 channels (PSRAM)

// Speaker
#define AUDIO_PIN       21

// TFT
#define TFT_SPIREG      spi1
#define TFT_SPIDREQ     DREQ_SPI1_TX
#define TFT_SCLK        30
#define TFT_MOSI        31
#define TFT_MISO        28
#define TFT_DC          3
#define TFT_CS          46  // 255 for LORES ST7789 (NO CS)
#define TFT_RST         22  // 255 for ILI/ST if connected to 3.3V
#define TFT_BACKLIGHT   255 // hardwired to 3.3v

// 2 buttons
#define PIN_KEY_USER1   0
#define PIN_KEY_USER2   4 

// SD
#define SD_SCLK         34
#define SD_MOSI         35
#define SD_MISO         36 
#define SD_CS           39
#define SD_DETECT       33
// Second SPI bus and DMA not conflicting with USB
#define SD_SPIREG       spi0


#define PSRAM_PIN_SCK       10
#define PSRAM_PIN_MOSI      11
#define PSRAM_PIN_MISO      8 
#define PSRAM_PIN_CS        9

#define PSRAM_ASYNC         1

#define PSRAM_SCLK          10
#define PSRAM_MOSI          11
#define PSRAM_MISO          8 
#define PSRAM_CS            9

#define PSRAM_SPIREG    	spi1

#if 0 // metro rp2350
#define PIN_CKN (15u)
#define PIN_CKP (14u)
#define PIN_D0N (19u)
#define PIN_D0P (18u)
#define PIN_D1N (17u)
#define PIN_D1P (16u)
#define PIN_D2N (13u)
#define PIN_D2P (12u)
#endif

#define PIN_CKP (13u)
#define PIN_D0P (15u)
#define PIN_D1P (17u)
#define PIN_D2P (19u)

#endif
