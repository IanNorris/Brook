"""Opt-in Brook physical-machine TCP sessions; no QEMU or host desktop APIs."""
import contextlib
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import stat
import time
import uuid

from common import ROOT,write_json
from guest_transport import exchange
from jobs import Jobs,GuestJobError,validate_id
from ui import Ui

class HardwareBootChanged(GuestJobError):
    pass

class Hardware:
    def __init__(self,registry=None):
        self.registry=Path(registry or os.environ.get('BROOK_HARDWARE_REGISTRY',ROOT/'build/hardware-bridge')).absolute()
        self.jobs=Jobs(self.registry,self.load_session,self.session_lock,self.guest,write_json,self.guest_batch)
        self.ui=Ui(self.registry,self.load_session,self.session_lock,self.guest,write_json,self.guest_batch)

    def load_session(self,session,live=True):
        validate_id(session);directory=self.registry/session;path=directory/'session.json'
        if self.registry.is_symlink() or directory.is_symlink() or path.is_symlink():raise ValueError('Hardware registry must not be a symlink')
        info=path.stat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid():raise ValueError('Hardware descriptor must be owned by this user')
        data=json.loads(path.read_text())
        if data.get('session')!=session or data.get('owner_uid')!=os.getuid() or data.get('transport')!='guest_tcp':raise ValueError('Hardware identity mismatch')
        if live and data.get('state')!='running':raise RuntimeError('Hardware session is detached')
        self.endpoint(data['host'],data['port']);validate_id(data['guest_boot_id'])
        if data['guest_boot_id']=='0'*32:raise ValueError('Unset hardware boot identity')
        return data

    @staticmethod
    def endpoint(host,port):
        if not isinstance(host,str):raise ValueError('Use an explicit IPv4 string')
        address=ipaddress.IPv4Address(host)
        if address.is_multicast or address.is_unspecified or address.is_reserved or int(address)==0xffffffff:raise ValueError('Use an explicit unicast IPv4 address')
        if isinstance(port,bool) or not isinstance(port,int) or not 1<=port<=65535:raise ValueError('Invalid TCP port')
        return str(address),port

    @contextlib.contextmanager
    def session_lock(self,session,timeout=10,live=True):
        self.load_session(session,live)
        fd=os.open(self.registry/session/'operation.lock',os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'a') as lock:
            info=os.fstat(lock.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid():raise ValueError('Invalid hardware operation lock')
            deadline=time.monotonic()+timeout
            while True:
                try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);break
                except BlockingIOError:
                    if time.monotonic()>=deadline:raise TimeoutError('Hardware connection is busy')
                    time.sleep(.02)
            yield self.load_session(session,live)

    @staticmethod
    def guarded(data,command):
        # Jobs/UI already carry a kernel-enforced boot token. Their hex upload
        # chunks use the full protocol line; do not add another envelope.
        boot=data['guest_boot_id']
        if command.startswith(('bridge job ','bridge ui ')) and command.endswith(' '+boot):return command
        return 'bridge guard '+boot+' '+command

    def guest_batch(self,data,commands,timeout=8):
        replies=exchange(data['host'],data['port'],[self.guarded(data,c) for c in commands],timeout)
        for reply in replies:
            if 'boot_error' in reply:raise HardwareBootChanged('Stale guest boot')
        return replies

    def guest(self,data,command,timeout=8):return self.guest_batch(data,[command],timeout)[0]

    def attach(self,host,port=1234,**_):
        host,port=self.endpoint(host,port);proposal=uuid.uuid4().hex
        epoch,caps=exchange(host,port,['bridge epoch init '+proposal,'bridge hardware caps'],8)
        boot=epoch.get('boot_id');validate_id(boot)
        if boot=='0'*32 or caps.get('hardware_api')!=1 or not caps.get('boot_guard') or caps.get('boot_id')!=boot:
            raise RuntimeError('Guest lacks matching hardware/boot-guard support; update the hardware image')
        if self.registry.is_symlink():raise ValueError('Hardware registry must not be a symlink')
        self.registry.mkdir(parents=True,exist_ok=True,mode=0o700)
        session=uuid.uuid4().hex;directory=self.registry/session;directory.mkdir(mode=0o700)
        data=dict(session=session,owner_uid=os.getuid(),transport='guest_tcp',host=host,port=port,state='running',
            generation=uuid.uuid4().hex,guest_boot_id=boot,created_unix=time.time(),guest_capabilities=caps)
        write_json(directory/'session.json',data)
        return {'session':session,'generation':data['generation'],'guest_boot_id':boot,'endpoint':f'{host}:{port}',
            'guest':caps,'note':'Pinned to this guest boot. Attach explicitly again after reboot; old operation IDs remain historical.'}

    def sessions(self,**_):
        rows=[]
        for path in sorted(self.registry.glob('*/session.json')):
            try:
                data=self.load_session(path.parent.name,live=False)
                rows.append({k:data[k] for k in ('session','host','port','state','guest_boot_id','generation')})
            except (OSError,ValueError,KeyError):continue
        return {'sessions':rows,'reachability':'Not probed; call hw_doctor for the selected session'}

    def doctor(self,session,**_):
        data=self.load_session(session)
        caps={'qmp':False,'host_desktop':False,'pause':False,'force_stop':False,'registers':False,
              'raw_memory':False,'frame_capture':False,'panic_transport':False,'raw_input':False}
        result={'session':session,'transport':'guest_tcp','guest_boot_id':data['guest_boot_id'],'capabilities':caps}
        try:
            with self.session_lock(session) as data:
                hardware,jobs,ui=self.guest_batch(data,['bridge hardware caps','bridge job caps','bridge ui caps'],5)
            caps.update(diagnostics=True,logs=hardware.get('logs_api')==1,stats=True,
                hardware_power=hardware.get('power_api')==1,power=hardware.get('power',{}),
                command_jobs=jobs.get('job_api')==2,semantic_windows=ui.get('ui_api')==1,semantic_launch=ui.get('launch_api')==1,app_catalog=ui.get('apps_api')==1)
            result.update(reachable=True,identity_matches=True,guest=hardware,jobs=jobs,ui=ui)
        except HardwareBootChanged as error:
            result.update(reachable=True,identity_matches=False,error=str(error),note='Attach explicitly again to target the new guest boot.')
        except (OSError,RuntimeError,ValueError) as error:
            result.update(reachable=False,error=str(error),note='TCP failure does not distinguish reboot, panic, link loss or an unreachable host.')
        return result

    def command(self,session,command,**_):
        with self.session_lock(session) as data:return self.guest(data,command)

    def power(self,session,request_id,operation,**_):
        validate_id(request_id)
        verbs={'shutdown':'shutdown','restart':'restart','status':'power_status','cancel':'cancel','commit':'commit'}
        if not isinstance(operation,str) or operation not in verbs:raise ValueError('Expected shutdown, restart, status, cancel or commit')
        with self.session_lock(session) as data:
            result=self.guest(data,f"bridge hardware power {request_id} {verbs[operation]} {data['guest_boot_id']}")
        if 'power_error' in result:raise GuestJobError(result['power_error'])
        if result.get('power_api')!=1 or result.get('request_id')!=request_id or result.get('operation')!=verbs[operation]:
            raise GuestJobError('Missing or mismatched power response; inspect this request ID before considering another mutation')
        return {'session':session,'guest_boot_id':data['guest_boot_id'],**result}

    def stats(self,session,audio_details=False,**_):
        commands=['bridge stats']+(['audio stats'] if audio_details else [])
        with self.session_lock(session) as data:replies=self.guest_batch(data,commands)
        result={'session':session,'guest_boot_id':data['guest_boot_id'],**replies[0]}
        if audio_details:result['audio']=replies[1]
        return result

    def logs(self,session,cursor=0,limit=64,**_):
        if any(isinstance(n,bool) or not isinstance(n,int) for n in (cursor,limit)) or not 0<=cursor<=0xffffffff or not 1<=limit<=64:raise ValueError('Use uint32 line cursor and limit 1..64')
        with self.session_lock(session) as data:reply=self.guest(data,f'bridge hardware logs {cursor} {limit}')
        if 'logs_error' in reply:raise RuntimeError(reply['logs_error'])
        return {'session':session,'guest_boot_id':data['guest_boot_id'],**reply}

    def guest_run(self,session,request_id,argv,cwd='/data/home',env=None,uid=1000,wait_s=0,_cancel=None,**_):
        return self.jobs.submit(session,request_id,argv,cwd,env,uid,wait_s,_cancel)

    def job(self,session,operation_id,stdout_offset=0,stderr_offset=0,output_limit=4096,wait_s=0,_cancel=None,**_):
        return self.jobs.status(session,operation_id,stdout_offset,stderr_offset,output_limit,wait_s,_cancel)

    def windows(self,session,**_):return self.ui.windows(session)
    def apps(self,session,**_):return self.ui.apps(session)
    def launch(self,session,app,request_id,wait_for='window',wait_s=2,_cancel=None,**_):
        return self.ui.launch(session,app,request_id,wait_for,wait_s,_cancel)

    def window(self,session,window,action,request_id,restore_target=None,wait_for='applied',wait_s=2,_cancel=None,**_):
        return self.ui.submit(session,window,action,request_id,restore_target,wait_for,wait_s,_cancel)

    def ui_status(self,session,operation_id,wait_s=0,wait_for='applied',_cancel=None,**_):
        return self.ui.status(session,operation_id,wait_s,wait_for,_cancel)
