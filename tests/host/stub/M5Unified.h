#pragma once
#include <Arduino.h>
#include "lgfx_fonts.h"
#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
namespace lgfx { inline namespace v1 {
  enum textdatum_t : uint8_t { top_left = 0, top_center = 1, top_centre = 1, top_right = 2, middle_left = 4, middle_center = 5, middle_centre = 5, middle_right = 6,
                               bottom_left = 8, bottom_center = 9, bottom_centre = 9, bottom_right = 10, baseline_left = 16, baseline_center = 17, baseline_centre = 17, baseline_right = 18 };
} }
using lgfx::textdatum_t;
using textdatum_t::top_left; using textdatum_t::top_center; using textdatum_t::top_right; using textdatum_t::middle_left; using textdatum_t::middle_center; using textdatum_t::middle_right;
using textdatum_t::bottom_left; using textdatum_t::bottom_center; using textdatum_t::bottom_right; using textdatum_t::baseline_left; using textdatum_t::baseline_center; using textdatum_t::baseline_right;

// The drawing surface: a 16-bit framebuffer with the LovyanGFX primitives the firmware uses, and text
// drawn with the real M5GFX glyph data (stub/fonts) so screens render as they will on the device.
struct LGFXBase {
  uint16_t* fb = nullptr; int w = 0, h = 0;
  uint16_t fg = 0xFFFF, bg = 0; bool hasBg = false; int cx = 0, cy = 0; float tsx = 1, tsy = 1;
  textdatum_t datum = top_left;
  int clipL = 0, clipT = 0, clipR = 0x7fff, clipB = 0x7fff;
  const lgfx::IFont* curFont = nullptr; lgfx::FontMetrics fm;   // set on first use: fonts live in another translation unit
  LGFXBase() {}
  const lgfx::IFont* font() { if (!curFont) setFont(&fonts::Font0); return curFont; }
  static constexpr uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
  void setRotation(int){} void setBrightness(uint8_t){}
  void setClipRect(int x, int y, int ww, int hh){ clipL = x; clipT = y; clipR = x + ww - 1; clipB = y + hh - 1; }
  void clearClipRect(){ clipL = 0; clipT = 0; clipR = 0x7fff; clipB = 0x7fff; }
  void pix(int x,int y,uint16_t c){ if(fb && x>=0&&y>=0&&x<w&&y<h && x>=clipL&&x<=clipR&&y>=clipT&&y<=clipB) fb[y*w+x]=c; }
  void fillScreen(uint32_t c){ fillRect(0,0,w,h,c); }
  void fillRect(int x,int y,int ww,int hh,uint32_t c){ for(int j=y;j<y+hh;j++) for(int i=x;i<x+ww;i++) pix(i,j,c); }
  void drawRect(int x,int y,int ww,int hh,uint32_t c){ drawFastHLine(x,y,ww,c); drawFastHLine(x,y+hh-1,ww,c); drawFastVLine(x,y,hh,c); drawFastVLine(x+ww-1,y,hh,c); }
  void drawPixel(int x,int y,uint32_t c){ pix(x,y,c); }
  void drawFastHLine(int x,int y,int ww,uint32_t c){ for(int i=0;i<ww;i++) pix(x+i,y,c); }
  void drawFastVLine(int x,int y,int hh,uint32_t c){ for(int j=0;j<hh;j++) pix(x,y+j,c); }
  void drawLine(int x0,int y0,int x1,int y1,uint32_t c){
    int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy;
    for(;;){ pix(x0,y0,c); if(x0==x1&&y0==y1) break; int e2=2*err; if(e2>=dy){ err+=dy; x0+=sx; } if(e2<=dx){ err+=dx; y0+=sy; } } }
  void drawCircle(int x,int y,int r,uint32_t c){ for(int j=-r;j<=r;j++) for(int i=-r;i<=r;i++){ int d=i*i+j*j; if(d<=r*r+r && d>r*r-r) pix(x+i,y+j,c);} }
  void fillCircle(int x,int y,int r,uint32_t c){ for(int j=-r;j<=r;j++) for(int i=-r;i<=r;i++) if(i*i+j*j<=r*r+r) pix(x+i,y+j,c); }
  void drawEllipse(int x,int y,int rx,int ry,uint32_t c){ for(int j=-ry;j<=ry;j++) for(int i=-rx;i<=rx;i++){ float d=(float)i*i/((float)rx*rx)+(float)j*j/((float)ry*ry); if(d<=1.0f && d>0.72f) pix(x+i,y+j,c);} }
  void fillEllipse(int x,int y,int rx,int ry,uint32_t c){ for(int j=-ry;j<=ry;j++) for(int i=-rx;i<=rx;i++){ float d=(float)i*i/((float)rx*rx)+(float)j*j/((float)ry*ry); if(d<=1.0f) pix(x+i,y+j,c);} }
  void drawRoundRect(int x,int y,int ww,int hh,int r,uint32_t c){
    drawFastHLine(x+r,y,ww-2*r,c); drawFastHLine(x+r,y+hh-1,ww-2*r,c); drawFastVLine(x,y+r,hh-2*r,c); drawFastVLine(x+ww-1,y+r,hh-2*r,c);
    int f=1-r, dx=1, dy=-2*r, xx=0, yy=r;   // midpoint circle, four corners
    while(xx<yy){ if(f>=0){ yy--; dy+=2; f+=dy; } xx++; dx+=2; f+=dx;
      pix(x+ww-1-r+xx,y+hh-1-r+yy,c); pix(x+ww-1-r+yy,y+hh-1-r+xx,c); pix(x+r-xx,y+hh-1-r+yy,c); pix(x+r-yy,y+hh-1-r+xx,c);
      pix(x+ww-1-r+xx,y+r-yy,c); pix(x+ww-1-r+yy,y+r-xx,c); pix(x+r-xx,y+r-yy,c); pix(x+r-yy,y+r-xx,c); } }
  void fillRoundRect(int x,int y,int ww,int hh,int r,uint32_t c){
    if (r <= 0) { fillRect(x,y,ww,hh,c); return; }
    for(int j=0;j<hh;j++){ int inset=0; int dy = j<r ? r-j : j>=hh-r ? j-(hh-r-1) : 0;
      if(dy){ int q=r*r-dy*dy+dy; int k=0; while((k+1)*(k+1)<=q) k++; inset=r-k; }
      drawFastHLine(x+inset,y+j,ww-2*inset,c); } }
  void drawTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint32_t c){ drawLine(x0,y0,x1,y1,c); drawLine(x1,y1,x2,y2,c); drawLine(x2,y2,x0,y0,c); }
  void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint32_t c){
    int miny=min(y0,min(y1,y2)), maxy=max(y0,max(y1,y2)), minx=min(x0,min(x1,x2)), maxx=max(x0,max(x1,x2));
    auto edge=[](int ax,int ay,int bx,int by,int px,int py){ return (bx-ax)*(py-ay)-(by-ay)*(px-ax); };
    for(int y=miny;y<=maxy;y++) for(int x=minx;x<=maxx;x++){ int a=edge(x0,y0,x1,y1,x,y), b=edge(x1,y1,x2,y2,x,y), d=edge(x2,y2,x0,y0,x,y);
      if((a>=0&&b>=0&&d>=0)||(a<=0&&b<=0&&d<=0)) pix(x,y,c); } }
  // Arcs: degrees, 0 at 3 o'clock, clockwise on screen (LovyanGFX's convention); r0 inner, r1 outer.
  void fillArc(int x,int y,int r0,int r1,float a0,float a1,uint32_t c){
    if(r0>r1) std::swap(r0,r1);
    float span=a1-a0; if(span<0) span+=360;
    for(int j=-r1;j<=r1;j++) for(int i=-r1;i<=r1;i++){ float d=sqrtf((float)i*i+(float)j*j); if(d>r1+0.5f || d<r0-0.5f) continue;
      float ang=atan2f((float)j,(float)i)*57.29578f; if(ang<0) ang+=360; float rel=ang-a0; if(rel<0) rel+=360; if(rel<=span || span>=360) pix(x+i,y+j,c);} }
  void drawArc(int x,int y,int r0,int r1,float a0,float a1,uint32_t c){ fillArc(x,y,r0,r1,a0,a1,c); }
  // ---- text
  void setTextColor(uint32_t f){ fg=f; hasBg=false; } void setTextColor(uint32_t f,uint32_t b){ fg=f; bg=b; hasBg=(f!=b); }
  void setFont(const lgfx::IFont* f){ if(!f) f=&fonts::Font0; curFont=f; f->getDefaultMetric(&fm); }
  void setTextSize(float s){ setTextSize(s,s); } void setTextSize(float sx,float sy){ tsx=sx>0?sx:1; tsy=sy>0?sy:1; }
  void setTextDatum(uint8_t d){ datum=(textdatum_t)d; } void setTextDatum(textdatum_t d){ datum=d; }
  void setTextWrap(bool, bool = false){}
  void setCursor(int32_t x,int32_t y){cx=x;cy=y;}
  int32_t fontHeight() { font(); return (int32_t)(fm.height*tsy); }
  int32_t fontHeight(const lgfx::IFont* f) const { lgfx::FontMetrics m; f->getDefaultMetric(&m); return (int32_t)(m.height*tsy); }
  int32_t textWidth(const char* s){ return textWidth(s, font()); }
  int32_t textWidth(const char* s, const lgfx::IFont* f);
  int32_t textWidth(const String& s){ return textWidth(s.c_str()); }
  size_t drawString(const char* s,int32_t x,int32_t y){ return drawStr(s,x,y,datum,font()); }
  size_t drawString(const char* s,int32_t x,int32_t y,const lgfx::IFont* f){ return drawStr(s,x,y,datum,f); }
  size_t drawString(const String& s,int32_t x,int32_t y){ return drawStr(s.c_str(),x,y,datum,font()); }
  size_t drawString(const String& s,int32_t x,int32_t y,const lgfx::IFont* f){ return drawStr(s.c_str(),x,y,datum,f); }
  size_t drawCenterString(const char* s,int32_t x,int32_t y){ return drawStr(s,x,y,top_center,font()); }
  size_t drawCenterString(const String& s,int32_t x,int32_t y){ return drawStr(s.c_str(),x,y,top_center,font()); }
  size_t drawRightString(const char* s,int32_t x,int32_t y){ return drawStr(s,x,y,top_right,font()); }
  size_t drawRightString(const String& s,int32_t x,int32_t y){ return drawStr(s.c_str(),x,y,top_right,font()); }
  size_t drawStr(const char* s,int32_t x,int32_t y,textdatum_t d,const lgfx::IFont* f);
  void print(char c);
  void print(const char* s){ while(*s) print(*s++); }
  void print(const String& s){ print(s.c_str()); }
};
struct M5GFX : LGFXBase {};
struct M5Canvas : LGFXBase { M5Canvas(LGFXBase*){} void* createSprite(int ww,int hh){ w=ww;h=hh; fb=(uint16_t*)calloc(ww*hh,2); return fb; }
  void setColorDepth(int){} void setPsram(bool){} void pushSprite(int,int); };
