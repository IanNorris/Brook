// The embedded volume pop: format, full-scale peak (-0.1 dBFS) and silent ends.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(){
    FILE* f=std::fopen(BROOK_VOLUME_POP_PATH,"rb");assert(f);
    std::vector<int16_t> s(1<<16);const size_t n=std::fread(s.data(),2,s.size(),f);std::fclose(f);
    assert(n==3540*2); // 3540 stereo frames, 80 ms at 44.1 kHz
    int peak=0;for(size_t i=0;i<n;++i)peak=std::max(peak,std::abs(int(s[i])));
    const double dbfs=20*std::log10(peak/32767.0);
    assert(dbfs>-0.15 && dbfs<=-0.05); // normalised to -0.1 dBFS, no clipping
    assert(std::abs(int(s[0]))<64 && std::abs(int(s[1]))<64);       // fade-in starts near zero
    assert(s[n-1]==0 && s[n-2]==0);                                  // fade-out ends at zero
}
