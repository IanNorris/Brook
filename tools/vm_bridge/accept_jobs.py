#!/usr/bin/env python3
"""Explicit-session VM command-job acceptance; writes evidence beneath build/."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

import server


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--shell',default='/boot/BIN/BASH')
    parser.add_argument('--resume',action='store_true',help='Reuse completed evidence from this same live session')
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    records=json.loads((args.out/'jobs.json').read_text()) if args.resume else []
    if any(record.get('request',{}).get('session',args.session)!=args.session for record in records):
        raise ValueError('Cannot resume evidence from a different VM session')
    def save():
        (args.out/'jobs.json').write_text(json.dumps(records,indent=2))
    def run(name,source,extra=(),**options):
        expected={'session':args.session,'argv':[args.shell,'-c',source,'brook-job',*extra],**options}
        old=next((record for record in records if record['name']==name and
                  {key:value for key,value in record.get('request',{}).items() if key!='request_id'}==expected),None)
        if old and old.get('result',{}).get('state')=='completed':
            print(name,'reuse completed same-session evidence',flush=True)
            return old
        ident=uuid.uuid4().hex;began=time.monotonic()
        request={'session':args.session,'request_id':ident,
                 'argv':[args.shell,'-c',source,'brook-job',*extra],**options}
        result=server.guest_run(**request,wait_s=20)
        record={'name':name,'request':request,'result':result,'elapsed_s':time.monotonic()-began}
        records.append(record);save()
        assert result['state']=='completed',record
        print(name,'exit',result.get('exit'),'seconds',round(record['elapsed_s'],3),flush=True)
        return record

    basic=run('identity-output','printf "uid=%s home=%s cwd=%s env=%s\\n" "$UID" "$HOME" "$PWD" "$CUSTOM"; printf "stderr-€\\n" >&2; exit 7',
              env={'CUSTOM':'spaces ; $() " literal'})
    result=basic['result']
    assert result['exit']=={'code':7,'signal':None,'wait_status':7<<8}
    assert result['stdout']['text']=='uid=1000 home=/data/home cwd=/data/home env=spaces ; $() " literal\n'
    assert result['stderr']['text']=='stderr-€\n'
    assert result['stdout']['eof'] and result['stderr']['eof']

    literal='literal ; $() "\n€ '*60
    large=run('long-literal-argv','printf "%s" "$1"',(literal,))
    assert large['result']['stdout']['text']==literal

    path='/tmp/brook-job-'+uuid.uuid4().hex
    once=run('dedup-write','printf "once\\n" >> "$1"',(path,))
    repeated=server.guest_run(**once['request'],wait_s=10)
    assert repeated['operation_id']==once['result']['operation_id']
    # Explicit replay against the guest, not just the host cache.
    with server.session_lock(args.session) as data:
        replay=server.guest(data,'bridge job commit '+once['request']['request_id']+' '+once['result']['guest_boot_id'])
    assert replay['state']=='completed'
    read=run('dedup-read','while IFS= read -r line; do printf "%s\\n" "$line"; done < "$1"; rm -f "$1"',(path,))
    assert read['result']['stdout']['text']=='once\n'

    signalled=run('signal-exit','kill -TERM $$')
    assert signalled['result']['exit']['signal']==15

    def background(name,source):
        old=next((record for record in records if record['name']==name and record.get('result',{}).get('state')=='completed'),None)
        if old:return old['first'],old['result']
        ident=uuid.uuid4().hex
        pending=server.guest_run(args.session,argv=[args.shell,'-c',source],request_id=ident,wait_s=.2)
        record={'name':name,'operation_id':ident,'pending':pending};records.append(record);save()
        deadline=time.monotonic()+10
        while True:
            first=server.job_status(args.session,ident)
            if first['state'] not in {'uploading','queued'} and first.get('guest',{}).get('exit_collected'):break
            if time.monotonic()>=deadline:raise AssertionError(first)
            time.sleep(.05)
        record['first']=first;save()
        finished=server.job_status(args.session,ident,wait_s=15)
        record['result']=finished;save()
        return first,finished

    first,finished=background('parent-exit-held-output','(sleep 10; printf child) & printf parent; exit 9')
    assert first['state']=='running' and first['guest']['leader_exited'],first
    assert first['exit']['code']==9 and not first['stdout']['eof'],first
    assert finished['state']=='completed' and finished['stdout']['text']=='parentchild',finished

    first,finished=background('eof-before-descendant-exit','(exec >/dev/null 2>&1; sleep 10) & printf parent; exit 0')
    assert first['state']=='running' and first['guest']['live_tasks']>0,first
    assert first['stdout']['eof'] and first['stderr']['eof'],first
    assert finished['state']=='completed',finished

    pressure=run('retained-output','printf "%0140000d" 0')
    assert pressure['result']['guest']['stdout_bytes']==140000
    assert pressure['result']['stdout']['dropped_bytes']==140000-65536
    assert pressure['result']['stdout']['text']=='0'*4096

    stress=run('signalled-full-pipe-producers',
        'for sig in TERM KILL; do for n in 1 2 3; do '
        '(trap "exit 0" TERM; if ((n % 2)); then printf "%05000000d" 0; '
        'else printf "%05000000d" 0 >&2; fi) & producer=$!; '
        'sleep 0.03; kill -"$sig" "$producer"; wait "$producer"; '
        'done; done; printf "\\nSTRESS_DONE\\n"')
    assert stress['result']['exit']['code']==0
    assert stress['result']['guest']['live_tasks']==0
    assert stress['result']['guest']['stdout_eof'] and stress['result']['guest']['stderr_eof']
    assert stress['result']['guest']['stdout_full_waits']>0
    assert stress['result']['guest']['stderr_full_waits']>0

    # A new host server must recover actual output/exit without another launch.
    recovered=subprocess.run([sys.executable,str(Path(server.__file__)),'--call','vm_job',
        '--arguments',json.dumps({'session':args.session,'operation_id':basic['request']['request_id']})],
        capture_output=True,text=True,timeout=15,check=True)
    envelope=json.loads(recovered.stdout)
    after=json.loads(envelope['content'][0]['text'])
    assert after['exit']==basic['result']['exit'] and after['stdout']['text']==result['stdout']['text']
    records.append({'name':'server-reconnect','result':after});save()
    print('PASS',len(records),'job scenarios; VM remains running for inspection',flush=True)


if __name__=='__main__':main()
