#include <M5Unified.h>
#include "glcdfont.h"
void LGFXBase::glyph(int x,int y,unsigned char ch){
  for(int col=0; col<6; col++){
    const uint8_t* tbl = (curFont && curFont->tbl) ? curFont->tbl : font;
    uint8_t bits = col<5 ? tbl[ch*5+col] : 0;
    for(int row=0; row<8; row++){
      bool on = bits & (1<<row);
      for(int sy=0; sy<ts; sy++) for(int sx=0; sx<ts; sx++){
        if(on) pix(x+col*ts+sx, y+row*ts+sy, fg); else if(hasBg && bg!=fg) pix(x+col*ts+sx, y+row*ts+sy, bg);
      }
    }
  }
}
