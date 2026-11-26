// SPDX-License-Identifier: MIT
// Brook M4 step 4: compile a minimal compute kernel for SM75 (TU104) with NAK and dump the
// encoded code, NAK's assembly and a QMD filled by nak_fill_qmd for fixed GPU VAs. Built and
// run by nix/nak-m4-kernel.nix against the pinned Mesa source; no GPU is needed.
//
// The kernel (TestShaderBuilder, as NAK's own hw tests): invocation i = ctaid.x * 32 + lane;
// exit if i >= cb0.invocations; store 0x600df00d at cb0.data_addr + i * cb0.data_stride.
use crate::hw_tests::TestShaderBuilder;
use crate::builder::*;
use crate::ir::*;
use nak_bindings::*;
use std::io::Write;

// Fixed VAs Brook maps (nv_gsp_m4.h kGrVaCompute*): shader code, CB0, QMD.
const SHADER_VA: u64 = 0x0000_002a_0000_0000;
const CB0_VA: u64 = 0x0000_002a_0001_0000;
const CB0_SIZE: u32 = 256;

#[test]
fn dump() {
    let out = std::env::var("BROOK_NAK_OUT").expect("BROOK_NAK_OUT");
    let invocations: u32 = std::env::var("BROOK_NAK_INVOCATIONS").ok().and_then(|v| v.parse().ok()).unwrap_or(128);
    let sm = ShaderModelInfo::new(75, 32);   // TU104: SM 7.5, 32 warps per SM
    let mut b = TestShaderBuilder::new(&sm);
    let v = b.copy(0x600df00d_u32.into());
    b.st_test_data(0, MemType::B32, v.into());
    let bin = b.compile();
    let code = unsafe { std::slice::from_raw_parts(bin.bin.code as *const u8, bin.bin.code_size as usize) };
    std::fs::write(format!("{out}/kernel.bin"), code).unwrap();
    // kernel.asm: NAK's post-RA IR, printed to stderr by NAK_DEBUG=print (the builder sets it).

    let mut dev: nv_device_info = unsafe { std::mem::zeroed() };
    dev.sm = 75;
    dev.cls_compute = 0xc5c0;   // TURING_COMPUTE_A
    dev.max_warps_per_mp = 32;
    // as nouveau_device.c for SM 7.5: 32 or 64 KB carveouts, 64 KB per workgroup
    dev.sm_smem_sizes_kB[0] = 32;
    dev.sm_smem_sizes_kB[1] = 64;
    dev.sm_smem_size_count = 2;
    dev.max_smem_per_wg_kB = 64;
    let cs = unsafe { bin.bin.info.__bindgen_anon_1.cs };
    let local = cs.local_size[0] as u32;
    let mut cbufs: [nak_qmd_cbuf; 8] = Default::default();
    cbufs[0] = nak_qmd_cbuf { index: 0, size: CB0_SIZE, addr: CB0_VA };
    let qmd_info = nak_qmd_info {
        addr: SHADER_VA,
        smem_size: cs.smem_size.into(),
        global_size: [invocations.div_ceil(local), 1, 1],
        num_cbufs: 1,
        cbufs,
    };
    let size = crate::qmd::nak_qmd_size_B(&dev) as usize;
    let mut qmd = vec![0u8; size];
    crate::qmd::nak_fill_qmd(&dev, &bin.bin.info, &qmd_info, qmd.as_mut_ptr().cast(), size);
    std::fs::write(format!("{out}/qmd.bin"), &qmd).unwrap();

    let mut f = std::fs::File::create(format!("{out}/info.txt")).unwrap();
    writeln!(f, "sm 75").unwrap();
    writeln!(f, "cls_compute 0xc5c0").unwrap();
    writeln!(f, "code_size {}", bin.bin.code_size).unwrap();
    writeln!(f, "num_gprs {}", bin.bin.info.num_gprs).unwrap();
    writeln!(f, "local_size {} {} {}", cs.local_size[0], cs.local_size[1], cs.local_size[2]).unwrap();
    writeln!(f, "smem_size {}", cs.smem_size).unwrap();
    writeln!(f, "slm_size {}", bin.bin.info.slm_size).unwrap();
    writeln!(f, "invocations {invocations}").unwrap();
    writeln!(f, "qmd_size {size}").unwrap();
    writeln!(f, "shader_va {SHADER_VA:#x}").unwrap();
    writeln!(f, "cb0_va {CB0_VA:#x} cb0_size {CB0_SIZE}").unwrap();
    writeln!(f, "cb0 data_addr_lo@0 data_addr_hi@4 data_stride@8 invocations@12").unwrap();
    writeln!(f, "store 0x600df00d").unwrap();
}
