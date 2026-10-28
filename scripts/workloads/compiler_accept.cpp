// Compile/link/runtime acceptance for a real Nix C++ toolchain in Brook.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>
int main(int argc,char** argv) {
    assert(argc==2);constexpr unsigned Count=100000,Threads=4;
    std::vector<uint32_t> values(Count);std::vector<std::thread> workers;
    for(unsigned worker=0;worker<Threads;++worker)
        workers.emplace_back([&,worker]{for(unsigned i=worker;i<Count;i+=Threads)values[i]=Count-i-1;});
    for(auto& worker:workers)worker.join();
    std::sort(values.begin(),values.end());
    uint64_t sum=std::accumulate(values.begin(),values.end(),uint64_t{0});assert(sum==4999950000ULL);
    for(unsigned i=0;i<Count;++i)assert(values[i]==i);
    {std::ofstream out(argv[1],std::ios::binary|std::ios::trunc);assert(out);out.write(reinterpret_cast<const char*>(values.data()),values.size()*sizeof(uint32_t));out.close();assert(out);}
    assert(std::filesystem::file_size(argv[1])==Count*sizeof(uint32_t));
    std::vector<uint32_t> reloaded(Count);std::ifstream in(argv[1],std::ios::binary);assert(in);
    in.read(reinterpret_cast<char*>(reloaded.data()),reloaded.size()*sizeof(uint32_t));assert(in && reloaded==values);
    std::cout<<"COMPILER_PROGRAM_PASS count="<<Count<<" threads="<<Threads<<" sum="<<sum<<" bytes="<<Count*sizeof(uint32_t)<<'\n';
}
