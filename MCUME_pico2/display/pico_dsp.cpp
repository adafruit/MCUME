/*
	This file is part of DISPLAY library. 
  Supports VGA and TFT display

	DISPLAY library is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Copyright (C) 2020 J-M Harvengt
*/

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>

#include "pico_dsp.h"
#include "font8x8.h"
#include "include.h"

#include "hdmi_framebuffer.h"

static hdmi_framebuffer_obj_t hdmi_obj;
static gfx_mode_t gfxmode = MODE_UNDEFINED;

/* TFT structures / constants */
#define digitalWrite(pin, val) gpio_put(pin, val)
#define SPICLOCK 60000000 
#ifdef USE_VGA
#define SPI_MODE SPI_CPOL_1
#else
#ifdef ST7789
#ifdef ST7789_POL
#define SPI_MODE SPI_CPOL_0
#else
#define SPI_MODE SPI_CPOL_1
#endif
#endif
#ifdef ILI9341
#define SPI_MODE SPI_CPOL_0
#endif
#endif

#define LINES_PER_BLOCK  64
#define NR_OF_BLOCK      4

#define TFT_SWRESET    0x01
#define TFT_SLPOUT     0x11
#define TFT_INVON      0x21
#define TFT_DISPOFF    0x28
#define TFT_DISPON     0x29
#define TFT_CASET      0x2A
#define TFT_PASET      0x2B
#define TFT_RAMWR      0x2C
#define TFT_MADCTL     0x36
#define TFT_PIXFMT     0x3A
#define TFT_MADCTL_MY  0x80
#define TFT_MADCTL_MX  0x40
#define TFT_MADCTL_MV  0x20
#define TFT_MADCTL_ML  0x10
#define TFT_MADCTL_RGB 0x00
#define TFT_MADCTL_BGR 0x08
#define TFT_MADCTL_MH  0x04

static void SPItransfer(uint8_t val)
{
  uint8_t dat8=val;
  spi_write_blocking(TFT_SPIREG, &dat8, 1);
}

static void SPItransfer16(uint16_t val)
{
  uint8_t dat8[2];
  dat8[0] = val>>8;
  dat8[1] = val&0xff;
  spi_write_blocking(TFT_SPIREG, dat8, 2);
}

#define DELAY_MASK     0x80
static const uint8_t init_commands[] = { 
  1+DELAY_MASK, TFT_SWRESET,  150,
  1+DELAY_MASK, TFT_SLPOUT,   255,
  2+DELAY_MASK, TFT_PIXFMT, 0x55, 10,
  2,            TFT_MADCTL, TFT_MADCTL_MV | TFT_MADCTL_BGR,
  1, TFT_INVON,
  1, TFT_DISPON,
  0
};

/* TFT structures / constants */
#define RGBVAL16(r,g,b)  ( (((r>>3)&0x1f)<<11) | (((g>>2)&0x3f)<<5) | (((b>>3)&0x1f)<<0) )

static uint16_t * blocks[NR_OF_BLOCK];
static uint16_t blocklens[NR_OF_BLOCK];
static dma_channel_config dmaconfig;
static uint dma_tx=0;
static volatile uint8_t rstop = 0;
static volatile bool cancelled = false;
static volatile uint8_t curTransfer = 0;
static uint8_t nbTransfer = 0;

#undef B16
/* VGA structures / constants */
#define R16(rgb) ((rgb>>8)&0xf8) 
#define G16(rgb) ((rgb>>3)&0xfc) 
#define B16(rgb) ((rgb<<3)&0xf8) 
#ifdef VGA222
#define VGA_RGB(r,g,b)   ( (((r>>6)&0x03)<<4) | (((g>>6)&0x03)<<2) | (((b>>6)&0x3)<<0) )
#else
#define VGA_RGB(r,g,b)   ( (((r>>5)&0x07)<<5) | (((g>>5)&0x07)<<2) | (((b>>6)&0x3)<<0) )
#endif

// 8 bits 320x240 frame buffer => 64K
static vga_pixel * visible_framebuffer = NULL;
static vga_pixel * framebuffer = NULL;
static vga_pixel * fb0 = NULL;
static vga_pixel * fb1 = NULL;

static int  fb_width;
static int  fb_height;
static int  fb_stride;

static int doorbell_id;


void PICO_DSP::setArea(uint16_t x1,uint16_t y1,uint16_t x2,uint16_t y2) {
  int dx=0;
  int dy=0;
#ifdef ST7789
  if (TFT_REALWIDTH == TFT_REALHEIGHT)
  {
#ifdef ROTATE_SCREEN
    if (!flipped) {
      dy += 80;    
    }
#else  
    if (flipped) {
      dx += 80;    
    }
#endif      
  }
#endif  

  digitalWrite(_dc, 0);
  SPItransfer(TFT_CASET);
  digitalWrite(_dc, 1);
  SPItransfer16(x1+dx);
  digitalWrite(_dc, 1);
  SPItransfer16(x2+dx);  
  digitalWrite(_dc, 0);
  SPItransfer(TFT_PASET);
  digitalWrite(_dc, 1);
  SPItransfer16(y1+dy);
  digitalWrite(_dc, 1);
  SPItransfer16(y2+dy);  

  digitalWrite(_dc, 0);
  SPItransfer(TFT_RAMWR);
  digitalWrite(_dc, 1);
 
  return; 
}


PICO_DSP::PICO_DSP()
{
}


static void (* volatile Core1Fnc)() = NULL; // core 1 remote function

static void VgaCore(void)
{


  void (*fnc)();
  while (1)
  {
    __dmb();
#ifdef HAS_SND
//    handle_fill_samples();
#endif
    // execute remote function
    fnc = Core1Fnc;
    if (fnc != NULL)
    {
      fnc();
      __dmb();
      Core1Fnc = NULL;
    }   
  }
}


