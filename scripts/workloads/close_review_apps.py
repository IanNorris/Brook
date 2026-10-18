#!/usr/bin/env python3
"""Native close barrier for known UI launch lineages on an isolated review VM.

Window disappearance and client-group exit are insufficient: shutdown helpers
can live in other groups. Require the full launch lineage to drain. Unregistered
legacy windows are deliberately unsupported. No signals or VM stop are sent.
"""
import json
from pathlib import Path
import sys
import time
import uuid


def close_apps(bridge, session, output, app_ids, launch_tokens=(), deadline_s=90,
               clock=time.monotonic, sleep=time.sleep):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    targets=set(app_ids)
    if not targets or any(not isinstance(x,str) or not x for x in targets):
        raise ValueError('Explicit application IDs are required')
    tokens={str(int(t)) for t in launch_tokens}
    if any(int(t)<=0 for t in tokens):raise ValueError('Positive launch identities required')
    deadline=clock()+deadline_s;requested=set()
    while True:
        observation=bridge.observe(session,include=['windows','processes'])
        (output/'observation.json').write_text(json.dumps(observation,indent=2))
        if not observation.get('identity_verified_at_end') or not observation.get('guest_boot_id'):
            raise RuntimeError('Guest boot identity unverified; exit is unverified')
        parts=observation['components']
        if any(parts[name]['state']!='ok' for name in ('windows','processes')):
            raise RuntimeError('Application census unavailable; exit is unverified')
        census=parts['processes']['result']
        if census.get('truncated'):raise RuntimeError('Truncated census; exit is unverified')
        identity={'session':session,'generation':observation.get('generation'),
                  'guest_boot_id':observation.get('guest_boot_id')}
        receipt=output/'barrier.json'
        if receipt.exists():
            previous=json.loads(receipt.read_text())
            if previous['identity']!=identity or previous['app_ids']!=sorted(targets):
                raise RuntimeError('Exit record targets another boot/application set')
            tokens.update(previous['launch_tokens'])
        windows=[w for w in parts['windows']['result']['windows'] if w.get('app_id') in targets]
        for window in windows:
            token=str(window.get('launch_id','0'))
            if not token.isdecimal() or int(token)<=0:
                raise RuntimeError('Window has no registered launch lineage; exit is unverified')
            tokens.add(str(int(token)))
        if not tokens:raise RuntimeError('No target launch identities; exit is unverified')
        remaining=[t for t in census['tasks'] if not t.get('kernel') and str(t.get('launch_id','0')) in tokens]
        result={'identity':identity,'app_ids':sorted(targets),'launch_tokens':sorted(tokens),
                'remaining_windows':windows,'remaining_tasks':remaining,
                'ready_for_checked_stop':not windows and not remaining,
                'scope':'Full recorded UI launch lineages; checked sync, VM exit and offline fsck remain separate'}
        # Persist target identities before any close can make them disappear.
        receipt.write_text(json.dumps(result,indent=2))
        if result['ready_for_checked_stop']:return result
        for window in windows:
            ref=window['reference'];key=(ref['window_id'],ref['incarnation'])
            if key in requested:continue
            path=output/f'close-{key[0]}-{key[1]}.json'
            if path.exists():
                request=json.loads(path.read_text())
                if request['reference']!=ref:raise RuntimeError('Close request belongs to another window epoch')
            else:
                request={'reference':ref,'request_id':uuid.uuid4().hex}
                path.write_text(json.dumps(request,indent=2))
            request['result']=bridge.window_action(session,ref,'close',request['request_id'],wait_for='applied',wait_s=2)
            path.write_text(json.dumps(request,indent=2));requested.add(key)
        if clock()>=deadline:
            raise TimeoutError('Application exit observation expired; retain VM and resume this barrier')
        sleep(.25)


if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session',required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--app-id',action='append',required=True);ap.add_argument('--launch-token',action='append',default=[])
    args=ap.parse_args()
    sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools/vm_bridge'))
    import server
    print(json.dumps(close_apps(server,args.session,args.output,args.app_id,args.launch_token),indent=2))
