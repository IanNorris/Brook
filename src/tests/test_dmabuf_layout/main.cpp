#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../shared/dmabuf_layout.h"
int main() {
    BrookDmabufLayout layout{1280,720,BROOK_DMABUF_XRGB8888,5120,0,0,BROOK_DMABUF_MOD_LINEAR};
    const uint64_t size=1280*720*4;
    assert(BrookDmabufValidate(&layout,size,0)==BROOK_DMABUF_LAYOUT_OK);
    assert(BrookDmabufValidate(&layout,size-1,0)==BROOK_DMABUF_LAYOUT_BOUNDS);
    layout.offset=4096;assert(BrookDmabufValidate(&layout,size+4096,0)==BROOK_DMABUF_LAYOUT_OK);
    assert(BrookDmabufValidate(&layout,size,0)==BROOK_DMABUF_LAYOUT_BOUNDS);
    layout.offset=1;assert(BrookDmabufValidate(&layout,UINT64_MAX,0)==BROOK_DMABUF_LAYOUT_BOUNDS);
    layout.offset=0;layout.stride=5116;assert(BrookDmabufValidate(&layout,UINT64_MAX,0)==BROOK_DMABUF_LAYOUT_BOUNDS);
    layout.stride=5121;assert(BrookDmabufValidate(&layout,UINT64_MAX,0)==BROOK_DMABUF_LAYOUT_BOUNDS);
    layout.stride=5120;layout.modifier=BROOK_DMABUF_MOD_IMPLICIT;
    assert(BrookDmabufValidate(&layout,size,1)==BROOK_DMABUF_LAYOUT_OK);
    assert(BrookDmabufValidate(&layout,size,0)==BROOK_DMABUF_LAYOUT_FORMAT);
    layout.modifier=1;assert(BrookDmabufValidate(&layout,size,1)==BROOK_DMABUF_LAYOUT_FORMAT);
    layout.modifier=0;layout.flags=1;assert(BrookDmabufValidate(&layout,size,1)==BROOK_DMABUF_LAYOUT_FORMAT);
    layout.flags=0;layout.fourcc=0;assert(BrookDmabufValidate(&layout,size,1)==BROOK_DMABUF_LAYOUT_FORMAT);
    layout.fourcc=BROOK_DMABUF_ARGB8888;layout.height=0;assert(BrookDmabufValidate(&layout,size,1)==BROOK_DMABUF_LAYOUT_DIMENSIONS);
    layout.height=720;layout.width=UINT32_MAX;assert(BrookDmabufValidate(&layout,UINT64_MAX,1)==BROOK_DMABUF_LAYOUT_DIMENSIONS);
    layout.width=16384;layout.height=16384;layout.stride=UINT32_MAX-3;layout.offset=UINT32_MAX-3;
    assert(BrookDmabufValidate(&layout,UINT32_MAX,0)==BROOK_DMABUF_LAYOUT_BOUNDS);
    assert(BrookDmabufValidate(&layout,UINT64_MAX,0)==BROOK_DMABUF_LAYOUT_OK);
    puts("DMABUF_LAYOUT_PASS geometry/format/modifier/flags/alignment/exact-bounds/large-stride");
}