gfx_error_t PICO_DSP::begin(gfx_mode_t mode)
{
  // Reset SPI if we come from TFT mode
  if ( (mode != MODE_TFT_320x240) && (gfxmode == MODE_TFT_320x240) ) {
    fillScreenNoDma(RGBVAL16(0x0,0x00,0x00));
    digitalWrite(_cs, 0);
    digitalWrite(_dc, 0);
    SPItransfer(TFT_DISPOFF);
    digitalWrite(_cs, 1);
    sleep_ms(20);
    digitalWrite(_cs, 0);
    digitalWrite(_cs, 1);
    if (_bkl != 0xff) {
      digitalWrite(_bkl, 0);
    }
    //spi_init(TFT_SPIREG, 0);
    //spi_deinit(TFT_SPIREG);
    //spi_set_slave(TFT_SPIREG, true);
  }  
  switch(mode) {
    case MODE_VGA_320x240:
      gfxmode = mode;
      fb_width = 320;
      fb_height = 240;
      fb_stride = 320;
      hdmi_obj.framebuffer = NULL;
      hdmi_framebuffer(&hdmi_obj, fb_width, fb_height, 8);
      fb0 = (uint8_t *)hdmi_obj.framebuffer;
      visible_framebuffer = fb0;
      framebuffer = fb0;
      break;

    case MODE_VGA_256x240:
      gfxmode = mode;
      fb_width = 256;
      fb_height = 240;
      fb_stride = 320;
      hdmi_obj.framebuffer = NULL;
      hdmi_framebuffer(&hdmi_obj, fb_width, fb_height, 8);
      fb0 = (uint8_t *)hdmi_obj.framebuffer;
      visible_framebuffer = fb0;
      framebuffer = fb0;
      break;

    case MODE_VGA_640x240:
      gfxmode = mode;
      fb_width = 640;
      fb_height = 240;
      fb_stride = 640;
      hdmi_obj.framebuffer = NULL;
      hdmi_framebuffer(&hdmi_obj, fb_width, fb_height, 8);
      fb0 = (uint8_t *)hdmi_obj.framebuffer;
      visible_framebuffer = fb0;
      framebuffer = fb0;
      break;

    case MODE_TFT_320x240:
      gfxmode = mode;
      fb_width = TFT_WIDTH;
      fb_height = TFT_HEIGHT;      
      fb_stride = fb_width;    
      _cs   = TFT_CS;
      _dc   = TFT_DC;
      _rst  = TFT_RST;
      _mosi = TFT_MOSI;
      _sclk = TFT_SCLK;
      _bkl = TFT_BACKLIGHT;
      gpio_init(_dc);
      gpio_set_dir(_dc, GPIO_OUT); 
      gpio_init(_cs);
      gpio_set_dir(_cs, GPIO_OUT);
      digitalWrite(_cs, 1);
      digitalWrite(_dc, 1);
      if (_bkl != 0xff) {
        gpio_init(_bkl);
        gpio_set_dir(_bkl, GPIO_OUT); 
        digitalWrite(_bkl, 1);
      } 

      spi_init(TFT_SPIREG, SPICLOCK);
      spi_set_format(TFT_SPIREG, 8, SPI_MODE, SPI_CPHA_0, SPI_MSB_FIRST);
      gpio_set_function(_sclk , GPIO_FUNC_SPI);
      gpio_set_function(_mosi , GPIO_FUNC_SPI);
         
      // Initialize display
      if (_rst != 0xff) {
        gpio_init(_rst);
        gpio_set_dir(_rst, GPIO_OUT);     
        digitalWrite(_rst, 1);
        sleep_ms(100);
        digitalWrite(_rst, 0);
        sleep_ms(100);
        digitalWrite(_rst, 1);
        sleep_ms(200);
      }
      const uint8_t *addr = init_commands;
      uint8_t count;
      digitalWrite(_cs, 0);
      while (count = *addr++) {
        uint8_t command = *addr++;
#ifdef ILI9341
        if ( command == TFT_INVON ) {
          // Skip TFT_INVON for ILI
        }
        else 
#endif
        {
          digitalWrite(_dc, 0); // command
          SPItransfer(command);
          uint16_t ms = count & DELAY_MASK;
          count &= ~DELAY_MASK;
          while (--count > 0) { // data
            uint8_t data = *addr++;
#ifdef ILI9341
#else
            if ( command == TFT_MADCTL ) {
              data = TFT_MADCTL_MX | TFT_MADCTL_MV |TFT_MADCTL_RGB;
            }
#endif
            digitalWrite(_dc, 1);
            SPItransfer(data);
          }
          if (ms) {
            ms = *addr++; // Read post-command delay time (ms)
            if(ms == 255) ms = 500;   // If 255, delay for 500 ms
            digitalWrite(_cs, 1);
            //SPI.endTransaction();
            sleep_ms(ms);
            //SPI.beginTransaction(SPISettings(SPICLOCK, MSBFIRST, SPI_MODE));
            digitalWrite(_cs, 0);      
          }     
        }
      }
      digitalWrite(_cs, 1);
      break;
  }	


  return(GFX_OK);
}

void PICO_DSP::end()
{
}

gfx_mode_t PICO_DSP::getMode(void)
{
  return gfxmode;
}

void PICO_DSP::flipscreen(bool flip)
{
  digitalWrite(_dc, 0);
  digitalWrite(_cs, 0);
  SPItransfer(TFT_MADCTL);
  digitalWrite(_dc, 1);
  if (flip) {
    flipped=true;

#ifdef ILI9341         
    SPItransfer(TFT_MADCTL_MV | TFT_MADCTL_BGR);
#endif
#ifdef ST7789
#ifdef ROTATE_SCREEN
    SPItransfer(TFT_MADCTL_RGB);
#else
    SPItransfer(TFT_MADCTL_MY | TFT_MADCTL_MV |TFT_MADCTL_RGB);
#endif
#endif 
  }
  else {
    flipped=false;
   
#ifdef ILI9341        
    SPItransfer(TFT_MADCTL_MX | TFT_MADCTL_MY | TFT_MADCTL_MV | TFT_MADCTL_BGR);
#endif
#ifdef ST7789
#ifdef ROTATE_SCREEN
    SPItransfer(TFT_MADCTL_MX | TFT_MADCTL_MY | TFT_MADCTL_RGB);
#else
    SPItransfer(TFT_MADCTL_MX | TFT_MADCTL_MV | TFT_MADCTL_RGB);
#endif
#endif  
  }
  digitalWrite(_cs, 1);   
}


bool PICO_DSP::isflipped(void)
{
  return(flipped);
}
  

/***********************************************************************************************
    DMA functions
 ***********************************************************************************************/
