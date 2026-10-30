// Adapted from anthropics/claude-desktop-buddy (MIT, Copyright 2026 Anthropic, PBC):
// trimmed to a single render target and landscape layout for ClaudeStick.
#pragma once
#include <stdint.h>

enum BuddyState : uint8_t { B_SLEEP, B_IDLE, B_BUSY, B_ATTENTION, B_CELEBRATE, B_DIZZY, B_HEART };

void        buddyDraw(uint8_t state);      // draws the current animation frame into `spr`
void        buddySetSpeciesIdx(uint8_t idx);
uint8_t     buddyNextSpecies();            // returns new index
uint8_t     buddySpeciesIdx();
uint8_t     buddySpeciesCount();
const char* buddySpeciesName();

typedef void (*StateFn)(uint32_t t);
struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];   // sleep, idle, busy, attention, celebrate, dizzy, heart
};
