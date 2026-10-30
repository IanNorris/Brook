#include "../../kernel/src/panic_qr_layout.h"
#include <cassert>
#include <initializer_list>
using namespace brook;
int main() {
    for(unsigned width: {640u,800u,1024u,1920u,3840u}) {
        const unsigned height=width==640?480:width==800?600:width*9/16;
        const unsigned x=width*55/100,w=width-x;
        const auto url=FitPanicQr({x,60,w,height/4},33,9,0,1);
        assert(url.scale && url.x>=x && url.x+url.side<=width);
        const unsigned top=url.y+url.side+16;
        for(unsigned pages=1;pages<=4;++pages) {
            PanicQrPlacement boxes[4];
            for(unsigned p=0;p<pages;++p) {
                boxes[p]=FitPanicQr({x,top,w,height-top-40},121,9,p,pages);
                const auto b=boxes[p];
                assert(b.scale && b.x>=x && b.y>=top);
                assert(b.x+b.side<=width && b.y+b.side<=height-40);
                for(unsigned other=0;other<p;++other) {
                    const auto a=boxes[other];
                    assert(b.x+b.side<=a.x || a.x+a.side<=b.x || b.y+b.side<=a.y || a.y+a.side<=b.y);
                }
            }
        }
    }
    assert(!FitPanicQr({0,0,10,10},121,9,0,1).scale);
    assert(!FitPanicQr({0,0,800,600},0,9,0,1).scale);
    assert(!FitPanicQr({0,0,800,600},121,9,4,4).scale);
}