static void dma_isr() { 
  irq_clear(DMA_IRQ_0);
  dma_hw->ints0 = 1u << dma_tx;
  curTransfer++;
  if (curTransfer >= nbTransfer) {
    curTransfer = 0;
  }
  if (cancelled) {
    rstop = 1;
  }
  else 
  {
    dma_channel_transfer_from_buffer_now(dma_tx, blocks[curTransfer], blocklens[curTransfer]);
  }  
}

static void setDmaStruct() {
  // Setup the control channel
  if (dma_tx == 0)  {
    dma_tx = dma_claim_unused_channel(true);
  }    
  dmaconfig = dma_channel_get_default_config(dma_tx);
  channel_config_set_transfer_data_size(&dmaconfig, DMA_SIZE_16);
  channel_config_set_dreq(&dmaconfig, TFT_SPIDREQ);  
  //channel_config_set_read_increment(&dmaconfig, true); // read incrementing
  //channel_config_set_write_increment(&dmaconfig, false); // no write incrementing

  dma_channel_configure(
      dma_tx,
      &dmaconfig,
      &spi_get_hw(TFT_SPIREG)->dr, // write address
      blocks[0],
      blocklens[0],
      false
  ); 

  irq_set_exclusive_handler(DMA_IRQ_0, dma_isr);
  dma_channel_set_irq0_enabled(dma_tx, true);
  irq_set_enabled(DMA_IRQ_0, true);
  dma_hw->ints0 = 1u << dma_tx;  
}

void PICO_DSP::startRefresh(void) {
  if (gfxmode == MODE_TFT_320x240) {
    uint32_t remaining = TFT_HEIGHT*TFT_WIDTH*2;
    int i=0;
    nbTransfer = 0;
    while (remaining > 0) {
      uint16_t * fb = blocks[i];
      int32_t len = (remaining >= (LINES_PER_BLOCK*TFT_WIDTH*2)?LINES_PER_BLOCK*TFT_WIDTH*2:remaining);     
      switch (i) {
        case 0:
          if (fb == 0) fb = (uint16_t*)((int)malloc(len+64)&0xffffffe0);       
          break;
        case 1:
          if (fb == 0) fb = (uint16_t*)((int)malloc(len+64)&0xffffffe0);      
          break;
        case 2:
          if (fb == 0) fb = (uint16_t*)((int)malloc(len+64)&0xffffffe0);      
          break;
        case 3:
          if (fb == 0) fb = (uint16_t*)((int)malloc(len+64)&0xffffffe0);       
          break;
      }
      blocks[i] = fb;
      blocklens[i] = len/2;
      if (blocks[i] == 0) {
        fillScreenNoDma(RGBVAL16(0xFF,0xFF,0x00));
        printf("FB allocaltion failed for block %d\n",i);
        sleep_ms(10000);    
      }
      nbTransfer++;
      remaining -= len;    
      i++;
    }                
    curTransfer = 0;  
    rstop = 0;     
    digitalWrite(_cs, 1);
    setDmaStruct();
    fillScreen(RGBVAL16(0x00,0x00,0x00));
    digitalWrite(_cs, 0);

    setArea((TFT_REALWIDTH-TFT_WIDTH)/2, (TFT_REALHEIGHT-TFT_HEIGHT)/2, (TFT_REALWIDTH-TFT_WIDTH)/2 + TFT_WIDTH-1, (TFT_REALHEIGHT-TFT_HEIGHT)/2+TFT_HEIGHT-1);  
    // we switch to 16bit mode!!
    spi_set_format(TFT_SPIREG, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    dma_start_channel_mask(1u << dma_tx);    
  }
  else { 
    fillScreen(RGBVAL16(0x00,0x00,0x00));   
  }
}

void PICO_DSP::stopRefresh(void) {
  if (gfxmode == MODE_TFT_320x240) {
    rstop = 1;
    unsigned long m = time_us_32()*1000;   
    cancelled = true; 
    while (!rstop)  {
      if ((time_us_32()*1000 - m) > 100) break;
      sleep_ms(100);
      asm volatile("wfi");
    };
    rstop = 0;
    sleep_ms(100);
    cancelled = false;  
    //dmatx.detachInterrupt();
    fillScreen(RGBVAL16(0x00,0x00,0x00));
    digitalWrite(_cs, 1);
    // we switch back to GFX mode!!
    begin(gfxmode);
    setArea(0, 0, TFT_REALWIDTH-1, TFT_REALHEIGHT-1);    
  }
}


/***********************************************************************************************
    GFX functions
 ***********************************************************************************************/
// retrieve size of the frame buffer
int PICO_DSP::get_frame_buffer_size(int *width, int *height)
{
  if (width != nullptr) *width = fb_width;
  if (height != nullptr) *height = fb_height;
  return fb_stride;
}

void PICO_DSP::waitSync()
{
  if (gfxmode == MODE_TFT_320x240) {
  }
  else { 
    hdmi_framebuffer_vsync();
    //HdmiVSync();
  }
}

void PICO_DSP::waitLine(int line)
{
  if (gfxmode == MODE_TFT_320x240) {
  }
  else { 
//  while (currentLine != line) {};
  }
}


/***********************************************************************************************
    GFX functions
 ***********************************************************************************************/

dsp_pixel * PICO_DSP::getLineBuffer(int j) {
  return ((dsp_pixel *)&framebuffer[j*fb_stride]);
}

void PICO_DSP::fillScreen(dsp_pixel color) {
  int i,j;
  if (gfxmode == MODE_TFT_320x240) {
    for (j=0; j<TFT_HEIGHT; j++)
    {
      uint16_t * block=blocks[j>>6];
      uint16_t * dst=&block[(j&0x3F)*fb_stride];
      for (i=0; i<fb_width; i++)
      {
        *dst++ = color;
      }
    }
  }
  else {
    vga_pixel color8 = VGA_RGB(R16(color),G16(color),B16(color));
    for (j=0; j<fb_height; j++)
    {
      vga_pixel * dst=&framebuffer[j*fb_stride];
      for (i=0; i<fb_width; i++)
      {
        *dst++ = color8;
      }
    }    
  }  
}

void PICO_DSP::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, dsp_pixel color) {
  int i,j,l=y;
  if (gfxmode == MODE_TFT_320x240) {
    for (j=0; j<h; j++)
    {
      uint16_t * block=blocks[l>>6];
      uint16_t * dst=&block[(l&0x3F)*fb_stride+x];
      for (i=0; i<w; i++)
      {
        *dst++ = color;
      }
      l++;
    }
  }
  else {
    vga_pixel color8 = VGA_RGB(R16(color),G16(color),B16(color));
    for (j=0; j<h; j++)
    {
      vga_pixel * dst=&framebuffer[l*fb_stride+x];
      for (i=0; i<w; i++)
      {
        *dst++ = color8;
      }
      l++;
    }
  }  
}

