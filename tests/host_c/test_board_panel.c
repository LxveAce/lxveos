// Host test for the pure board-layer panel helper (components/lxveos_board/src/lxveos_board_panel.c): the
// classic-CYD RDID4 0xD3 -> driver-name decision, off-target with no ESP-IDF. Guards the ILI9341 (00 93 41)
// vs ST7789 split that create_panel() reports to bsp_display_panel()/the CC status line — a regression here
// would mislabel the panel driver on a real CYD.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lxveos_board_panel.h"

int main(void)
{
    const uint8_t ili[3]     = {0x00, 0x93, 0x41};   // 1-USB CYD: ILI9341 RDID4
    const uint8_t st789[3]   = {0x00, 0x00, 0x00};   // unconnected/other id -> ST7789 (2-USB CYD)
    const uint8_t other[3]   = {0x85, 0x85, 0x52};   // a different non-ILI9341 id still resolves ST7789
    const uint8_t partial[3] = {0x00, 0x93, 0x00};   // only two ILI9341 bytes match -> NOT ILI9341

    assert(strcmp(lxveos_panel_from_probe_d3(ili), "ILI9341") == 0);
    assert(strcmp(lxveos_panel_from_probe_d3(st789), "ST7789") == 0);
    assert(strcmp(lxveos_panel_from_probe_d3(other), "ST7789") == 0);
    assert(strcmp(lxveos_panel_from_probe_d3(partial), "ST7789") == 0);

    printf("board panel host tests: OK\n");
    return 0;
}
