#include "PWR_Key.h"

// The board uses a soft-power latch: GPIO7 (PWR_Control) high keeps VBAT connected,
// low cuts it. GPIO6 (PWR key) is an active-low push button that also boots the board.
//
// The game owns all power semantics now (see game/engine/power.cpp): short press ->
// light sleep, hold -> deep sleep, long hold -> off. So this driver only sets up the
// pins (latch asserted, key input) and exposes the raw button read + Shutdown(). The
// old long-press state machine here drove nothing (Device_State was never consumed and
// Fall_Asleep/Restart were empty), so it has been removed.

bool PWR_Key_Down(void)
{
    return !gpio_get_level(PWR_KEY_Input_PIN);   // active-low: pressed == 0
}

void Shutdown(void)
{
    gpio_set_level(PWR_Control_PIN, false);      // drop the latch -> power off (battery)
    LCD_Backlight = 0;
    Set_Backlight(0);
}

void PWR_Init(void)
{
    // Assert the latch immediately so the board stays powered for the whole session
    // regardless of how it booted (button, USB, or a deep-sleep timer wake).
    gpio_reset_pin(PWR_Control_PIN);
    gpio_set_direction(PWR_Control_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PWR_Control_PIN, true);

    // Key input. An internal pull-up backstops the external one so the idle level is
    // well defined (and the pin reads/wakes cleanly on press).
    gpio_reset_pin(PWR_KEY_Input_PIN);
    gpio_set_direction(PWR_KEY_Input_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PWR_KEY_Input_PIN, GPIO_PULLUP_ONLY);
}
