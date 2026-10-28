// Host-only codec screening. Build with -O3 -I<exact LZAV checkout> -ldl.
// Arguments: liblz4.so libzstd.so corpus-file ...; no guest/kernel claims.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "lzav.h"
using Bytes=std::vector<char>;
using Clock=std::chrono::steady_clock;
static volatile unsigned sink;
template<class T> T Symbol(void* lib,const char* name) {
    auto p=dlsym(lib,name);if(!p)throw std::runtime_error(name);return reinterpret_cast<T>(p);
}
struct Codec {
    const char* name;
    std::function<size_t(const char*,size_t,char*,size_t)> encode,decode;
};
static void Bench(const std::string& corpus,const Bytes& input,size_t block,const Codec& codec) {
    std::vector<Bytes> packed;std::vector<size_t> sizes;
    Bytes output(block*2+1024),decoded(block);size_t total=0,stored=0;
    for(size_t at=0;at<input.size();at+=block) {
        size_t n=std::min(block,input.size()-at),z=codec.encode(input.data()+at,n,output.data(),output.size());
        if(!z||z>output.size())throw std::runtime_error("encode");
        packed.emplace_back(output.begin(),output.begin()+z);sizes.push_back(n);total+=n;stored+=z;
        if(codec.decode(output.data(),z,decoded.data(),n)!=n||memcmp(decoded.data(),input.data()+at,n))
            throw std::runtime_error("roundtrip");
    }
    auto measure=[&](bool decode) {
        auto start=Clock::now();size_t done=0;unsigned rounds=0;
        do {
            size_t at=0;
            for(size_t i=0;i<sizes.size();++i) {
                size_t n=decode?codec.decode(packed[i].data(),packed[i].size(),decoded.data(),sizes[i]):
                    codec.encode(input.data()+at,sizes[i],output.data(),output.size());
                if(!n||n>output.size())throw std::runtime_error("timed codec failure");
                sink=sink^static_cast<unsigned char>(decode?decoded[n/2]:output[n/2]);
                at+=sizes[i];
            }
            done+=total;++rounds;
        }while(Clock::now()-start<std::chrono::milliseconds(120));
        const double seconds=std::chrono::duration<double>(Clock::now()-start).count();
        return double(done)/seconds/1e6;
    };
    double enc=measure(false),dec=measure(true);
    printf("%s,%zu,%s,%zu,%zu,%.4f,%.1f,%.1f,%.3f\n",corpus.c_str(),block,codec.name,total,stored,
        double(total)/stored,enc,dec,block/dec);fflush(stdout);
}
int main(int argc,char** argv) {
  try {
    if(argc<3)throw std::runtime_error("usage: benchmark liblz4.so libzstd.so [corpus ...]");
    void* lz=dlopen(argv[1],RTLD_NOW);void* zs=dlopen(argv[2],RTLD_NOW);if(!lz||!zs)throw std::runtime_error("dlopen");
    auto lzenc=Symbol<int(*)(void*,const char*,char*,int,int,int)>(lz,"LZ4_compress_fast_extState");
    auto lzdec=Symbol<int(*)(const char*,char*,int,int)>(lz,"LZ4_decompress_safe");
    auto hc=Symbol<int(*)(void*,const char*,char*,int,int,int)>(lz,"LZ4_compress_HC_extStateHC");
    Bytes state(Symbol<int(*)()>(lz,"LZ4_sizeofState")()),hcstate(Symbol<int(*)()>(lz,"LZ4_sizeofStateHC")());
    auto zcreate=Symbol<void*(*)()>(zs,"ZSTD_createCCtx"),zdcreate=Symbol<void*(*)()>(zs,"ZSTD_createDCtx");
    auto zenc=Symbol<size_t(*)(void*,void*,size_t,const void*,size_t,int)>(zs,"ZSTD_compressCCtx");
    auto zdec=Symbol<size_t(*)(void*,void*,size_t,const void*,size_t)>(zs,"ZSTD_decompressDCtx");
    auto zerr=Symbol<unsigned(*)(size_t)>(zs,"ZSTD_isError");
    void* zc=zcreate();void* zd=zdcreate();if(!zc||!zd)throw std::runtime_error("zstd context");
    std::vector<Codec> codecs;
    codecs.push_back({"memcpy",[](const char* p,size_t n,char* o,size_t){memcpy(o,p,n);return n;},
        [](const char* p,size_t n,char* o,size_t){memcpy(o,p,n);return n;}});
    auto ldecode=[&](const char* p,size_t n,char* o,size_t cap)->size_t {int z=lzdec(p,o,n,cap);return z<0?0:z;};
    codecs.push_back({"lz4-1.10.0",[&](const char* p,size_t n,char* o,size_t cap)->size_t{return lzenc(state.data(),p,o,n,cap,1);},ldecode});
    codecs.push_back({"lz4hc-9",[&](const char* p,size_t n,char* o,size_t cap)->size_t{return hc(hcstate.data(),p,o,n,cap,9);},ldecode});
    codecs.push_back({"lzav-5.20",[](const char* p,size_t n,char* o,size_t cap)->size_t{return lzav_compress_default(p,o,n,cap);},
        [](const char* p,size_t n,char* o,size_t cap)->size_t{int z=lzav_decompress(p,o,n,cap);return z<0?0:z;}});
    for(int level:{-3,1})codecs.push_back({level<0?"zstd-1.5.7-fast3":"zstd-1.5.7-level1",
        [&,level](const char* p,size_t n,char* o,size_t cap)->size_t{size_t z=zenc(zc,o,cap,p,n,level);return zerr(z)?0:z;},
        [&](const char* p,size_t n,char* o,size_t cap)->size_t{size_t z=zdec(zd,o,cap,p,n);return zerr(z)?0:z;}});
    std::vector<std::pair<std::string,Bytes>> corpora;
    for(int i=3;i<argc;++i) {
        std::ifstream f(argv[i],std::ios::binary);if(!f)throw std::runtime_error(argv[i]);
        Bytes b(8*1024*1024);f.read(b.data(),b.size());b.resize(f.gcount());if(!b.empty())corpora.push_back({"file"+std::to_string(i-3),std::move(b)});
        fprintf(stderr,"file%d=%s\n",i-3,argv[i]);
    }
    Bytes zero(1024*1024,0),sparse=zero,noise=zero;unsigned rng=1234567;
    for(size_t i=0;i<noise.size();++i){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;noise[i]=rng;sparse[i]=(i%64==0)?noise[i]:0;}
    corpora.push_back({"zero",std::move(zero)});corpora.push_back({"sparse",std::move(sparse)});corpora.push_back({"random",std::move(noise)});
    puts("corpus,block_bytes,codec,input_bytes,stored_bytes,ratio,encode_MBps,decode_MBps,decode_block_us");
    for(auto& corpus:corpora)for(size_t block:{4096,65536,262144})for(auto& codec:codecs)Bench(corpus.first,corpus.second,block,codec);
    Symbol<size_t(*)(void*)>(zs,"ZSTD_freeCCtx")(zc);Symbol<size_t(*)(void*)>(zs,"ZSTD_freeDCtx")(zd);
    fprintf(stderr,"sink=%u; LZ4 workspace=%zu LZ4HC workspace=%zu; LZAV default may allocate; Zstd contexts retained\n",sink,state.size(),hcstate.size());
  }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
