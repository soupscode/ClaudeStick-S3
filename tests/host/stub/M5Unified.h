#pragma once
#include <Arduino.h>
namespace fonts { struct IFont{ const uint8_t* tbl = nullptr; }; extern const IFont Font0, Font2, Font4; }
namespace lgfx { using IFont = fonts::IFont; struct GLCDfont : fonts::IFont { GLCDfont(const void* t, const uint8_t*, uint8_t, uint8_t, uint8_t){ tbl=(const uint8_t*)t; } }; }
#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
struct LGFXBase {
  uint16_t* fb = nullptr; int w = 0, h = 0;
  uint16_t fg = 0xFFFF, bg = 0; bool hasBg = false; int cx = 0, cy = 0; int ts = 1;
  void setRotation(int){} void setBrightness(uint8_t){} void print(const char* s){ while(*s) print(*s++); }
  void pix(int x,int y,uint16_t c){ if(fb && x>=0&&y>=0&&x<w&&y<h) fb[y*w+x]=c; }
  void fillScreen(uint32_t c){ fillRect(0,0,w,h,c); }
  void fillRect(int x,int y,int ww,int hh,uint32_t c){ for(int j=y;j<y+hh;j++) for(int i=x;i<x+ww;i++) pix(i,j,c); }
  void drawRect(int x,int y,int ww,int hh,uint32_t c){ drawFastHLine(x,y,ww,c); drawFastHLine(x,y+hh-1,ww,c); drawFastVLine(x,y,hh,c); drawFastVLine(x+ww-1,y,hh,c); }
  void drawPixel(int x,int y,uint32_t c){ pix(x,y,c); }
  void drawRoundRect(int x,int y,int ww,int hh,int r,uint32_t c){
    drawFastHLine(x+r,y,ww-2*r,c); drawFastHLine(x+r,y+hh-1,ww-2*r,c); drawFastVLine(x,y+r,hh-2*r,c); drawFastVLine(x+ww-1,y+r,hh-2*r,c);
    int f=1-r, dx=1, dy=-2*r, xx=0, yy=r;   // midpoint circle, four corners
    while(xx<yy){ if(f>=0){ yy--; dy+=2; f+=dy; } xx++; dx+=2; f+=dx;
      pix(x+ww-1-r+xx,y+hh-1-r+yy,c); pix(x+ww-1-r+yy,y+hh-1-r+xx,c); pix(x+r-xx,y+hh-1-r+yy,c); pix(x+r-yy,y+hh-1-r+xx,c);
      pix(x+ww-1-r+xx,y+r-yy,c); pix(x+ww-1-r+yy,y+r-xx,c); pix(x+r-xx,y+r-yy,c); pix(x+r-yy,y+r-xx,c); } }
  void drawFastHLine(int x,int y,int ww,uint32_t c){ for(int i=0;i<ww;i++) pix(x+i,y,c); }
  void drawFastVLine(int x,int y,int hh,uint32_t c){ for(int j=0;j<hh;j++) pix(x,y+j,c); }
  void setTextColor(uint32_t f){ fg=f; hasBg=false; } void setTextColor(uint32_t f,uint32_t b){ fg=f; bg=b; hasBg=true; }
  const fonts::IFont* curFont=nullptr; void setFont(const fonts::IFont* f){ curFont=f; } void setTextSize(float s){ ts=(int)s; } void setCursor(int32_t x,int32_t y){cx=x;cy=y;}
  void glyph(int x,int y,unsigned char ch);
  void print(char c){ glyph(cx,cy,(unsigned char)c); cx+=6*ts; }
  size_t drawString(const char* s,int32_t x,int32_t y){ int ox=x; while(*s){ glyph(x,y,(unsigned char)*s++); x+=6*ts; } return x-ox; }
  size_t drawString(const String& s,int32_t x,int32_t y){ return drawString(s.c_str(),x,y); }
  int32_t textWidth(const char* s){ return strlen(s)*6*ts; } int32_t textWidth(const String& s){ return textWidth(s.c_str()); }
};
struct M5GFX : LGFXBase {};
struct M5Canvas : LGFXBase { M5Canvas(LGFXBase*){} void* createSprite(int ww,int hh){ w=ww;h=hh; fb=(uint16_t*)calloc(ww*hh,2); return fb; }
  void setColorDepth(int){} void setPsram(bool){} void pushSprite(int,int); };
struct Button_Class { bool c=false,hd=false; bool isPressed(){return false;} bool wasClicked(){return c;} bool wasHold(){return hd;} void setHoldThresh(uint16_t){} };
namespace m5 { struct Power_Class { enum is_charging_t { is_discharging=0, is_charging=1, charge_unknown=2 };
 int32_t getBatteryLevel(){return 87;} int16_t getBatteryVoltage(){return 4012;} is_charging_t isCharging(){return is_discharging;} }; }
struct Speaker_Class { void end(); bool begin(){return true;} bool tone(float, uint32_t){return true;} void setVolume(uint8_t){} };
struct Mic_Class { bool begin(); void end(); bool record(int16_t*, size_t, uint32_t); size_t isRecording(); };
struct IMU_Class { bool getAccel(float*a,float*b,float*c){*a=*b=0;*c=1;return true;} };
struct M5Config { uint32_t serial_baudrate; };
struct M5Unified { M5GFX Display; Button_Class BtnA, BtnB, BtnPWR; IMU_Class Imu; Mic_Class Mic; m5::Power_Class Power; Speaker_Class Speaker;
 M5Config config(){return {};} void begin(M5Config){} void update(){} };
extern M5Unified M5;