void PICO_DSP::drawText(int16_t x, int16_t y, const char * text, dsp_pixel fgcolor, dsp_pixel bgcolor, bool doublesize) {
  if (gfxmode == MODE_TFT_320x240) {
    uint16_t c;
    uint16_t * block;
    uint16_t * dst;
    fgcolor = fgcolor;
    bgcolor = bgcolor;
    while ((c = *text++)) {
      const unsigned char * charpt=&font8x8[c][0];
      int l=y;
      for (int i=0;i<8;i++)
      {     
        unsigned char bits;
        if (doublesize) {
          block=blocks[l>>6];
          dst=&block[(l&0x3F)*fb_stride+x];         
          bits = *charpt;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor;
          else *dst++=bgcolor;
          l++;       
        }
        block=blocks[l>>6];
        dst=&block[(l&0x3F)*fb_stride+x]; 
        bits = *charpt++;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor;
        else *dst++=bgcolor;
        l++;
      }
      x +=8;
    }  
  }
  else {
    vga_pixel fgcolor8 = VGA_RGB(R16(fgcolor),G16(fgcolor),B16(fgcolor));
    vga_pixel bgcolor8 = VGA_RGB(R16(bgcolor),G16(bgcolor),B16(bgcolor));
    vga_pixel c;
    vga_pixel * dst;
    while ((c = *text++)) {
      const unsigned char * charpt=&font8x8[c][0];
      int l=y;
      for (int i=0;i<8;i++)
      {     
        unsigned char bits;
        if (doublesize) {
          dst=&framebuffer[l*fb_stride+x];
          bits = *charpt;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          bits = bits >> 1;     
          if (bits&0x01) *dst++=fgcolor8;
          else *dst++=bgcolor8;
          l++;
        }
        dst=&framebuffer[l*fb_stride+x]; 
        bits = *charpt++;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        bits = bits >> 1;     
        if (bits&0x01) *dst++=fgcolor8;
        else *dst++=bgcolor8;
        l++;
      }
      x +=8;
    }
  }  
}

void PICO_DSP::drawSprite(int16_t x, int16_t y, const dsp_pixel *bitmap, uint16_t arx, uint16_t ary, uint16_t arw, uint16_t arh)
{
  int bmp_offx = 0;
  int bmp_offy = 0;
  uint16_t *bmp_ptr;
  int w =*bitmap++;
  int h = *bitmap++;
  if ( (arw == 0) || (arh == 0) ) {
    // no crop window
    arx = x;
    ary = y;
    arw = w;
    arh = h;
  }
  else {
    if ( (x>(arx+arw)) || ((x+w)<arx) || (y>(ary+arh)) || ((y+h)<ary)   ) {
      return;
    }
    // crop area
    if ( (x > arx) && (x<(arx+arw)) ) { 
      arw = arw - (x-arx);
      arx = arx + (x-arx);
    } else {
      bmp_offx = arx;
    }
    if ( ((x+w) > arx) && ((x+w)<(arx+arw)) ) {
      arw -= (arx+arw-x-w);
    }  
    if ( (y > ary) && (y<(ary+arh)) ) {
      arh = arh - (y-ary);
      ary = ary + (y-ary);
    } else {
      bmp_offy = ary;
    }
    if ( ((y+h) > ary) && ((y+h)<(ary+arh)) ) {
      arh -= (ary+arh-y-h);
    }     
  }
  int l=ary;
  bitmap = bitmap + bmp_offy*w + bmp_offx;


  if (gfxmode == MODE_TFT_320x240) {
    for (int row=0;row<arh; row++)
    {
      uint16_t * block=blocks[l>>6];
      uint16_t * dst=&block[(l&0x3F)*fb_stride+arx];  
      bmp_ptr = (uint16_t*)bitmap;
      for (int col=0;col<arw; col++)
      {
          *dst++ = *bmp_ptr++;            
      } 
      bitmap += w;
      l++;
    } 
  }
  else {
    for (int row=0;row<arh; row++)
    {
      vga_pixel * dst=&framebuffer[l*fb_stride+arx];  
      bmp_ptr = (uint16_t *)bitmap;
      for (int col=0;col<arw; col++)
      {
          uint16_t pix= *bmp_ptr++;
          *dst++ = VGA_RGB(R16(pix),G16(pix),B16(pix));
      } 
      bitmap += w;
      l++;
    }     
  }  

}

void PICO_DSP::drawSprite(int16_t x, int16_t y, const dsp_pixel *bitmap) {
    drawSprite(x,y,bitmap, 0,0,0,0);
}

