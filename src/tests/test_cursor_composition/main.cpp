#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cursor_composition.h"
using namespace brook;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
int main()
{
    CursorImage source, first, next, restored;
    source.width=24;source.height=32;source.hotX=7;source.hotY=9;
    source.pixels[0]=0x80776655;source.pixels[8*CursorCapacity+7]=0xff112233;
    CursorImage original=source;
    ComposeCursor(source,first,true,0,0xff00ddaa);
    ComposeCursor(source,next,true,100,0xff00ddaa);
    CHECK(first.width>source.width && first.height>=source.height);
    CHECK(first.hotX==7 && first.hotY==9);
    CHECK(first.pixels[0]==source.pixels[0]);
    CHECK(first.pixels[8*CursorCapacity+7]==source.pixels[8*CursorCapacity+7]);
    CHECK(std::memcmp(first.pixels,next.pixels,sizeof(first.pixels))!=0);
    CHECK(std::memcmp(&source,&original,sizeof(source))==0);
    ComposeCursor(source,restored,false,300,0);
    CHECK(std::memcmp(&source,&restored,sizeof(source))==0);
    struct Guarded {uint64_t before=0x12345678;CursorImage cursor;uint64_t after=0x87654321;} bounds;
    source.width=source.height=CursorCapacity;
    ComposeCursor(source,bounds.cursor,true,700,0xff00ddaa);
    CHECK(bounds.cursor.width==CursorCapacity && bounds.cursor.height==CursorCapacity);
    CHECK(bounds.before==0x12345678 && bounds.after==0x87654321);
    CHECK(bounds.cursor.hotX==source.hotX && bounds.cursor.hotY==source.hotY);
    CursorUploadState upload;unsigned calls=0;bool accept=false;
    auto submit=[&](uint32_t w,uint32_t h){++calls;CHECK(w==source.width && h==source.height);return accept;};
    CHECK(!upload.Update(source,false,submit) && !upload.valid && calls==1);
    accept=true;CHECK(upload.Update(source,false,submit) && upload.valid && calls==2);
    source.hotX=1;source.hotY=2;
    CHECK(upload.Update(source,false,submit) && calls==2); // hotspot is draw geometry
    source.width=12;source.height=16;CHECK(upload.Update(source,false,submit) && calls==3);
    source.width=source.height=CursorCapacity;CHECK(upload.Update(source,false,submit) && calls==4);
    source.pixels[0]^=1;accept=false;
    CHECK(!upload.Update(source,false,submit) && calls==5);
    CHECK(upload.accepted.pixels[0]!=source.pixels[0]);
    accept=true;CHECK(upload.Update(source,false,submit) && calls==6);
    CHECK(upload.Update(source,false,submit) && calls==6);
    upload.valid=false;CHECK(upload.Update(source,false,submit) && calls==7); // recreated texture
    CHECK(upload.Update(source,true,submit) && calls==8); // matched baseline control
    std::puts("cursor composition: source preservation, animation, hotspot and bounds PASS");
}
