#!/usr/bin/env python3
"""Verify private guarded desktop files; never open or modify a device.

The reference initrd and build metadata are evidence, not attestation or
current-boot identity. A successful result does not authorize installation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from prepare_guard_check_pair import CHECKER_SHA, E2FSCK_SHA, REQUIRED, selector
from prepare_hardware_kernel_pair import (INITRD_MAX, KERNEL_MAX, module_version, regular, desktop_startup, add_vulkan_desktop_start,
                                          KERNEL_CONSOLE_SHORTCUT, KERNEL_CONSOLE_SHORTCUT_PATH)

FILES=('ETC/ahci-write-target','ETC/ahci-boot-volume','ETC/ahci-data-volume',
       'BOOT/FATFSCK.ELF','BOOT/E2FSCK.ELF')
EXPECTED=dict(REQUIRED, BROOK_SSD_AUTO_RECOVERY='ON', BROOK_SSD_GUARDED_DESKTOP='ON',
              BROOK_SSD_GUARDED_DESKTOP_RESET='ON',BROOK_MAPPING_TEST_HOOKS='OFF',
              BROOK_REGISTER_TEST_HOOKS='OFF',BROOK_SCHED_START_TEST_HOOKS='OFF',
              BROOK_EPOLL_TARGET_DIAGNOSTICS='OFF',BROOK_GPU_CAPTURE_FAULT_TEST='OFF',BROOK_GPU_SUBMISSION_TIMING='OFF',BROOK_GPU_BLOCKED_RECOVERY_TEST='OFF',BROOK_EFI_SHUTDOWN_TEST='OFF')
DERIVED={'BROOK_SSD_GUARD_TRANSACTION','BROOK_SSD_GUARD_REPAIR','BROOK_SSD_GUARD_COMPLETION'}

def sha(data):return hashlib.sha256(data).hexdigest()

def verify(candidate, source, reference, build, expected_kernel, expected_source, expected_reference, abi, allow_gpu_capture_test=False, allow_gpu_submission_timing=False, allow_vulkan_desktop=False, expected_vulkan_supervisor=None, expected_vulkan_renderer=None, allow_vulkan_partial=False, allow_gpu_blocked_recovery_test=False, expected_gpu_provider=None, allow_gpu_source_trace=False, allow_dell_acpi_monitor=False, allow_ac_loss_poweroff=False, allow_dell_bcs_present=False, allow_vulkan_async_partial=False, allow_bdw_cache_probe=False, allow_bdw_rps_trial=False, allow_bdw_coherent_render=False, allow_irq_signal_probe=False, allow_hidden_kernel_console=False):
    if (allow_vulkan_partial or allow_vulkan_async_partial) and not allow_vulkan_desktop:
        raise ValueError('partial rendering requires Vulkan desktop opt-in')
    if allow_dell_bcs_present and not allow_vulkan_desktop:raise ValueError('Dell BCS presentation requires Vulkan desktop opt-in')
    if expected_gpu_provider and not allow_vulkan_desktop:raise ValueError('GPU source requires Vulkan desktop opt-in')
    if allow_gpu_source_trace and not expected_gpu_provider:raise ValueError('GPU source trace requires GPU source opt-in')
    if allow_ac_loss_poweroff and not allow_dell_acpi_monitor:
        raise ValueError('AC loss power-off requires the Dell monitor opt-in')
    if type(abi) is not int or abi<1:raise ValueError('invalid module ABI')
    image=regular(candidate/'INITRD.IMG',INITRD_MAX);kernel=regular(candidate/'kernel.elf',KERNEL_MAX)
    original=regular(source,INITRD_MAX);guard=regular(reference,INITRD_MAX)
    for label,actual,expected in (('kernel',sha(kernel),expected_kernel),('source',sha(original),expected_source),('reference',sha(guard),expected_reference)):
        if not re.fullmatch('[0-9a-f]{64}',expected) or actual!=expected:raise ValueError(label+' hash mismatch')
    if kernel!=regular(build/'BROOK.elf',KERNEL_MAX):raise ValueError('candidate kernel differs from build')
    cache=regular(build/'CMakeCache.txt',1024*1024).decode()
    expected_options=dict(EXPECTED)
    monitor_option='BROOK_DELL_ACPI_MONITOR'
    if allow_dell_acpi_monitor or f'{monitor_option}:BOOL=' in cache:
        expected_options[monitor_option]='ON' if allow_dell_acpi_monitor else 'OFF'
    probe_option='BROOK_BDW_CACHE_PROBE'
    if allow_bdw_cache_probe or f'{probe_option}:BOOL=' in cache:
        expected_options[probe_option]='ON' if allow_bdw_cache_probe else 'OFF'
    trial_option='BROOK_BDW_RPS_TRIAL'
    if allow_bdw_rps_trial or f'{trial_option}:BOOL=' in cache:
        expected_options[trial_option]='ON' if allow_bdw_rps_trial else 'OFF'
    coherent_option='BROOK_BDW_COHERENT_RENDER'
    if allow_bdw_coherent_render or f'{coherent_option}:BOOL=' in cache:
        expected_options[coherent_option]='ON' if allow_bdw_coherent_render else 'OFF'
    if allow_gpu_capture_test:expected_options['BROOK_GPU_CAPTURE_FAULT_TEST']='ON'
    if allow_gpu_submission_timing:expected_options['BROOK_GPU_SUBMISSION_TIMING']='ON'
    if allow_gpu_blocked_recovery_test:expected_options['BROOK_GPU_BLOCKED_RECOVERY_TEST']='ON'
    for name,value in expected_options.items():
        if len(re.findall(r'^'+name+r':BOOL='+value+r'$',cache,re.M))!=1:raise ValueError('unsafe build option '+name)
    if len(re.findall(r'^BROOK_SMP_BOOT_TEST_STAGE:STRING=0$',cache,re.M))!=1:raise ValueError('unsafe SMP test stage')
    compile_data=regular(build/'compile_commands.json',16*1024*1024)
    commands=json.loads(compile_data)
    protected=set(expected_options)|DERIVED|{'BROOK_SMP_BOOT_TEST_STAGE',monitor_option}
    expected_defs={k for k,v in expected_options.items() if v=='ON'}|DERIVED
    for name in ('kernel.cpp','ahci_readonly.cpp','ahci_system_volume.cpp','guard_checker_runner.cpp','shutdown.cpp','intel_bdw_ioctl.cpp'):
        matches=[item for item in commands if Path(item['file']).name==name]
        if len(matches)!=1:raise ValueError('missing compile evidence '+name)
        args=shlex.split(matches[0]['command']);defs=[];i=0
        while i<len(args):
            arg=args[i]
            if arg.startswith(('@','-include','-imacros','-Wp,')) or arg in ('-Xclang','-Xpreprocessor'):
                raise ValueError('unreviewable compile flags '+name)
            if arg in ('-D','-U'):
                i+=1
                if i>=len(args):raise ValueError('incomplete compile flags '+name)
                defs.append((arg,args[i]))
            elif arg.startswith(('-D','-U')) and len(arg)>2:defs.append((arg[:2],arg[2:]))
            i+=1
        found=[x for x in defs if x[1].split('=')[0] in protected]
        required_defs={('-D',x+'=1') for x in expected_defs}|{('-D','BROOK_SMP_BOOT_TEST_STAGE=0')}
        if len(found)!=len(required_defs) or set(found)!=required_defs:
            raise ValueError('unsafe compile flags '+name)
    with tempfile.TemporaryDirectory(prefix='brook-guarded-desktop-verify-') as tmp:
        work=Path(tmp);paths=[]
        for name,contents in (('candidate',image),('source',original),('reference',guard)):
            p=work/(name+'.img');p.write_bytes(contents);paths.append(p)
        live,base,ref=paths
        subprocess.run(['fsck.fat','-n',str(live)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        def read(path,name):return subprocess.check_output(['mtype','-i',str(path),'::/'+name],stderr=subprocess.DEVNULL)
        for name,enabled in (('dell-acpi-monitor',allow_dell_acpi_monitor),
                             ('ac-loss-poweroff',allow_ac_loss_poweroff),
                             ('dell-gpu-bcs-present',allow_dell_bcs_present),
                             ('irq-signal-probe',allow_irq_signal_probe)):
            marker=subprocess.run(['mtype','-i',str(live),'::/ETC/'+name],capture_output=True)
            if enabled and (marker.returncode or marker.stdout!=b'1\n'):
                raise ValueError('Missing or invalid opt-in '+name)
            if not enabled and marker.returncode==0:
                raise ValueError('Unexpected opt-in '+name)
        values={name:read(live,name) for name in FILES}
        for name in FILES:
            if values[name]!=read(base,name) or values[name]!=read(ref,name):raise ValueError('guard input mismatch '+name)
        if sha(values['BOOT/FATFSCK.ELF'])!=CHECKER_SHA or sha(values['BOOT/E2FSCK.ELF'])!=E2FSCK_SHA:
            raise ValueError('recovery checker pin mismatch')
        esp=selector(values['ETC/ahci-boot-volume'],'esp');data=selector(values['ETC/ahci-data-volume'],'data')
        if esp[0]!=data[0] or esp[1]==data[1] or not (esp[2]+esp[3]<=data[2] or data[2]+data[3]<=esp[2]):
            raise ValueError('inconsistent guard selectors')
        script=read(live,'BOOT/INIT.RC')
        original_script=read(base,'BOOT/INIT.RC')
        expected_script=desktop_startup(original_script,allow_hidden_kernel_console,allow_vulkan_desktop,allow_vulkan_partial,
            bool(expected_gpu_provider),allow_gpu_source_trace,allow_vulkan_async_partial)
        if not script or script!=expected_script:raise ValueError('desktop startup script mismatch')
        def optional(path,name):
            found=subprocess.run(['mtype','-i',str(path),'::/'+name],capture_output=True)
            return None if found.returncode else found.stdout
        shortcut=optional(live,KERNEL_CONSOLE_SHORTCUT_PATH)
        expected_shortcut=KERNEL_CONSOLE_SHORTCUT if allow_hidden_kernel_console else optional(base,KERNEL_CONSOLE_SHORTCUT_PATH)
        if shortcut!=expected_shortcut:raise ValueError('Kernel Console shortcut mismatch')
        vulkan_files={}
        if allow_vulkan_desktop:
            executables=[('VKSUP.ELF',expected_vulkan_supervisor),('VKRENDER.ELF',expected_vulkan_renderer)]
            if expected_gpu_provider:executables.append(('INTLWL.ELF',expected_gpu_provider))
            for name,expected in executables:
                content=read(live,'BOOT/'+name)
                if not expected or not re.fullmatch('[0-9a-f]{64}',expected) or not content.startswith(b'\x7fELF') or sha(content)!=expected:
                    raise ValueError('Vulkan executable mismatch '+name)
                vulkan_files[name]=sha(content)
        def modules(path):
            listing=subprocess.check_output(['mdir','-a','-b','-i',str(path),'::/DRIVERS/*'],text=True)
            names=[line.rsplit('/',1)[-1] for line in listing.splitlines() if line]
            if not names or len({n.lower() for n in names})!=len(names) or any(not re.fullmatch(r'[A-Za-z0-9_-]+\.[Mm][Oo][Dd]',n) for n in names):
                raise ValueError('invalid module inventory')
            return names
        names=modules(live)
        if {n.lower() for n in names}!={n.lower() for n in modules(base)}:raise ValueError('module inventory changed')
        module_hashes={}
        for name in names:
            contents=regular(build/'drivers'/name.lower(),16*1024*1024)
            if module_version(contents)!=abi or read(live,'DRIVERS/'+name)!=contents:raise ValueError('module mismatch '+name)
            module_hashes[name]=sha(contents)
    return {'scope':'private-file verification only; not deployment or physical boot authorization',
            'kernel_sha256':sha(kernel),'initrd_sha256':sha(image),'source_initrd_sha256':sha(original),
            'reference_initrd_sha256':sha(guard),'build_cache_sha256':sha(cache.encode()),
            'compile_commands_sha256':sha(compile_data),'startup_script_sha256':sha(script),
            'guard_input_sha256':{name:sha(value) for name,value in values.items()},'module_abi':abi,'modules':module_hashes,
            'vulkan_desktop_enabled':allow_vulkan_desktop,'vulkan_partial_enabled':allow_vulkan_partial or allow_vulkan_async_partial,
            'vulkan_async_partial_enabled':allow_vulkan_async_partial,'vulkan_executables_sha256':vulkan_files,
            'kernel_console_hidden':allow_hidden_kernel_console}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('candidate','source','reference','build'):p.add_argument('--'+name,type=Path,required=True)
    for name in ('expected-kernel','expected-source','expected-reference'):p.add_argument('--'+name,required=True)
    p.add_argument('--abi',type=int,required=True)
    p.add_argument('--allow-bdw-coherent-render',action='store_true',help='explicitly permit the coherent Broadwell render trial')
    p.add_argument('--allow-bdw-rps-trial',action='store_true',help='explicitly permit the bounded Broadwell clock trial')
    p.add_argument('--allow-bdw-cache-probe',action='store_true',help='explicitly permit the root-only Broadwell cache oracle')
    p.add_argument('--allow-gpu-capture-test',action='store_true',help='explicitly permit the dedicated fault-injection kernel')
    p.add_argument('--allow-gpu-submission-timing',action='store_true',help='explicitly permit bounded diagnostic GPU timings')
    p.add_argument('--allow-gpu-blocked-recovery-test',action='store_true',help='explicitly permit the dedicated synthetic pending-fence test kernel')
    p.add_argument('--allow-vulkan-desktop',action='store_true',help='expect the exact opt-in supervised desktop startup entry')
    p.add_argument('--allow-vulkan-partial',action='store_true',help='expect experimental partial rendering startup')
    p.add_argument('--allow-vulkan-async-partial',action='store_true',help='expect the two-buffer asynchronous partial startup')
    p.add_argument('--allow-dell-bcs-present',action='store_true',help='expect the exact opt-in for Dell Broadwell blitter presentation')
    p.add_argument('--expected-vulkan-supervisor',help='SHA-256 of the embedded supervisor')
    p.add_argument('--expected-vulkan-renderer',help='SHA-256 of the embedded renderer')
    p.add_argument('--expected-gpu-provider',help='SHA-256 of the embedded paired GPU-source provider')
    p.add_argument('--allow-gpu-source-trace',action='store_true',help='expect opt-in first direct-source presentation logging')
    p.add_argument('--allow-dell-acpi-monitor',action='store_true',help='expect the Dell firmware monitor build and boot opt-in')
    p.add_argument('--allow-ac-loss-poweroff',action='store_true',help='expect the AC loss shutdown boot opt-in')
    p.add_argument('--allow-irq-signal-probe',action='store_true',help='expect experimental interrupt-return signal delivery')
    p.add_argument('--allow-hidden-kernel-console',action='store_true',
                   help='expect the boot kconsole line removed and the Kernel Console Apps entry added')
    a=p.parse_args();print(json.dumps(verify(a.candidate,a.source,a.reference,a.build,a.expected_kernel,a.expected_source,a.expected_reference,a.abi,a.allow_gpu_capture_test,a.allow_gpu_submission_timing,a.allow_vulkan_desktop,a.expected_vulkan_supervisor,a.expected_vulkan_renderer,a.allow_vulkan_partial,a.allow_gpu_blocked_recovery_test,a.expected_gpu_provider,a.allow_gpu_source_trace,a.allow_dell_acpi_monitor,a.allow_ac_loss_poweroff,a.allow_dell_bcs_present,a.allow_vulkan_async_partial,a.allow_bdw_cache_probe,a.allow_bdw_rps_trial,a.allow_bdw_coherent_render,a.allow_irq_signal_probe,a.allow_hidden_kernel_console),indent=2))

if __name__=='__main__':main()