void PICO_DSP::writeLine(int width, int height, int y, dsp_pixel *buf) {
  if (gfxmode == MODE_TFT_320x240) {
    uint16_t * block=blocks[y>>6];
    uint16_t * dst=&block[(y&0x3F)*fb_stride];
    if (width > fb_width) {
#ifdef TFT_LINEARINT   
      int delta = (width/(width-fb_width))-1;   
      int pos = delta;
      for (int i=0; i<fb_width; i++)
      {
        uint16_t val = *buf++;
        pos--;      
        if (pos == 0) {
#ifdef LINEARINT_HACK        
          val  = ((uint32_t)*buf++ + val)/2;
#else
          uint16_t val2 = *buf++;
          val = RGBVAL16((R16(val)+R16(val2))/2,(G16(val)+G16(val2))/2,(B16(val)+B16(val2))/2);
 #endif        
          pos = delta;
        }
        *dst++=val;
      }
  #else
      int step = ((width << 8)/fb_width);
      int pos = 0;
      for (int i=0; i<fb_width; i++)
      {
        *dst++=buf[pos >> 8];
        pos +=step;
      }  
  #endif       
    }
    else if ((width*2) == fb_width) 
    {
      for (int i=0; i<width; i++)
      {
        *dst++=*buf;
        *dst++=*buf++;
      }       
    }
    else
    {
      if (width <= fb_width) {
        dst += (fb_width-width)/2;
      }
      for (int i=0; i<width; i++)
      {
        *dst++=*buf++;
      }       
    }    
  }
  else {
    if ( (height<fb_height) && (height > 2) ) y += (fb_height-height)/2;
    vga_pixel * dst=&framebuffer[y*fb_stride];    
    if (width > fb_width) {
      int step = ((width << 8)/fb_width);
      int pos = 0;
      for (int i=0; i<fb_width; i++)
      {
        uint16_t pix = buf[pos >> 8];
        *dst++ = VGA_RGB(R16(pix),G16(pix),B16(pix)); 
        pos +=step;
      }        
    }
    else if ((width*2) == fb_width) {
      for (int i=0; i<width; i++)
      {
        uint16_t pix = *buf++;
        vga_pixel col = VGA_RGB(R16(pix),G16(pix),B16(pix));
        *dst++= col;
        *dst++= col;
      }       
    }
    else {
      if (width <= fb_width) {
        dst += (fb_width-width)/2;
      }
      for (int i=0; i<width; i++)
      {
        uint16_t pix = *buf++;
        *dst++= VGA_RGB(R16(pix),G16(pix),B16(pix));
      }      
    }
  }  
}

void PICO_DSP::writeLinePal(int width, int height, int y, uint8_t *buf, dsp_pixel *palette) {
  if (gfxmode == MODE_TFT_320x240) {
    if ( (height<fb_height) && (height > 2) ) y += (fb_height-height)/2;
    uint16_t * block=blocks[y>>6];
    uint16_t * dst=&block[(y&0x3F)*fb_stride];
    if (width > fb_width) {
#ifdef TFT_LINEARINT    
      int delta = (width/(width-fb_width))-1;
      int pos = delta;
      for (int i=0; i<fb_width; i++)
      {
        uint16_t val = palette[*buf++];
        pos--;
        if (pos == 0) {
#ifdef LINEARINT_HACK
          val  = ((uint32_t)palette[*buf++] + val)/2;
#else
          uint16_t val2 = *buf++;
          val = RGBVAL16((R16(val)+R16(val2))/2,(G16(val)+G16(val2))/2,(B16(val)+B16(val2))/2);
#endif        
          pos = delta;
        }
        *dst++=val;
      }
#else
      int step = ((width << 8)/fb_width);
      int pos = 0;
      for (int i=0; i<fb_width; i++)
      {
        *dst++=palette[buf[pos >> 8]];
        pos +=step;
      }  
#endif
    }
    else if ((width*2) == fb_width) {
      for (int i=0; i<width; i++)
      {
        *dst++=palette[*buf];
        *dst++=palette[*buf++];
      } 
    }
    else {
      if (width <= fb_width) {
        dst += (fb_width-width)/2;
      }
      for (int i=0; i<width; i++)
      {
        *dst++=palette[*buf++];
      } 
    }    
  }  
  else {
    if ( (height<fb_height) && (height > 2) ) y += (fb_height-height)/2;
    vga_pixel * dst=&framebuffer[y*fb_stride];
    if (width > fb_width) {
      int step = ((width << 8)/fb_width);
      int pos = 0;
      for (int i=0; i<fb_width; i++)
      {
        uint16_t pix = palette[buf[pos >> 8]];
        *dst++= VGA_RGB(R16(pix),G16(pix),B16(pix));
        pos +=step;
      }  
    }
    else if ((width*2) == fb_width) {
      for (int i=0; i<width; i++)
      {
        uint16_t pix = palette[*buf++];
        *dst++= VGA_RGB(R16(pix),G16(pix),B16(pix));
        *dst++= VGA_RGB(R16(pix),G16(pix),B16(pix));
      } 
    }
    else {
      if (width <= fb_width) {
        dst += (fb_width-width)/2;
      }
      for (int i=0; i<width; i++)
      {
        uint16_t pix = palette[*buf++];
        *dst++= VGA_RGB(R16(pix),G16(pix),B16(pix));
      } 
    }
  }
}

void PICO_DSP::writeScreenPal(int width, int height, int stride, uint8_t *buf, dsp_pixel *palette16) {
  uint8_t *src; 
  int i,j,y=0;
  int sy = 0;  
  int systep=(1<<8);
  int h = height;
  if (height <= ( (2*fb_height)/3)) {
    systep=(systep*height)/fb_height;
    h = fb_height;
  }  
  if (gfxmode == MODE_TFT_320x240) {
    if (width*2 <= fb_width) {
      for (j=0; j<h; j++)
      {
        uint16_t * block=blocks[y>>6];
        uint16_t * dst=&block[(y&0x3F)*fb_stride];        
        src=&buf[(sy>>8)*stride];
        for (i=0; i<width; i++)
        {
          uint16_t val = palette16[*src++];
          *dst++ = val;
          *dst++ = val;
        }
        y++;
        sy+=systep;  
      }
    }
    else if (width <= fb_width) {
      for (j=0; j<h; j++)
      {
        uint16_t * block=blocks[y>>6];
        uint16_t * dst=&block[(y&0x3F)*fb_stride+(fb_width-width)/2];        
        src=&buf[(sy>>8)*stride];
        for (i=0; i<width; i++)
        {
          uint16_t val = palette16[*src++];
          *dst++ = val;
        }
        y++;
        sy+=systep;  
      }
    }
  }       
  else { // VGA
    if (width*2 <= fb_width) {
      for (j=0; j<h; j++)
      {
        vga_pixel * dst=&framebuffer[y*fb_stride];                
        src=&buf[(sy>>8)*stride];
        for (i=0; i<width; i++)
        {
          uint16_t pix = palette16[*src++];
          *dst++ = VGA_RGB(R16(pix),G16(pix),B16(pix));
          *dst++ = VGA_RGB(R16(pix),G16(pix),B16(pix));
        }
        y++;
        sy+=systep;  
      }
    }
    else if (width <= fb_width) {
      for (j=0; j<h; j++)
      {
        vga_pixel * dst=&framebuffer[y*fb_stride+(fb_width-width)/2];                
        src=&buf[(sy>>8)*stride];
        for (i=0; i<width; i++)
        {
          uint16_t pix = palette16[*src++];
          *dst++ = VGA_RGB(R16(pix),G16(pix),B16(pix));
        }
        y++;
        sy+=systep;  
      }
    }
  }
}


