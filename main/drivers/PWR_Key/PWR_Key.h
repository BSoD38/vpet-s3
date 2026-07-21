#pragma once
#include <stdbool.h>
#include "ST7789.h"

#define PWR_KEY_Input_PIN   6      // PWR button, active-low (pressed = 0)
#define PWR_Control_PIN     7      // power latch: high keeps the board powered, low cuts it

void Shutdown(void);               // release the power latch (battery: powers off) + panel dark

void PWR_Init(void);               // configure the latch (asserted) + key input
bool PWR_Key_Down(void);           // true while the PWR button is held (debounce is the caller's job)