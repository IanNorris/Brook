#include <cassert>
#include <cstdio>
#include "../../drivers/intel_hda/hda_speaker_route.h"
using namespace brook;
int main() {
    // Dell Linux reference: speaker 0x14 -> mixer 0x0d -> DAC 0x03;
    // the earlier DAC 0x02 belongs to the alternate headphone route.
    HdaRouteNode nodes[5]{};
    nodes[0].nid=2;nodes[0].type=0;
    nodes[1].nid=3;nodes[1].type=0;
    nodes[2].nid=0x0c;nodes[2].type=2;nodes[2].connectionCount=1;nodes[2].connections[0]=2;
    nodes[3].nid=0x0d;nodes[3].type=2;nodes[3].connectionCount=1;nodes[3].connections[0]=3;
    nodes[4].nid=0x14;nodes[4].type=4;nodes[4].pinCapabilities=0x10;nodes[4].pinDefault=0x90170110;
    nodes[4].connectionCount=2;nodes[4].connections[0]=0x0c;nodes[4].connections[1]=0x0d;
    nodes[4].hasSelection=true;nodes[4].selectedConnection=1;
    HdaSpeakerRoute route{};
    assert(HdaFindSpeakerRoute(nodes,5,route));
    assert(route.length==3 && route.nodes[0]==0x14 && route.nodes[1]==0x0d && route.nodes[2]==3 && route.choices[0]==1);
    nodes[4].connections[1]=0x60;
    assert(!HdaFindSpeakerRoute(nodes,5,route) && route.length==0);
    assert(HdaFindSpeakerRoute(nodes,5,route,false) && route.nodes[2]==2 && route.choices[0]==0);
    nodes[4].connections[1]=0x0d;
    nodes[2].connections[0]=0x14;nodes[3].connections[0]=0x14;assert(!HdaFindSpeakerRoute(nodes,5,route) && route.length==0);
    nodes[2].connections[0]=2;nodes[3].connections[0]=3;nodes[0].digital=true;nodes[1].digital=true;assert(!HdaFindSpeakerRoute(nodes,5,route));
    nodes[0].digital=false;
    nodes[1].digital=false;nodes[4].pinDefault=0x411111f0;assert(!HdaFindSpeakerRoute(nodes,5,route));
    nodes[4].pinDefault=0x90170110;nodes[4].pinCapabilities=0;assert(!HdaFindSpeakerRoute(nodes,5,route));
    nodes[4].pinCapabilities=0x10;nodes[4].connectionCount=17;assert(!HdaFindSpeakerRoute(nodes,5,route));
    nodes[4].connectionCount=1;assert(!HdaFindSpeakerRoute(nodes,5,route));
    nodes[4].selectedConnection=0;nodes[3].nid=0x80;assert(!HdaFindSpeakerRoute(nodes,5,route));
    nodes[3].nid=nodes[2].nid;assert(!HdaFindSpeakerRoute(nodes,5,route));
    assert(!HdaFindSpeakerRoute(nullptr,0,route));
    HdaRouteNode chain[9]{};
    for(unsigned i=0;i<9;++i){chain[i].nid=i+1;chain[i].type=2;chain[i].connectionCount=1;chain[i].connections[0]=i+2;}
    chain[0].type=4;chain[0].pinCapabilities=0x10;chain[0].pinDefault=0x90170110;
    chain[7].type=0;chain[7].connectionCount=0;
    assert(HdaFindSpeakerRoute(chain,8,route) && route.length==8);
    chain[7].type=2;chain[7].connectionCount=1;chain[8].type=0;chain[8].connectionCount=0;
    assert(!HdaFindSpeakerRoute(chain,9,route));
    // A disconnected first speaker does not hide a later reachable one.
    chain[7].type=0;chain[7].connectionCount=0;chain[8].type=4;chain[8].pinCapabilities=0x10;
    chain[8].pinDefault=0x90170110;chain[8].connectionCount=1;chain[8].connections[0]=8;
    chain[0].connections[0]=0x60;
    assert(HdaFindSpeakerRoute(chain,9,route) && route.nodes[0]==9 && route.nodes[1]==8);
    // Dense, reconvergent graphs without a DAC fail within a shared budget.
    HdaRouteNode dense[16]{};
    for(unsigned i=0;i<16;++i){dense[i].nid=i+1;dense[i].type=2;dense[i].connectionCount=16;
        for(unsigned j=0;j<16;++j)dense[i].connections[j]=j+1;}
    dense[0].type=4;dense[0].pinCapabilities=0x10;dense[0].pinDefault=0x90170110;
    assert(!HdaFindSpeakerRoute(dense,16,route) && route.length==0);
    std::puts("HDA speaker route: PASS");
}