/***********************************************************************************************
    No DMA functions
 ***********************************************************************************************/
void PICO_DSP::fillScreenNoDma(dsp_pixel color) {
  if (gfxmode == MODE_TFT_320x240) {
    digitalWrite(_cs, 0);
    setArea(0, 0, TFT_REALWIDTH-1, TFT_REALHEIGHT-1);  
    int i,j;
    for (j=0; j<TFT_REALHEIGHT; j++)
    {
      for (i=0; i<TFT_REALWIDTH; i++) {
        //digitalWrite(_dc, 1);
        SPItransfer16(color);     
      }
    }
#ifdef ILI9341  
    digitalWrite(_dc, 0);
    SPItransfer(ILI9341_SLPOUT);
    digitalWrite(_dc, 1);
#endif
    digitalWrite(_cs, 1);
    setArea(0, 0, (TFT_REALWIDTH-1), (TFT_REALHEIGHT-1));     
  }
  else {
    fillScreen(color);
  }   
}

void PICO_DSP::drawRectNoDma(int16_t x, int16_t y, int16_t w, int16_t h, dsp_pixel color) {
  if (gfxmode == MODE_TFT_320x240) {
    digitalWrite(_cs, 0);
    setArea(x,y,x+w-1,y+h-1);
    int i;
    for (i=0; i<(w*h); i++)
    {
      SPItransfer16(color);
    }
#ifdef ILI9341  
    digitalWrite(_dc, 0);
    SPItransfer(ILI9341_SLPOUT);
    digitalWrite(_dc, 1);
#endif
    digitalWrite(_cs, 1);
    setArea(0, 0, (TFT_REALWIDTH-1), (TFT_REALHEIGHT-1));   
  }
  else {
    drawRect(x, y, w, h, color);
  }  
}


void PICO_DSP::drawSpriteNoDma(int16_t x, int16_t y, const dsp_pixel *bitmap) {  
  drawSpriteNoDma(x,y,bitmap, 0,0,0,0);
}

void PICO_DSP::drawSpriteNoDma(int16_t x, int16_t y, const dsp_pixel *bitmap, uint16_t arx, uint16_t ary, uint16_t arw, uint16_t arh)
{
  if (gfxmode == MODE_TFT_320x240) {
    int bmp_offx = 0;
    int bmp_offy = 0;
    uint16_t *bmp_ptr;
    int w =*bitmap++;
    int h =*bitmap++;
    if ( (arw == 0) || (arh == 0) ) {
      // no crop window
      arx = x;
      ary = y;
      arw = w;
      arh = h;
    }
    else {
      if ( (x>(arx+arw)) || ((x+w)<arx) || (y>(ary+arh)) || ((y+h)<ary)   ) {
        return;
      }
      // crop area
      if ( (x > arx) && (x<(arx+arw)) ) { 
        arw = arw - (x-arx);
        arx = arx + (x-arx);
      } else {
        bmp_offx = arx;
      }
      if ( ((x+w) > arx) && ((x+w)<(arx+arw)) ) {
        arw -= (arx+arw-x-w);
      }  
      if ( (y > ary) && (y<(ary+arh)) ) {
        arh = arh - (y-ary);
        ary = ary + (y-ary);
      } else {
        bmp_offy = ary;
      }
      if ( ((y+h) > ary) && ((y+h)<(ary+arh)) ) {
        arh -= (ary+arh-y-h);
      }     
    }
    digitalWrite(_cs, 0);
    setArea(arx, ary, arx+arw-1, ary+arh-1);        
    bitmap = bitmap + bmp_offy*w + bmp_offx;
    for (int row=0;row<arh; row++)
    {
      bmp_ptr = (uint16_t*)bitmap;
      for (int col=0;col<arw; col++)
      {
          SPItransfer16(*bmp_ptr++);             
      } 
      bitmap +=  w;
    }
#ifdef ILI9341  
    digitalWrite(_dc, 0);
    SPItransfer(ILI9341_SLPOUT);
    digitalWrite(_dc, 1);
#endif
    setArea(0, 0, TFT_REALWIDTH-1, TFT_REALHEIGHT-1);  
    digitalWrite(_cs, 1);  
  }
  else {
    drawSprite(x, y, bitmap, arx, ary, arw, arh);
  }   
}

void PICO_DSP::drawTextNoDma(int16_t x, int16_t y, const char * text, dsp_pixel fgcolor, dsp_pixel bgcolor, bool doublesize) {
  if (gfxmode == MODE_TFT_320x240) {
    uint16_t c;
    while ((c = *text++)) {
      const unsigned char * charpt=&font8x8[c][0];
      digitalWrite(_cs, 0);
      setArea(x,y,x+7,y+(doublesize?15:7));
      for (int i=0;i<8;i++)
      {
        unsigned char bits;
        if (doublesize) {
          bits = *charpt;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);
          bits = bits >> 1;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);
          bits = bits >> 1;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);
          bits = bits >> 1;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);
          bits = bits >> 1;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);
          bits = bits >> 1;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);
          bits = bits >> 1;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);
          bits = bits >> 1;     
          if (bits&0x01) SPItransfer16(fgcolor);
          else SPItransfer16(bgcolor);       
        }
        bits = *charpt++;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
        bits = bits >> 1;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
        bits = bits >> 1;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
        bits = bits >> 1;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
        bits = bits >> 1;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
        bits = bits >> 1;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
        bits = bits >> 1;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
        bits = bits >> 1;     
        if (bits&0x01) SPItransfer16(fgcolor);
        else SPItransfer16(bgcolor);
      }
      x +=8;
#ifdef ILI9341  
      digitalWrite(_dc, 0);
      SPItransfer(ILI9341_SLPOUT);
      digitalWrite(_dc, 1);
#endif
      digitalWrite(_cs, 1); 
    }

    digitalWrite(_cs, 0);
    setArea(0, 0, (TFT_REALWIDTH-1), (TFT_REALHEIGHT-1));  
    digitalWrite(_cs, 1);
  }
  else {
    drawText(x, y, text, fgcolor, bgcolor, doublesize);
  } 
}


