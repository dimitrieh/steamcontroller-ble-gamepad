/*
 * Linker script for the Steam Controller BLE gamepad radio application.
 *
 * FLASH starts at 0x18000, the S110 8.0.0 application base, so the SoftDevice
 * occupying 0x0..0x16917 is untouched by an application flash.
 *
 * Sizes are for the part fitted to the Steam Controller: 16 kB RAM, and
 * probably 128 kB flash. Both were established from Valve's own radio image --
 * its application vector table declares an initial stack pointer of
 * 0x20003E28, which only makes sense if RAM ends at 0x20004000, and its
 * application runs to 0x1F014, just under a 128 kB ceiling. See
 * docs/FINDINGS.md.
 *
 * FLASH stops 2 kB short of 0x20000. pstorage keeps bonds in the last two
 * flash pages (data and swap, 1 kB each on nRF51), located at run time from
 * FICR CODESIZE, so on a 128 kB part they are 0x1F800..0x1FFFF and the image
 * must never reach them.
 *
 * S110 8.0.0 reserves the first 8 kB of RAM, leaving 0x20002000..0x20004000
 * for the application. The last 16 bytes of that are left out of the region:
 * main.c keeps a fault record there that survives a soft reset.
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x18000,    LENGTH = 0x7800
  RAM  (rwx) : ORIGIN = 0x20002000, LENGTH = 0x1FF0
}

INCLUDE "nrf5x_common.ld"
