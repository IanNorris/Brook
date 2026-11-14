// Volume-change pop policy: debounce and mute rules (volume_feedback.h).
#include <cassert>
#include "volume_feedback.h"
using namespace brook;
int main(){
    {VolumeFeedback f; // a single step pops immediately at the new level
     f.Change(50,55);assert(f.Tick(1000));assert(!f.Tick(1001));}
    {VolumeFeedback f; // no change, no pop
     f.Change(40,40);assert(!f.Tick(10));}
    {VolumeFeedback f; // muting never pops and cancels a pending pop
     f.Change(50,BROOK_AUDIO_MUTED|50);assert(!f.Tick(10));
     f.Change(50,55);f.Change(55,BROOK_AUDIO_MUTED|55);assert(!f.Tick(10));
     f.Change(20,0);assert(!f.Tick(20));}  // level 0 is silent too
    {VolumeFeedback f; // unmuting is an audible change and pops
     f.Change(BROOK_AUDIO_MUTED|30,30);assert(f.Tick(5));}
    {VolumeFeedback f; // a drag pops at most once per 150 ms ...
     unsigned pops=0;uint32_t level=10;
     for(uint64_t t=0;t<=1000;t+=10){f.Change(level,level+1);++level;if(f.Tick(t))++pops;}
     assert(pops==7); // t=0,150,...,900
     // ... and the level reached at release still pops once the debounce allows.
     assert(!f.Tick(1001));assert(f.Tick(1050));assert(!f.Tick(2000));}
    {VolumeFeedback f; // two quick steps: first now, second after the debounce
     f.Change(50,55);assert(f.Tick(0));f.Change(55,60);assert(!f.Tick(100));assert(f.Tick(150));}
    {VolumeFeedback f; // a mute during the debounce window drops the pending pop
     f.Change(50,55);assert(f.Tick(0));f.Change(55,60);f.Change(60,BROOK_AUDIO_MUTED|60);assert(!f.Tick(500));}
    {VolumeFeedback f; // the authorization bit (bit 16) is not a level change
     f.Change(50,50|(1u<<16));assert(!f.Tick(0));}
}
