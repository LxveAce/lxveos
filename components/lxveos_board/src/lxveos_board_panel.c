// Pure board-layer panel helpers — see lxveos_board_panel.h. No ESP-IDF deps so it host-tests off-target.
#include "lxveos_board_panel.h"

const char *lxveos_panel_from_probe_d3(const uint8_t d3[3])
{
    // ILI9341 RDID4 answers 00 93 41; the 2-USB CYD's ST7789 answers with anything else. The caller has
    // already confirmed a panel is physically present via the RDDID / bring-up read, so this is only the
    // A-vs-B split, never a "no panel" case.
    if (d3[0] == 0x00 && d3[1] == 0x93 && d3[2] == 0x41) {
        return "ILI9341";
    }
    return "ST7789";
}
