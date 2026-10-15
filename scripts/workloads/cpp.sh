#!/usr/bin/env bash
# Run inside Brook with the pinned bootstrap SDK installed.
set -eu
export HOME=/data/home TMPDIR=/data/tmp
mkdir -p /data/workloads
cd /data/workloads
cat > demo.cpp <<'CPP'
#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>
int main() {
    std::vector<int> values(10000);
    std::iota(values.begin(), values.end(), 1);
    std::reverse(values.begin(), values.end());
    std::thread worker([&]{ std::sort(values.begin(), values.end()); });
    worker.join();
    long total = std::accumulate(values.begin(), values.end(), 0L);
    if (total != 50005000 || values.front() != 1 || values.back() != 10000) return 1;
    { std::ofstream file("result.txt"); file << total << '\n'; }
    long loaded = 0;
    { std::ifstream file("result.txt"); file >> loaded; }
    if (loaded != total) return 2;
    std::cout << "CPP_WORKLOAD_PASS total=" << total << '\n';
}
CPP
/nix/profile/bin/brook-bootstrap-shell -c '"$HOST_CXX" -std=c++17 -O2 -pthread demo.cpp -o demo && ./demo'
echo CPP_SCRIPT_EXIT_0
