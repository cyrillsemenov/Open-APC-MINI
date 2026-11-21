/* Linker script for Akai APC Mini (STM32F102RBT6: 128 KB Flash, 16 KB SRAM)
 * Application region: 0x08002000 .. 0x080077FD (22,526 bytes)
 * Checksum word:      0x080077FE .. 0x080077FF (2 bytes, 16-bit two's complement)
 * Bootloader flag:    0x08007C00 .. 0x08007FFF (1 KB Flash page)
 */

MEMORY
{
    rom (rx)  : ORIGIN = 0x08002000, LENGTH = 0x57FE
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

PROVIDE(_boot_magic_page = 0x08007C00);
PROVIDE(_crash_log_page = 0x08007800);
PROVIDE(_lua_flash_page = 0x08008000);
PROVIDE(_config_flash_page = 0x08009000);

INCLUDE cortex-m-generic.ld