#ifdef HAS_SND

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "pico/float.h"
#include <string.h>
#include <stdio.h>
#include "pico/audio_i2s.h"
#include "hardware/i2c.h"


static void (*fillsamples)(audio_sample * stream, int len) = nullptr;

/********************************
 * Initialization
********************************/ 
struct audio_buffer_pool *producer_pool;

#if AUDIO_8BIT
_Static_assert(!AUDIO_8BIT, "only configured for 16 bit audio");
#endif

static audio_format_t audio_format = {
        .sample_freq = SOUNDRATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 1,
};

const struct audio_i2s_config config =
        {
            .data_pin = PICO_AUDIO_I2S_DATA_PIN,
            .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
            .dma_channel = AUD_DMA_CHANNEL,
            .pio_sm = 0,
            .clock_pins_swapped = true,
        };

static struct audio_buffer_format producer_format = {
        .format = &audio_format,
        .sample_stride = 2
};

#define I2C_ADDR 0x18

#define DEBUG_I2C (0)
static void writeRegister(uint8_t reg, uint8_t value) {
  uint8_t buf[2];
  buf[0] = reg;
  buf[1] = value;
  int res = i2c_write_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ false, 1000);
  if (res != 2) {
    panic("i2c_write_timeout failed: res=%d\n", res);
  }
if(DEBUG_I2C)
  printf("Write Reg: %d = 0x%x\n", reg, value);
}

static uint8_t readRegister(uint8_t reg) {
  uint8_t buf[1];
  buf[0] = reg;
  int res = i2c_write_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ true, 1000);
  if (res != 1) {
if(DEBUG_I2C)
printf("res=%d\n", res);
    panic("i2c_write_timeout failed: res=%d\n", res);
  }
  res = i2c_read_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ false, 1000);
  if (res != 1) {
if(DEBUG_I2C)
printf("res=%d\n", res);
    panic("i2c_read_timeout failed: res=%d\n", res);
  }
  uint8_t value = buf[0];
if(DEBUG_I2C)
  printf("Read Reg: %d = 0x%x\n", reg, value);
  return value;
}

static void modifyRegister(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = readRegister(reg);
if(DEBUG_I2C)
  printf("Modify Reg: %d = [Before: 0x%x] with mask 0x%x and value 0x%x\n", reg, current, mask, value);
  uint8_t new_value = (current & ~mask) | (value & mask);
  writeRegister(reg, new_value);
}

static void setPage(uint8_t page) {
  printf("Set page %d\n", page);
  writeRegister(0x00, page);
}


static void Wire_begin() {
    i2c_init(i2c0, 100000);
    gpio_set_function(20, GPIO_FUNC_I2C);
    gpio_set_function(21, GPIO_FUNC_I2C);
}

