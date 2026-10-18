"""Boot-bound orderly guest power operations, with retained dispatch receipts.

A timeout preserves the original request. Status never resends a start/commit.
Diagnostic force-stop/reset remain separate tools. No focus-dependent input.
"""
import json
from pathlib import Path
import time
import uuid

from common import REGISTRY, load_session, process_is_alive, session_lock, write_json
from lifecycle import exchange, valid_id


def operate(session, action, request_id=None, wait_s=10, guest=None):
    if action not in {'shutdown','restart','status','cancel','commit'}:
        raise ValueError('action must be shutdown, restart, status, cancel or commit')
    if isinstance(wait_s,bool) or not isinstance(wait_s,(int,float)) or not 0<=wait_s<=20:
        raise ValueError('wait_s must be 0–20 seconds')
    if request_id is None:
        if action not in {'shutdown','restart'}:raise ValueError('The original request_id is required')
        request_id=uuid.uuid4().hex
    valid_id(request_id)
    deadline=time.monotonic()+wait_s
    with session_lock(session,live=False):
        data=load_session(session,live=False)
        directory=REGISTRY/session/'lifecycle-operations';directory.mkdir(exist_ok=True)
        path=directory/(request_id+'.json')
        if path.exists():
            record=json.loads(path.read_text())
            if record['session']!=session or record['request_id']!=request_id:raise ValueError('Lifecycle receipt identity mismatch')
            if action in {'shutdown','restart'} and record['action']!=action:raise ValueError('Request ID is bound to another action')
            verb='power_status'
        elif action in {'shutdown','restart'}:
            if not process_is_alive(data['pid'],data['process_identity']):raise RuntimeError('Selected VM is stopped')
            if action=='restart' and data.get('qemu_reboot_action')!='reset':raise RuntimeError('Guest restart requires launch reboot policy reset; nothing dispatched')
            hello=exchange(session,'hello',uuid.uuid4().hex,uuid.uuid4().hex,timeout=3)
            record={'session':session,'request_id':request_id,'action':action,'guest_boot_id':hello['guest_boot_id'],
                    'generation':data['generation'],'dispatch':'not_sent','commit_dispatch':'not_sent','state':'preparing',
                    'guest_sync_completed':False,'started_host_monotonic':time.monotonic()}
            write_json(path,record);verb=action
        else:raise ValueError('No retained lifecycle request; status cannot create or redispatch one')
        if action=='cancel':verb='cancel'
        if action=='commit':
            if record['commit_dispatch']!='not_sent':verb='power_status'
            elif record.get('last_response',{}).get('state')!='ready':raise RuntimeError('Observe ready for this request before committing')
            else:verb='commit'
        while True:
            data=load_session(session,live=False)
            if not process_is_alive(data['pid'],data['process_identity']):
                record['qemu_exited']=True;record['qemu_exit_code']=data.get('exit_code')
                record['orderly_shutdown_completed']=(record['action']=='shutdown' and record['guest_sync_completed'] and record['commit_dispatch']!='not_sent' and data.get('exit_code')==0)
                record['state']='stopped' if record['action']=='shutdown' else 'restart_failed_vm_stopped'
                # This is the VM supervisor's observed QEMU result, not proof
                # of completion of an outer shell/original exec launcher.
                record['original_launcher_exit_verified']=False
                write_json(path,record);break
            if record['commit_dispatch']!='not_sent':
                try:
                    observed=exchange(session,'power_status',request_id,record['guest_boot_id'],timeout=.5)
                    record['last_response']=observed;record['state']=observed['state']
                    record['guest_sync_completed']=record['guest_sync_completed'] or observed['guest_sync_completed']
                    record.pop('observation_error',None)
                    write_json(path,record)
                    if observed['state']=='failed':break
                except (OSError,RuntimeError,ValueError) as error:record['observation_error']=str(error)
            if record['commit_dispatch']!='not_sent' and record['action']=='restart':
                try:
                    hello=exchange(session,'hello',uuid.uuid4().hex,uuid.uuid4().hex,timeout=.5)
                    if hello['guest_boot_id']!=record['guest_boot_id']:
                        caps=guest(data,'bridge ui caps',timeout=2) if guest else {}
                        if caps.get('boot_id')==hello['guest_boot_id']:
                            record.update(state='restarted',new_guest_boot_id=hello['guest_boot_id'],restart_verified=True,guest_ready=True)
                            record.pop('observation_error',None)
                            write_json(path,record);break
                except (OSError,RuntimeError,ValueError) as error:record['observation_error']=str(error)
            elif record['commit_dispatch']=='not_sent':
                if verb in {'shutdown','restart'}:record['dispatch']='unknown';write_json(path,record)
                if verb=='commit':
                    # Invalidate cached references before any power delivery.
                    data['generation']=uuid.uuid4().hex
                    data['generation_log_offset']=Path(data['serial_log']).stat().st_size
                    write_json(REGISTRY/session/'session.json',data)
                    for name in ('last-frame.json','frame-source.json'):(REGISTRY/session/name).unlink(missing_ok=True)
                    record['generation']=data['generation'];record['commit_dispatch']='unknown';write_json(path,record)
                try:
                    result=exchange(session,verb,request_id,record['guest_boot_id'],timeout=2)
                    record['last_response']=result;record['state']=result['state']
                    record['guest_sync_completed']=result['guest_sync_completed']
                    if verb in {'shutdown','restart'}:record['dispatch']='acknowledged'
                    if verb=='commit':record['commit_dispatch']='acknowledged'
                    record.pop('observation_error',None)
                except (OSError,RuntimeError,ValueError) as error:
                    record['observation_error']=str(error);record['completion_unknown']=True
                write_json(path,record)
                if record['state'] in {'cancelled','failed','stale','missing','full','unsupported','busy','conflict','too_late','not_ready'}:break
                # A start may advance all the way to power in this call. A
                # status lookup only observes; commit remains explicit after a
                # start returned while save dialogs or tasks were blocking.
                if action in {'shutdown','restart'} and record.get('last_response',{}).get('state')=='ready' and time.monotonic()<deadline:
                    verb='commit';continue
                verb='power_status'
            if time.monotonic()>=deadline:break
            time.sleep(min(.2,max(0,deadline-time.monotonic())))
        record['receipt']=str(path)
        return record
