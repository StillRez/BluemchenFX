/**
 * bootcheck — minimal test that the Daisy bootloader can start a BOOT_SRAM
 * program on this module. Blinks the Seed LED (500 ms) and shows a counter
 * on the Bluemchen OLED. No audio, SD, QSPI writes or SDRAM use.
 *
 * Build: make -C tools/bootcheck   Flash: make -C tools/bootcheck program-dfu
 */
#include "daisy_seed.h"
#include "dev/oled_ssd130x.h"
#include <cstdio>

using namespace daisy;

static DaisySeed                          hw_seed;
static OledDisplay<SSD130xI2c64x32Driver> display;

int main(void)
{
    hw_seed.Configure();
    hw_seed.Init(true);

    // 3 quick blinks: we're running from SRAM via the bootloader
    for(int i = 0; i < 3; i++)
    {
        hw_seed.SetLed(true);
        System::Delay(80);
        hw_seed.SetLed(false);
        System::Delay(80);
    }

    OledDisplay<SSD130xI2c64x32Driver>::Config cfg;
    cfg.driver_config.transport_config.Defaults();
    display.Init(cfg);

    uint32_t n = 0;
    char     buf[16];
    for(;;)
    {
        display.Fill(false);
        display.SetCursor(0, 0);
        display.WriteString("BOOT OK", Font_7x10, true);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)n++);
        display.SetCursor(0, 16);
        display.WriteString(buf, Font_7x10, true);
        display.Update();
        hw_seed.SetLed(n & 1);
        System::Delay(500);
    }
}
