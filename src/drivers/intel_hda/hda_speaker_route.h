#pragma once
#include <stdint.h>

namespace brook {
// Pure bounded route selection over an already validated HDA codec graph.
// Strict mode follows a reported selector/pin choice; mixers can have several
// inputs independently muted. This proves neither an active nor audible path.
// It issues no codec verbs and does not enable or configure hardware.
constexpr unsigned HdaRouteMaxNodes=64, HdaRouteMaxConnections=16, HdaRouteMaxDepth=8;
constexpr unsigned HdaRouteVisitBudget=256;
struct HdaRouteNode {
    uint8_t nid, type, connectionCount;
    bool digital, hasSelection;
    uint8_t selectedConnection;
    uint32_t pinCapabilities, pinDefault;
    uint8_t connections[HdaRouteMaxConnections];
};
struct HdaSpeakerRoute {
    uint8_t length=0;
    uint8_t nodes[HdaRouteMaxDepth]{};
    // Index into the corresponding node's connection list; DAC has no edge.
    uint8_t choices[HdaRouteMaxDepth]{};
};
inline const HdaRouteNode* HdaFindRouteNode(const HdaRouteNode* nodes,unsigned count,uint8_t nid) {
    for(unsigned i=0;i<count;++i)if(nodes[i].nid==nid)return &nodes[i];
    return nullptr;
}
inline bool HdaSpeakerRouteWalk(const HdaRouteNode* nodes,unsigned count,uint8_t nid,
                                uint64_t* visited,unsigned depth,unsigned& budget,
                                bool strictSelection,HdaSpeakerRoute& route) {
    if(depth>=HdaRouteMaxDepth || !budget)return false;
    --budget;
    const uint64_t bit=uint64_t(1)<<(nid&63);const unsigned bank=nid>>6;
    if(visited[bank]&bit)return false;
    const auto* node=HdaFindRouteNode(nodes,count,nid);
    if(!node || node->digital)return false;
    visited[bank]|=bit;route.nodes[depth]=nid;
    if(node->type==0 && !node->digital) {route.length=depth+1;visited[bank]&=~bit;return true;}
    // Only output pins, mixers and selectors can lead to an analog DAC.
    if(node->type==2 || node->type==3 || (depth==0 && node->type==4)) {
        for(unsigned n=0;n<node->connectionCount;++n) {
            if(!budget || (strictSelection && node->hasSelection && n))break;
            // Prefer the currently selected input when one is reported.
            unsigned i=n;
            if(node->hasSelection && node->selectedConnection<node->connectionCount) {
                if(n==0)i=node->selectedConnection;
                else if(n<=node->selectedConnection)i=n-1;
            }
            route.choices[depth]=static_cast<uint8_t>(i);
            if(HdaSpeakerRouteWalk(nodes,count,node->connections[i],visited,depth+1,budget,strictSelection,route)) {
                visited[bank]&=~bit;return true;
            }
        }
    }
    visited[bank]&=~bit;return false;
}
inline bool HdaFindSpeakerRoute(const HdaRouteNode* nodes,unsigned count,HdaSpeakerRoute& result,bool strictSelection=true) {
    result={};
    if(!nodes || !count || count>HdaRouteMaxNodes)return false;
    for(unsigned i=0;i<count;++i) {
        if(!nodes[i].nid || nodes[i].nid>0x7f || nodes[i].connectionCount>HdaRouteMaxConnections ||
           (nodes[i].hasSelection && nodes[i].selectedConnection>=nodes[i].connectionCount))return false;
        for(unsigned j=0;j<nodes[i].connectionCount;++j)if(nodes[i].connections[j]>0x7f)return false;
        for(unsigned j=0;j<i;++j)if(nodes[i].nid==nodes[j].nid)return false;
    }
    unsigned budget=HdaRouteVisitBudget;
    for(unsigned i=0;i<count && budget;++i) {
        const auto& node=nodes[i];
        // HDA default device 1 means speaker; connectivity 1 means absent.
        if(node.type!=4 || !(node.pinCapabilities&(1u<<4)) ||
           ((node.pinDefault>>20)&15)!=1 || ((node.pinDefault>>30)&3)==1)continue;
        HdaSpeakerRoute candidate{};uint64_t visited[4]{};
        if(HdaSpeakerRouteWalk(nodes,count,node.nid,visited,0,budget,strictSelection,candidate)) {result=candidate;return true;}
    }
    return false;
}
}
