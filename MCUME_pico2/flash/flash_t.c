/*
  RP2350 flash driver
*/

#include <string.h>

#include "flash_t.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "emuapi.h"

extern size_t _psram_size;

static bool using_psram;

#define PSRAM_BASE (0x11000000)
// #define PSRAM_BASE (0x15000000) // there's no reason to use uncached access

unsigned char * flash_start = (unsigned char *)(XIP_BASE + HW_FLASH_STORAGE_BASE);
unsigned char * flash_end = (unsigned char *)(XIP_BASE + HW_FLASH_STORAGE_TOP);

static uint8_t cache[FLASH_SECTOR_SIZE];

static void flash_setup() {
    if (_psram_size) {
        using_psram = true;
        flash_start = (unsigned char*)PSRAM_BASE;
        flash_end = flash_start + _psram_size;
    } else {
        using_psram = false;
    }
}

int flash_load_common(const char * filename, bool do_bswap)
{
  flash_setup();

  uint8_t *dest = flash_start;
  int n;
  int size = 0;
  emu_printf("flash_load...");
  int f = emu_FileOpen(filename,"r+b");
  if (f) {
    while ( (dest < (flash_end-FLASH_SECTOR_SIZE)) && (n = emu_FileRead(cache,FLASH_SECTOR_SIZE,f) ) ) {
      if (do_bswap) {
          for (int i=0;i<n; i+=2) {
              uint8_t k = cache[i];
              cache[i]=cache[i+1];
              cache[i+1] = k;
          }
      }
      if (using_psram) {
          memcpy(dest, cache, n);
      } else if (memcmp(cache, dest, n)) {
          uint32_t ints = save_and_disable_interrupts();
          uint32_t offset = dest - (uint8_t*)XIP_BASE;
          flash_range_erase(offset, FLASH_SECTOR_SIZE);
          flash_range_program(offset, (const uint8_t *)&cache[0], FLASH_SECTOR_SIZE);
          restore_interrupts(ints);
          emu_printi(n);
          emu_printi(offset);
          //uint8_t * pt = (uint8_t*)(XIP_BASE + offset);
          //emu_printi(pt[0]);
          //emu_printi(pt[1]);
          //emu_printi(pt[2]);
          //emu_printi(pt[3]);
      }
      dest += FLASH_SECTOR_SIZE;
      size += n;          
    }
    emu_FileClose(f);
    emu_printf("flash_load OK.");    
  }

  return size;
}

int flash_load(const char * filename)
{
    return flash_load_common(filename, false);
}

int flash_load_bswap(const char * filename)
{
    return flash_load_common(filename, true);
}

int flash_verify(unsigned char * buf, int size)
{
  flash_setup();
  unsigned char * datapt = flash_start;
  emu_printf("flash_verify...");
  int count = size;
  while (count++ < size) {
    if (*datapt != *buf) {
      emu_printf("mismatch at ");
      emu_printi(count);
      emu_printi(*datapt);
      emu_printi(*buf);
      return 1;
    }  
    datapt++;
    buf++; 
  }  
  emu_printf("flash_verify OK.");
  return 0;
}
