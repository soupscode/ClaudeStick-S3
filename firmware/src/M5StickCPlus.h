// Compatibility shim so the ASCII buddy species from anthropics/claude-desktop-buddy
// (written against M5StickCPlus / TFT_eSPI) render onto an M5Unified M5Canvas.
#pragma once
#include <M5Unified.h>
using TFT_eSprite = M5Canvas;