static void i2s_audio_init(void)
{

  gpio_init(22);
  gpio_set_dir(22, true);
  gpio_put(22, true); // allow i2s to come out of reset

  Wire_begin();
  sleep_ms(1000);
  
if(DEBUG_I2C)
  printf("initialize codec\n");

  // Reset codec
  writeRegister(0x01, 0x01);
  sleep_ms(10);

  // Interface Control
  modifyRegister(0x1B, 0xC0, 0x00);
  modifyRegister(0x1B, 0x30, 0x00);

  // Clock MUX and PLL settings
  modifyRegister(0x04, 0x03, 0x03);
  modifyRegister(0x04, 0x0C, 0x04);
  
  writeRegister(0x06, 0x20); // PLL J
  writeRegister(0x08, 0x00); // PLL D LSB
  writeRegister(0x07, 0x00); // PLL D MSB
  
  modifyRegister(0x05, 0x0F, 0x02); // PLL P/R
  modifyRegister(0x05, 0x70, 0x10);

  // DAC/ADC Config
  modifyRegister(0x0B, 0x7F, 0x08); // NDAC
  modifyRegister(0x0B, 0x80, 0x80);
  
  modifyRegister(0x0C, 0x7F, 0x02); // MDAC
  modifyRegister(0x0C, 0x80, 0x80);
  
  modifyRegister(0x12, 0x7F, 0x08); // NADC
  modifyRegister(0x12, 0x80, 0x80);
  
  modifyRegister(0x13, 0x7F, 0x02); // MADC
  modifyRegister(0x13, 0x80, 0x80);

  // PLL Power Up
  modifyRegister(0x05, 0x80, 0x80);

  // Headset and GPIO Config
setPage(1);
modifyRegister(0x2e, 0xFF, 0x0b); 
setPage(0);
  modifyRegister(0x43, 0x80, 0x80); // Headset Detect
  modifyRegister(0x30, 0x80, 0x80); // INT1 Control
  modifyRegister(0x33, 0x3C, 0x14); // GPIO1


  // DAC Setup
  modifyRegister(0x3F, 0xC0, 0xC0);

  // DAC Routing
  setPage(1);
  modifyRegister(0x23, 0xC0, 0x40);
  modifyRegister(0x23, 0x0C, 0x04);

  // DAC Volume Control
  setPage(0);
  modifyRegister(0x40, 0x0C, 0x00);
  writeRegister(0x41, 0x28); // Left DAC Vol
  writeRegister(0x42, 0x28); // Right DAC Vol

  // ADC Setup
  modifyRegister(0x51, 0x80, 0x80);
  modifyRegister(0x52, 0x80, 0x00);
  writeRegister(0x53, 0x68); // ADC Volume

  // Headphone and Speaker Setup
  setPage(1);
  modifyRegister(0x1F, 0xC0, 0xC0); // HP Driver
  modifyRegister(0x28, 0x04, 0x04); // HP Left Gain
  modifyRegister(0x29, 0x04, 0x04); // HP Right Gain
  writeRegister(0x24, 0x0A);  // Left Analog HP
  writeRegister(0x25, 0x0A);  // Right Analog HP
  
  modifyRegister(0x28, 0x78, 0x40); // HP Left Gain
  modifyRegister(0x29, 0x78, 0x40); // HP Right Gain

  // Speaker Amp
  modifyRegister(0x20, 0x80, 0x80);
  modifyRegister(0x2A, 0x04, 0x04);
  modifyRegister(0x2A, 0x18, 0x08);
  writeRegister(0x26, 0x0A);

  // Return to page 0
  setPage(0);

if(DEBUG_I2C)
  printf("Initialization complete!\n");


  // Read all registers for verification
if(DEBUG_I2C) {
  printf("Reading all registers for verification:\n");
  
  setPage(0);
  readRegister(0x00);  // AIC31XX_PAGECTL
  readRegister(0x01);  // AIC31XX_RESET
  readRegister(0x03);  // AIC31XX_OT_FLAG
  readRegister(0x04);  // AIC31XX_CLKMUX
  readRegister(0x05);  // AIC31XX_PLLPR
  readRegister(0x06);  // AIC31XX_PLLJ
  readRegister(0x07);  // AIC31XX_PLLDMSB
  readRegister(0x08);  // AIC31XX_PLLDLSB
  readRegister(0x0B);  // AIC31XX_NDAC
  readRegister(0x0C);  // AIC31XX_MDAC
  readRegister(0x0D);  // AIC31XX_DOSRMSB
  readRegister(0x0E);  // AIC31XX_DOSRLSB
  readRegister(0x10);  // AIC31XX_MINI_DSP_INPOL
  readRegister(0x12);  // AIC31XX_NADC
  readRegister(0x13);  // AIC31XX_MADC
  readRegister(0x14);  // AIC31XX_AOSR
  readRegister(0x19);  // AIC31XX_CLKOUTMUX
  readRegister(0x1A);  // AIC31XX_CLKOUTMVAL
  readRegister(0x1B);  // AIC31XX_IFACE1
  readRegister(0x1C);  // AIC31XX_DATA_OFFSET
  readRegister(0x1D);  // AIC31XX_IFACE2
  readRegister(0x1E);  // AIC31XX_BCLKN
  readRegister(0x1F);  // AIC31XX_IFACESEC1
  readRegister(0x20);  // AIC31XX_IFACESEC2
  readRegister(0x21);  // AIC31XX_IFACESEC3
  readRegister(0x22);  // AIC31XX_I2C
  readRegister(0x24);  // AIC31XX_ADCFLAG
  readRegister(0x25);  // AIC31XX_DACFLAG1
  readRegister(0x26);  // AIC31XX_DACFLAG2
  readRegister(0x27);  // AIC31XX_OFFLAG
  readRegister(0x2C);  // AIC31XX_INTRDACFLAG
  readRegister(0x2D);  // AIC31XX_INTRADCFLAG
  readRegister(0x2E);  // AIC31XX_INTRDACFLAG2
  readRegister(0x2F);  // AIC31XX_INTRADCFLAG2
  readRegister(0x30);  // AIC31XX_INT1CTRL
  readRegister(0x31);  // AIC31XX_INT2CTRL
  readRegister(0x33);  // AIC31XX_GPIO1
  readRegister(0x3C);  // AIC31XX_DACPRB
  readRegister(0x3D);  // AIC31XX_ADCPRB
  readRegister(0x3F);  // AIC31XX_DACSETUP
  readRegister(0x40);  // AIC31XX_DACMUTE
  readRegister(0x41);  // AIC31XX_LDACVOL
  readRegister(0x42);  // AIC31XX_RDACVOL
  readRegister(0x43);  // AIC31XX_HSDETECT
  readRegister(0x51);  // AIC31XX_ADCSETUP
  readRegister(0x52);  // AIC31XX_ADCFGA
  readRegister(0x53);  // AIC31XX_ADCVOL

  setPage(1);
  readRegister(0x1F);  // AIC31XX_HPDRIVER
  readRegister(0x20);  // AIC31XX_SPKAMP
  readRegister(0x21);  // AIC31XX_HPPOP
  readRegister(0x22);  // AIC31XX_SPPGARAMP
  readRegister(0x23);  // AIC31XX_DACMIXERROUTE
  readRegister(0x24);  // AIC31XX_LANALOGHPL
  readRegister(0x25);  // AIC31XX_RANALOGHPR
  readRegister(0x26);  // AIC31XX_LANALOGSPL
  readRegister(0x27);  // AIC31XX_RANALOGSPR
  readRegister(0x28);  // AIC31XX_HPLGAIN
  readRegister(0x29);  // AIC31XX_HPRGAIN
  readRegister(0x2A);  // AIC31XX_SPLGAIN
  readRegister(0x2B);  // AIC31XX_SPRGAIN
  readRegister(0x2C);  // AIC31XX_HPCONTROL
  readRegister(0x2E);  // AIC31XX_MICBIAS
  readRegister(0x2F);  // AIC31XX_MICPGA
  readRegister(0x30);  // AIC31XX_MICPGAPI
  readRegister(0x31);  // AIC31XX_MICPGAMI
  readRegister(0x32);  // AIC31XX_MICPGACM

  setPage(3);
  readRegister(0x10);  // AIC31XX_TIMERDIVIDER
 }
 
    const struct audio_format *output_format = audio_i2s_setup(&audio_format, &config);
    assert(output_format);
    if (!output_format) {
        panic("PicoAudio: Unable to open audio device.\n");
    }
    assert(producer_pool);
    bool ok = audio_i2s_connect(producer_pool);
    assert(ok); 
    audio_i2s_set_enabled(true);
}

static void i2s_audio_handle_buffer(void) {
    audio_buffer *buffer = take_audio_buffer(producer_pool, true);
    fillsamples(reinterpret_cast<audio_sample*>(buffer->buffer->bytes), buffer->max_sample_count);
    buffer->sample_count = buffer->max_sample_count;
    give_audio_buffer(producer_pool, buffer);
}

static void core1_func_tft() {
  i2s_audio_init();
    while (true) {
        if (producer_pool && fillsamples) i2s_audio_handle_buffer();
        __dmb();
    }
}

void PICO_DSP::begin_audio(int samplesize, void (*callback)(short * stream, int len))
{
  if (!callback) return;

  producer_pool = audio_new_producer_pool(&producer_format, 3, samplesize);
  fillsamples = callback;
  multicore_launch_core1(core1_func_tft);
}

void PICO_DSP::end_audio()
{
}

void * PICO_DSP::get_buffer_audio(void)
{
    return NULL; // not implemented
}

#endif

 

