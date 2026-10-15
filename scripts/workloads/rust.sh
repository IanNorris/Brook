#!/usr/bin/env bash
# Run inside Brook after installing the pinned Rust compiler and bootstrap SDK.
set -eu
export HOME=/data/home TMPDIR=/data/tmp
mkdir -p /data/workloads
cd /data/workloads
cat > demo.rs <<'RS'
use std::{fs, thread};
fn main() {
    let worker = thread::spawn(|| {
        let mut values: Vec<u64> = (1..=10000).rev().collect();
        values.sort();
        assert_eq!(values[0], 1);
        assert_eq!(values[9999], 10000);
        values.iter().sum::<u64>()
    });
    let total = worker.join().unwrap();
    assert_eq!(total, 50005000);
    fs::write("rust-result.txt", format!("{}\n", total)).unwrap();
    assert_eq!(fs::read_to_string("rust-result.txt").unwrap(), "50005000\n");
    println!("RUST_WORKLOAD_PASS total={}", total);
}
RS
/nix/profile/bin/brook-bootstrap-shell -c 'rustc --edition=2021 -C opt-level=1 -C linker="$HOST_CC" demo.rs -o rust-demo && ./rust-demo'
echo RUST_SCRIPT_EXIT_0