struct Button_Class { bool c=false,hd=false; bool isPressed(){return false;} bool wasClicked(){return c;} bool wasHold(){return hd;} void setHoldThresh(uint16_t){} };
extern int BATT_PCT; extern bool BATT_CHARGING;   // tests set these
namespace m5 { struct Power_Class { enum is_charging_t { is_discharging=0, is_charging=1, charge_unknown=2 };
 int32_t getBatteryLevel(){return BATT_PCT;} int16_t getBatteryVoltage(){return 4012;} is_charging_t isCharging(){return BATT_CHARGING ? is_charging : is_discharging;} }; }
extern int TONES;
struct Speaker_Class { void end(); bool begin(){return true;} bool tone(float, uint32_t){ TONES++; return true; } void setVolume(uint8_t){} };
struct Mic_Class { bool begin(); void end(); bool record(int16_t*, size_t, uint32_t); size_t isRecording(); };
extern float IMU_ACC[3];   // tests set the accelerometer reading
struct IMU_Class { bool getAccel(float*a,float*b,float*c){*a=IMU_ACC[0];*b=IMU_ACC[1];*c=IMU_ACC[2];return true;} };
struct M5Config { uint32_t serial_baudrate; };
struct M5Unified { M5GFX Display; Button_Class BtnA, BtnB, BtnPWR; IMU_Class Imu; Mic_Class Mic; m5::Power_Class Power; Speaker_Class Speaker;
 M5Config config(){return {};} void begin(M5Config){} void update(){} };
extern M5Unified M5;
