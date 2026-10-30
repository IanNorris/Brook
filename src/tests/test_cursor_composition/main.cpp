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
    source={};source.width=12;source.height=16;source.hotX=3;source.hotY=5;
    source.pixels[2*CursorCapacity+1]=0x80776655;
    ScaleCursor(source,restored,200);
    CHECK(restored.width==24 && restored.height==32 && restored.hotX==6 && restored.hotY==10);
    CHECK(restored.pixels[4*CursorCapacity+2]==0x80776655);
    CHECK(restored.pixels[5*CursorCapacity+3]==0x80776655);
    ScaleCursor(source,restored,150);
    CHECK(restored.width==18 && restored.height==24 && restored.hotX==4 && restored.hotY==7);
    ScaleCursor(source,restored,100);CHECK(std::memcmp(&source,&restored,sizeof(source))==0);
    source.width=source.height=CursorCapacity;
    ScaleCursor(source,bounds.cursor,200);
    CHECK(bounds.cursor.width==CursorCapacity && bounds.cursor.height==CursorCapacity);
    CHECK(bounds.before==0x12345678 && bounds.after==0x87654321);
    std::puts("cursor composition: source preservation, animation, hotspot and bounds PASS");
}
