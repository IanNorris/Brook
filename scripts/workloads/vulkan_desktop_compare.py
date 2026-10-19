#!/usr/bin/env python3
"""Compare accepted direct/staged WSI runs using the same frozen kernel/stack.

This checks presentation evidence, workload dimensions/arguments, command
drainage and cleanup before reporting vkmark's two repeated scene results.
It does not replace the saved-document/profile preservation audit.
"""
import argparse
import hashlib
import json
from pathlib import Path


def load(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def checked_run(root,mode,composition):
    accepted=load(root/'acceptance-complete.json')
    assert accepted['passed'] and accepted['presentation']==mode and accepted['composition']==composition
    assert accepted['window_cycles']==accepted['vkmark_cycles']==2
    assert not (root/'forced-stop.json').exists()
    assert load(root/'shutdown-result.json')['orderly_shutdown_completed']
    assert load(root/'shutdown-result.json')['qemu_exit_code']==0
    assert load(root/'launcher-exit.json')['exit_code']==0
    assert all(row['exit']==0 for row in load(root/'after-fsck.json').values())
    assert load(root/'heap-check.json')['reply']=='heap integrity: OK'
    assert load(root/'renderer-errors.json')['passed']
    backing=load(root/'final-stats.json')['gpu_backing']
    for key in ('host_blob_resources','host_aperture_leases','host_aperture_bytes',
                'host_blob_quarantined','quarantined_resources','failed_retirements'):
        assert backing[key]==0,(root,key,backing[key])
    assert backing['allocated_mappings']==backing['released_mappings']
    assert backing['allocated_page_bytes']==backing['released_page_bytes']
    presentation=load(root/'presentation-evidence.json')
    assert bool(presentation['imports'])==(mode=='direct')
    requests={}
    names=['cycle1','cycle2','vkmark1','vkmark2','cycle1-concurrent-gl','cycle2-concurrent-gl',
           'vkmark1-concurrent-gl','vkmark2-concurrent-gl','environment-policy']
    if composition=='gpu':names.append('wayland-gl')
    for name in names:
        value=load(root/name/'command-evidence.json');status=value['status']
        assert status['state']=='completed' and status['exit']['code']==0 and status['exit']['signal'] is None
        assert status['guest']['live_tasks']==0
        for stream in ('stdout','stderr'):
            info=status[stream]
            assert info['eof'] and not info['gaps'] and not info['dropped_bytes'] and not info['read_error']
            assert len(value[stream].encode())==status['guest'][stream+'_bytes']==value[stream+'_offset']
        requests[name]=value['request']
        if name=='wayland-gl':
            gl= json.loads(value['stdout'])
            assert gl['passed'] and gl['presented_frames']==60
            assert gl['other_scenes']==gl['queued_jobs']==gl['snapshot_bytes']==0
            assert 'error' not in value['stderr'].lower(),value['stderr']
    dimensions=[];fps={scene:[] for scene in ('vertex','texture')}
    for name in ('vkmark1','vkmark2'):
        window=load(root/name/'first-window.json')
        dimensions.append([window['buffer_width'],window['buffer_height']])
        for scene,value in load(root/name/'scene-fps.json')['fps'].items():
            assert scene in fps and value>0;fps[scene].append(value)
    assert all(len(values)==2 for values in fps.values())
    return dict(root=str(root.resolve()),presentation=mode,composition=composition,
                dimensions=dimensions,fps=fps,imports=len(presentation['imports']),
                kernel_sha256=load(root/'manifest.json')['kernel_sha256']),requests


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--direct',required=True,type=Path)
    parser.add_argument('--staged',required=True,type=Path)
    parser.add_argument('--fallback',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();paths=[args.direct,args.staged,args.fallback]
    checked=[checked_run(path,mode,composition) for path,mode,composition in
             zip(paths,('direct','staged','staged'),('gpu','gpu','cpu'))]
    assert len({item[0]['kernel_sha256'] for item in checked})==1
    assert len({sha(path/'acceptance-driver.py') for path in paths})==1
    modules=[load(path/'module-abi-audit.json') for path in paths]
    assert len(modules[0])==12 and all(row['abi']==32 for row in modules[0].values())
    assert all(value==modules[0] for value in modules)
    controls=[load(path/'manifest.json')['control_packages'] for path in paths]
    assert all(value==controls[0] for value in controls)
    assert len({load(path/'host-renderer.json')['sha256'] for path in paths})==1
    assert all(item[0]['dimensions']==checked[0][0]['dimensions'] for item in checked)
    assert checked[0][0]['dimensions']==[[1280,720],[1280,720]]
    for name in ('cycle1','cycle2','vkmark1','vkmark2'):
        reference=checked[0][1][name]
        for _,requests in checked:
            current=requests[name]
            assert all(current[key]==reference[key] for key in ('argv','uid','cwd'))
    # Automatic cases must receive the complete desktop environment themselves.
    for path,expected in ((args.direct,'direct'),(args.fallback,'staged')):
        assert load(path/'acceptance-complete.json')['requested']=='auto'
        policy=load(path/'environment-policy/command-evidence.json')
        assert not policy['request']['env']
        environment=json.loads(policy['stdout'])
        assert environment['BROOK_VULKAN_PRESENTATION']==expected
        assert environment['VK_DRIVER_FILES']==controls[0]['guestICD']
        assert environment['MESA_VK_WSI_DEBUG']==('' if expected=='direct' else 'sw,noshm')
        for name in ('cycle1','cycle2','vkmark1','vkmark2'):
            assert not load(path/name/'command-evidence.json')['request']['env']
    result=dict(passed=True,runs=[value for value,_ in checked],
                comparison='Same frozen kernel, host stack, 1280x720 vkmark vertex/texture 15s scenes; two repeats per path. Direct/staged use GPU composition; CPU fallback is a separate configuration.',
                limitation='Small acceptance samples, not a statistically controlled performance study; saved-document/profile preservation is audited separately.')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x') as out:json.dump(result,out,indent=2);out.write('\n')
    print(json.dumps(result))


if __name__=='__main__':main()
