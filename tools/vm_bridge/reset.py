"""Bounded, observable VM reset; acknowledgement is not proof of a new boot."""
import json
from pathlib import Path
import re
import time
import uuid

from common import write_json


def launch_reboot_action(args):
    """Only infer launch policies whose interpretation is unambiguous here."""
    options=[a.split('=',1)[0].lstrip('-') for a in args if a.startswith('-')]
    if any(a in {'readconfig','action'} for a in options):
        return None
    return 'shutdown' if 'no-reboot' in options else 'reset'


def boot_id(reply):
    value=reply.get('boot_id') if isinstance(reply,dict) else None
    return value if isinstance(value,str) and re.fullmatch('[0-9a-f]{32}',value) and value!='0'*32 else None


def policy(data):
    # Do not guess a restoration policy for older/custom registrations.
    return data.get('qemu_reboot_action')


def read_epoch(data,guest,timeout):
    deadline=time.monotonic()+timeout
    reply=guest(data,'bridge epoch',timeout=timeout)
    if isinstance(reply,dict) and reply.get('boot_id')=='0'*32:
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('Boot identity initialization deadline')
        reply=guest(data,'bridge epoch init '+uuid.uuid4().hex,timeout=remaining)
    return boot_id(reply)


def sample(q,data,guest,record,timeout):
    deadline=time.monotonic()+timeout
    # Clear current evidence before the first fallible read. Historical proof
    # lives in separate verified_* fields and must survive transport loss.
    record['vm']=None;record['guest_ready']=False;record['current_restart_observed']=False
    record['current_guest_boot_id']=None;record['current_observation_state']='unavailable'
    record.pop('current_error',None);record.pop('guest_error',None)
    if hasattr(q,'sock'):q.sock.settimeout(timeout)
    record['vm']=q.call('query-status')
    record['sampled_host_monotonic']=time.monotonic()
    record['current_observation_state']='observed'
    if not record['vm'].get('running'):return
    try:
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('Guest identity sample deadline')
        current=read_epoch(data,guest,remaining)
        record['current_guest_boot_id']=current
        record['guest_ready']=current is not None
        record.pop('guest_error',None)
        changed=bool(current and record.get('previous_guest_boot_id') and current!=record['previous_guest_boot_id'])
        record['current_restart_observed']=changed
        if changed and not record.get('restart_verified'):
            record['restart_verified']=True
            record['verified_guest_boot_id']=current
            record['verified_host_monotonic']=time.monotonic()
    except (OSError,RuntimeError,ValueError) as error:
        record['guest_error']=str(error)


