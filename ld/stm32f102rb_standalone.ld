/* Linker script for Akai APC Mini (STM32F102RBT6: 128 KB Flash, 16 KB SRAM)
 * Standalone layout for direct SWD flashing without bootloader (BOOTLOADER=0).
 */

MEMORY
{
    rom (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

PROVIDE(_boot_magic_page = 0x0801FC00);
PROVIDE(_crash_log_page = 0x0801F800);
PROVIDE(_lua_flash_page = 0x0801E000);
PROVIDE(_config_flash_page = 0x0801F000);

INCLUDE cortex-m-generic.ld
