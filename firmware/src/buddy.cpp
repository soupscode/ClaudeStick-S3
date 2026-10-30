// Adapted from anthropics/claude-desktop-buddy src/buddy.cpp (MIT, Copyright 2026 Anthropic, PBC).
// Changes: render only into `spr`, fixed 1x scale, geometry moved to the left pane of a
// 240x135 landscape layout, species persistence handled by main.cpp.
#include "buddy.h"
#include "buddy_common.h"
#include <M5StickCPlus.h>
#include <string.h>

extern TFT_eSprite spr;

// Left pane is 86px wide; species particles reach about -42..+36px from center.
const int BUDDY_X_CENTER  = 43;
const int BUDDY_CANVAS_W  = 86;
const int BUDDY_Y_BASE    = 44;
const int BUDDY_Y_OVERLAY = 22;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;

const uint16_t BUDDY_BG     = 0x0000;
const uint16_t BUDDY_HEART  = 0xF810;
const uint16_t BUDDY_DIM    = 0x8410;
const uint16_t BUDDY_YEL    = 0xFFE0;
const uint16_t BUDDY_WHITE  = 0xFFFF;
const uint16_t BUDDY_CYAN   = 0x07FF;
const uint16_t BUDDY_GREEN  = 0x07E0;
const uint16_t BUDDY_PURPLE = 0xA01F;
const uint16_t BUDDY_RED    = 0xF800;
const uint16_t BUDDY_BLUE   = 0x041F;

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int len = strlen(line);
  int w = len * BUDDY_CHAR_W;
  spr.setTextColor(color, BUDDY_BG);
  spr.setCursor(BUDDY_X_CENTER - w / 2 + xOff, yPx);
  for (int i = 0; i < len; i++) spr.print(line[i]);
}

void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff) {
  spr.setTextSize(1);
  for (uint8_t i = 0; i < nLines; i++)
    buddyPrintLine(lines[i], BUDDY_Y_BASE + yOffset + i * BUDDY_CHAR_H, color, xOff);
}

void buddySetCursor(int x, int y) { spr.setCursor(x, y); }
void buddySetColor(uint16_t fg)   { spr.setTextColor(fg, BUDDY_BG); }
void buddyPrint(const char* s)    { spr.setTextSize(1); spr.print(s); }

extern const Species CAPYBARA_SPECIES, DUCK_SPECIES, GOOSE_SPECIES, BLOB_SPECIES, CAT_SPECIES,
  DRAGON_SPECIES, OCTOPUS_SPECIES, OWL_SPECIES, PENGUIN_SPECIES, TURTLE_SPECIES, SNAIL_SPECIES,
  GHOST_SPECIES, AXOLOTL_SPECIES, CACTUS_SPECIES, ROBOT_SPECIES, RABBIT_SPECIES, MUSHROOM_SPECIES,
  CHONK_SPECIES;

static const Species* SPECIES_TABLE[] = {
  &CAPYBARA_SPECIES, &DUCK_SPECIES, &GOOSE_SPECIES, &BLOB_SPECIES, &CAT_SPECIES, &DRAGON_SPECIES,
  &OCTOPUS_SPECIES, &OWL_SPECIES, &PENGUIN_SPECIES, &TURTLE_SPECIES, &SNAIL_SPECIES, &GHOST_SPECIES,
  &AXOLOTL_SPECIES, &CACTUS_SPECIES, &ROBOT_SPECIES, &RABBIT_SPECIES, &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const uint8_t N_SPECIES = sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);
static uint8_t  current = 0;
static uint32_t tickCount = 0, nextTickAt = 0;
static const uint32_t TICK_MS = 200;

void buddySetSpeciesIdx(uint8_t idx) { if (idx < N_SPECIES) current = idx; }
uint8_t buddyNextSpecies() { current = (current + 1) % N_SPECIES; return current; }
uint8_t buddySpeciesIdx() { return current; }
uint8_t buddySpeciesCount() { return N_SPECIES; }
const char* buddySpeciesName() { return SPECIES_TABLE[current]->name; }

void buddyDraw(uint8_t state) {
  uint32_t now = millis();
  if ((int32_t)(now - nextTickAt) >= 0) { nextTickAt = now + TICK_MS; tickCount++; }
  if (state > B_HEART) state = B_IDLE;
  const Species* sp = SPECIES_TABLE[current];
  if (sp->states[state]) sp->states[state](tickCount);
}