def perform(session,data,registry,guest,qmp_factory,wait_s=20,cancel=None):
    """Caller holds the VM lock. Never repeats an ambiguously delivered reset."""
    if isinstance(wait_s,bool) or not isinstance(wait_s,(int,float)) or not 0<=wait_s<=20:
        raise ValueError('reset wait_s must be 0–20')
    previous_policy=policy(data)
    if previous_policy not in {'reset','shutdown'}:
        raise ValueError('Cannot restore unknown QEMU reboot policy; reset not dispatched')
    run=Path(registry)/session;path=run/'reset.json';takeover=run/'control.json'
    record={'operation_id':uuid.uuid4().hex,'previous_generation':data['generation'],
        'generation':data['generation'],'previous_guest_boot_id':None,
        'restart_verified':False,'guest_ready':False,'state':'preparing',
        'reset_delivery':'not_sent','launch_reboot_action':previous_policy,
        'reboot_policy_restored':True,'started_host_monotonic':time.monotonic()}
    try:record['previous_guest_boot_id']=read_epoch(data,guest,3)
    except (OSError,RuntimeError,ValueError) as error:record['previous_boot_error']=str(error)
    if cancel is not None and cancel.is_set():
        record['state']='cancelled';write_json(path,record);return record
    write_json(path,record);changed=False
    try:
        with qmp_factory(session) as q:
            if previous_policy!='reset':
                # Mark before sending: a lost reply can still change policy.
                changed=True;record['reboot_policy_restored']=False;write_json(path,record)
                q.call('set-action',{'reboot':'reset'})
            # Invalidate refs before sending: a lost reset reply must never
            # leave an old frame/window reference usable after reboot.
            data['generation_log_offset']=Path(data['serial_log']).stat().st_size
            data['generation']=uuid.uuid4().hex
            record['generation']=data['generation'];record['state']='pending'
            write_json(run/'session.json',data)
            for name in ('last-frame.json','frame-source.json'):(run/name).unlink(missing_ok=True)
            record['reset_delivery']='unknown';write_json(path,record)
            q.call('system_reset');record['reset_delivery']='acknowledged'
            # QEMU latches reset-vs-shutdown when processing the request. Restore
            # immediately so subsequent guest crashes retain the launch policy.
            # Its monitor accepts one client: use this healthy connection.
            if changed:
                q.call('set-action',{'reboot':previous_policy})
                record['reboot_policy_restored']=True;changed=False
            deadline=time.monotonic()+wait_s;resumed=False
            while True:
                remaining=max(0,deadline-time.monotonic())
                sample(q,data,guest,record,min(3,max(.1,remaining)))
                active=takeover.exists() and json.loads(takeover.read_text()).get('user_takeover')
                cancelled=cancel is not None and cancel.is_set()
                # Reset can transition through shutdown before prelaunch. Keep
                # watching, but resume at most once and only eligible pauses.
                if not resumed and record['vm'].get('status') in {'prelaunch','paused'}:
                    if active:record['resume_skipped_takeover']=True
                    elif cancelled:record['wait_cancelled']=True
                    else:q.call('cont');resumed=True;record['resume_dispatched']=True
                if record['restart_verified']:
                    record['state']='restarted';break
                if record['guest_ready'] and record['previous_guest_boot_id'] is None:
                    record['state']='running_unverified';break
                if cancelled:
                    record['wait_cancelled']=True;break
                if time.monotonic()>=deadline:break
                time.sleep(min(.2,max(0,deadline-time.monotonic())))
            # Reapply host takeover intent to the reset guest latch without
            # calling the UI wrapper, which would acquire this same lock.
            if record['guest_ready'] and takeover.exists() and json.loads(takeover.read_text()).get('user_takeover'):
                try:
                    reply=guest(data,'bridge ui takeover 1 '+record['current_guest_boot_id'],timeout=3)
                    if not isinstance(reply,dict) or reply.get('user_takeover') is not True:
                        raise RuntimeError('Guest did not acknowledge takeover')
                    record['guest_takeover_reply']=reply
                except (OSError,RuntimeError,ValueError) as error:record['guest_takeover_error']=str(error)
    except (OSError,RuntimeError,ValueError) as error:
        record['error']=str(error)
        record['state']='unknown' if record['reset_delivery']=='unknown' else 'unavailable'
    finally:
        # The primary connection is closed before fallback: QMP is single-client.
        if changed:
            try:
                with qmp_factory(session) as restore:restore.call('set-action',{'reboot':previous_policy})
                record['reboot_policy_restored']=True
            except (OSError,RuntimeError,ValueError) as error:record['policy_restore_error']=str(error)
        record['completed_host_monotonic']=time.monotonic();write_json(path,record)
    return record


def status(session,data,registry,guest,qmp_factory,available=True):
    """No reset/resume/policy change; epoch initialization is bookkeeping only."""
    path=Path(registry)/session/'reset.json'
    if not path.exists():raise ValueError('No recorded bridge reset')
    record=json.loads(path.read_text())
    if record.get('generation')!=data['generation']:
        return {**record,'state':'stale','current_observation_state':'generation_changed'}
    if not available:
        return {**record,'vm':None,'guest_ready':False,'current_guest_boot_id':None,
            'current_restart_observed':False,'current_observation_state':'vm_unavailable'}
    if record.get('reset_delivery')=='not_sent':return record
    try:
        with qmp_factory(session) as q:sample(q,data,guest,record,3)
        if record.get('restart_verified'):record['state']='restarted'
        elif record['guest_ready'] and record.get('previous_guest_boot_id') is None:record['state']='running_unverified'
        else:record['state']='pending'
    except (OSError,RuntimeError,ValueError) as error:
        record.update(current_observation_state='unavailable',current_error=str(error),guest_ready=False)
    write_json(path,record)
    return record
