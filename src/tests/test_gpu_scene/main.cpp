#include "../../kernel/src/gpu_scene.h"
#include "../../shared/surface_geometry.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <cstring>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"GPU_SCENE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
void TestSceneVersions();
void TestSceneService();
int main() {
    TestSceneVersions();
    TestSceneService();
    using namespace brook;
    CHECK(GpuViewportHalfBits(5)==0x40200000u);
    CHECK(GpuViewportHalfBits(511)==0x437f8000u);
    CHECK(GpuViewportHalfBits(512)==0x43800000u);
    CHECK(GpuViewportHalfBits(1280)==0x44200000u);
    CHECK(GpuViewportHalfBits(16384)==0x46000000u);
    for (uint32_t dimension=0;dimension<=GPU_SCENE_MAX_DIMENSION;++dimension) {
        const float half=float(dimension)/2;uint32_t bits;
        std::memcpy(&bits,&half,sizeof(bits));CHECK(GpuViewportHalfBits(dimension)==bits);
    }
    // Independently specified physical pixel order for a 3x2 asymmetric grid.
    // Each row is the transformed buffer read back into logical surface order.
    const unsigned expected[8][6]={
        {0,1,2,3,4,5},{3,0,4,1,5,2},{5,4,3,2,1,0},{2,5,1,4,0,3},
        {2,1,0,5,4,3},{0,3,1,4,2,5},{3,4,5,0,1,2},{5,2,4,1,3,0}};
    for (unsigned t=0;t<8;++t) {
        brook_geometry_state state{};state.transform=t;
        brook_geometry g{};CHECK(!brook_geometry_resolve(&state,3,2,&g));
        GpuSceneLayer layer{1,g.sx,g.sy,g.sw,g.sh,0,0,g.w,g.h,255,0,0,g.w,g.h,
            SCENE_LAYER_FIXED8|(t<<SCENE_LAYER_TRANSFORM_SHIFT)};
        GpuSceneRect rect{};CHECK(GpuSceneClip(layer,3,2,g.w,g.h,rect));
        auto a=GpuSceneCorner(rect,layer.flags,false,false),b=GpuSceneCorner(rect,layer.flags,true,false);
        auto c=GpuSceneCorner(rect,layer.flags,false,true);
        for (unsigned y=0;y<g.h;++y) for (unsigned x=0;x<g.w;++x) {
            uint32_t px,py;brook_geometry_pixel(&g,3,2,x,y,&px,&py);
            CHECK(py*3+px==expected[t][y*g.w+x]);
            double u=double(a.u)/a.uDen+(x+.5)/g.w*(double(b.u)/b.uDen-double(a.u)/a.uDen)
                +(y+.5)/g.h*(double(c.u)/c.uDen-double(a.u)/a.uDen);
            double v=double(a.v)/a.vDen+(x+.5)/g.w*(double(b.v)/b.vDen-double(a.v)/a.vDen)
                +(y+.5)/g.h*(double(c.v)/c.vDen-double(a.v)/a.vDen);
            CHECK(unsigned(u*3)==px && unsigned(v*2)==py);
        }
    }
    brook_geometry_state wine{};wine.source_set=1;wine.w=648*256;wine.h=394*256;
    brook_geometry geometry{};CHECK(!brook_geometry_resolve(&wine,768,512,&geometry));
    CHECK(geometry.w==648 && geometry.h==394 && geometry.sw==648*256);
    wine.x=128;wine.w=647*256+128;wine.destination_set=1;wine.dw=321;wine.dh=197;
    CHECK(!brook_geometry_resolve(&wine,768,512,&geometry));
    CHECK(geometry.sx==128 && geometry.sw==647*256+128 && geometry.w==321);
    wine.destination_set=0;CHECK(brook_geometry_resolve(&wine,768,512,&geometry)==BROOK_GEOMETRY_SIZE);
    wine.destination_set=1;wine.x=768*256;CHECK(brook_geometry_resolve(&wine,768,512,&geometry)==BROOK_GEOMETRY_BOUNDS);
    wine={};wine.scale=2;CHECK(!brook_geometry_resolve(&wine,768,512,&geometry));
    CHECK(geometry.w==384 && geometry.h==256);
    CHECK(brook_geometry_resolve(&wine,767,512,&geometry)==BROOK_GEOMETRY_BUFFER_SIZE);
    CHECK(!brook_geometry_resolve(&wine,0,0,&geometry));
    // Non-square buffer: scale two, fractional crop, reflected quarter turn,
    // destination scaling and negative clipping exercised together.
    brook_geometry_state combined{};combined.scale=2;combined.transform=5;
    combined.source_set=1;combined.x=64;combined.y=192;combined.w=384;combined.h=640;
    combined.destination_set=1;combined.dw=7;combined.dh=5;
    CHECK(!brook_geometry_resolve(&combined,8,4,&geometry));
    CHECK(geometry.sx==128 && geometry.sy==384 && geometry.sw==768 && geometry.sh==1280);
    GpuSceneLayer combinedLayer{1,geometry.sx,geometry.sy,geometry.sw,geometry.sh,-2,-1,7,5,255,0,0,6,5,
        SCENE_LAYER_FIXED8|(5<<SCENE_LAYER_TRANSFORM_SHIFT)};
    GpuSceneRect combinedRect{};CHECK(GpuSceneClip(combinedLayer,8,4,6,5,combinedRect));
    auto ca=GpuSceneCorner(combinedRect,combinedLayer.flags,false,false);
    auto cb=GpuSceneCorner(combinedRect,combinedLayer.flags,true,false);
    auto cc=GpuSceneCorner(combinedRect,combinedLayer.flags,false,true);
    for (unsigned y=0;y<4;++y) for (unsigned x=0;x<5;++x) {
        uint32_t px,py;brook_geometry_pixel(&geometry,8,4,x+2,y+1,&px,&py);
        const double fu=(x+.5)/(combinedRect.right-combinedRect.x),fv=(y+.5)/(combinedRect.bottom-combinedRect.y);
        const double u=double(ca.u)/ca.uDen+fu*(double(cb.u)/cb.uDen-double(ca.u)/ca.uDen)+fv*(double(cc.u)/cc.uDen-double(ca.u)/ca.uDen);
        const double v=double(ca.v)/ca.vDen+fu*(double(cb.v)/cb.vDen-double(ca.v)/ca.vDen)+fv*(double(cc.v)/cc.vDen-double(ca.v)/ca.vDen);
        CHECK(unsigned(u*8)==px && unsigned(v*4)==py);
    }
    // Maximum fixed-point clip used to overflow 32-bit UV numerators.
    GpuSceneLayer fixed{1,128,128,16383*256,16383*256,-17,-31,16384,16384,255,0,0,16384,16384,SCENE_LAYER_FIXED8};
    GpuSceneRect fixedRect{};CHECK(GpuSceneClip(fixed,16384,16384,16384,16384,fixedRect));
    CHECK(fixedRect.uDen==int64_t(16384)*16384*256 && fixedRect.u1>INT_MAX);
    fixed.flags=16;CHECK(!GpuSceneClip(fixed,16384,16384,16384,16384,fixedRect));
    uint32_t random=7;
    auto next=[&](unsigned bound) { random=random*1664525u+1013904223u;return random%bound; };
    for (unsigned trial=0;trial<10000;++trial) {
        const uint32_t sourceW=1+next(16),sourceH=1+next(16),rootW=1+next(19),rootH=1+next(19);
        const uint32_t sx=next(sourceW),sy=next(sourceH);
        GpuSceneLayer l{1,sx,sy,1+next(sourceW-sx),1+next(sourceH-sy),
            int(next(81))-40,int(next(81))-40,1+next(32),1+next(32),next(256),
            int(next(81))-40,int(next(81))-40,next(33),next(33)};
        GpuSceneRect r;CHECK(GpuSceneClip(l,sourceW,sourceH,rootW,rootH,r));
        bool seen=false;
        for (int y=0;y<int(rootH);++y) for (int x=0;x<int(rootW);++x) {
            const bool covered=l.opacity && x>=l.x && y>=l.y && x<int64_t(l.x)+l.w &&
                y<int64_t(l.y)+l.h && x>=l.clipX && y>=l.clipY &&
                x<int64_t(l.clipX)+l.clipW && y<int64_t(l.clipY)+l.clipH;
            CHECK(covered==(r.visible && x>=r.x && x<r.right && y>=r.y && y<r.bottom));
            if (!covered) continue;
            seen=true;
            const double u=(l.sx+(x+.5-l.x)*l.sw/l.w)/sourceW;
            const double v=(l.sy+(y+.5-l.y)*l.sh/l.h)/sourceH;
            const double actualU=(r.u0+(x+.5-r.x)*(r.u1-r.u0)/(r.right-r.x))/r.uDen;
            const double actualV=(r.v0+(y+.5-r.y)*(r.v1-r.v0)/(r.bottom-r.y))/r.vDen;
            CHECK(std::abs(u-actualU)<1e-12 && std::abs(v-actualV)<1e-12);
        }
        CHECK(seen==r.visible);
    }
    GpuSceneLayer edge{1,0,0,16384,16384,0,0,16384,16384,255,0,0,16384,16384};
    GpuSceneRect r;CHECK(GpuSceneClip(edge,16384,16384,16384,16384,r));
    CHECK(r.u1==268435456 && r.uDen==268435456 && r.visible);
    for (int offset:{INT_MIN,INT_MAX}) {
        edge.x=edge.y=offset;edge.clipX=edge.clipY=offset;
        CHECK(GpuSceneClip(edge,16384,16384,16384,16384,r) && !r.visible);
    }
    edge.x=edge.y=edge.clipX=edge.clipY=0;
    edge.sx=UINT_MAX;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.sx=0;
    edge.opacity=256;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.opacity=255;
    edge.w=UINT_MAX;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.w=16384;
    edge.clipW=UINT_MAX;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.clipW=16384;
    CHECK(!GpuSceneClip(edge,0,4,4,4,r));
    const std::vector<GpuSceneStep> order={GpuSceneStep::UploadVertices,GpuSceneStep::RenderAndFence,GpuSceneStep::RestoreAndFence};
    for (unsigned fail=0;fail<=order.size();++fail) {
        unsigned seen=0;
        const bool complete=GpuSceneSequence([&](auto step) { CHECK(step==order[seen]);return seen++!=fail; });
        CHECK(complete==(fail==order.size()));CHECK(seen==(fail<order.size()?fail+1:order.size()));
    }
    std::puts("GPU_SCENE_PASS randomized clipped/scaled UVs, signed limits, bounds, completion/restore failures");
}
