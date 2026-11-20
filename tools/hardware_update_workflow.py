"""Durable host receipts for one maintenance upload; mutations are never retried.

The bridge is injected so this module has no host-desktop or QEMU dependencies.
A failed run is inspected via its original guest job / power request IDs, rather
than restarted with a new transaction. Reboot success requires both a changed
boot epoch and the expected payload ID from a new boot-pinned guest job.
"""
import hashlib,json,os,re,time,uuid
from pathlib import Path
from hardware_update_protocol import upload,read_payload,KERNEL_MAX,INITRD_MAX
from hardware_bulk_protocol import validate_pacing

def power_blocker_summary(status):
    """One line naming the guest's power stage and the processes still blocking it."""
    if not isinstance(status,dict):return 'no power status recorded'
    parts=[f"state={status.get('state','?')}"]
    if status.get('stage'):parts.append(f"stage={status['stage']}")
    blockers=status.get('blockers') or []
    if blockers:
        named=', '.join(f"{b.get('name','?')} pid {b.get('pid','?')}"+(' (window)' if b.get('window') else ' (no window)' if not b.get('service') else ' (service)') for b in blockers)
        extra=(status.get('blocker_count') or len(blockers))-len(blockers)
        parts.append('blockers='+named+(f' (+{extra} more)' if extra>0 else ''))
    if status.get('next_signal_ms'):parts.append(f"next_signal_in={status['next_signal_ms']/1000:.1f}s")
    if status.get('status'):parts.append(f"status={status['status']!r}")
    return ' '.join(parts)

class UpdateRun:
    def __init__(self,bridge,output,uploader=upload,clock=time.monotonic,sleep=time.sleep):
        self.bridge=bridge;self.output=Path(output);self.uploader=uploader;self.clock=clock;self.sleep=sleep
        self.receipt={}

    def save(self,**changes):
        self.receipt.update(changes)
        temporary=self.output/'receipt.new'
        with temporary.open('w') as stream:
            os.chmod(temporary,0o600);json.dump(self.receipt,stream,indent=2);stream.write('\n');stream.flush();os.fsync(stream.fileno())
        temporary.replace(self.output/'receipt.json')
        directory=os.open(self.output,os.O_RDONLY|os.O_DIRECTORY)
        try:os.fsync(directory)
        finally:os.close(directory)

    def wait_job(self,session,ident,predicate,timeout=60):
        end=self.clock()+timeout
        while self.clock()<end:
            result=self.bridge.job(session,ident,wait_s=2)
            if predicate(result):return result
            if result.get('state') in ('completed','failed','rejected','lost'):raise RuntimeError(result)
            self.sleep(.1)
        raise TimeoutError('Inspect original guest job '+ident)

    def system_receiver_mode(self,session,boot,guest,first_install):
        # Both receivers answer before reading the key or taking the update
        # lock. This probe is a different immutable structured job from the
        # upload, so a lost negotiation can never downgrade that upload.
        ident=uuid.uuid4().hex
        self.save(state='receiver_caps_dispatched',receiver_caps_job_id=ident)
        self.bridge.guest_run(session,ident,['/boot/HWUPSYS.ELF','--caps'],cwd='/tmp',uid=0,wait_s=2)
        job=self.wait_job(session,ident,lambda r:r.get('state')=='completed')
        self.save(receiver_caps_job=job)
        stdout,stderr=(job.get(name,{}) for name in ('stdout','stderr'))
        if (job.get('session')!=session or job.get('guest_boot_id')!=boot or
                job.get('operation_id')!=ident or
                any(not stream.get('eof') or stream.get('gaps') or stream.get('read_error')
                    for stream in (stdout,stderr))):
            raise RuntimeError('The system receiver capability reply is incomplete or from a different boot')
        reply=(job.get('exit',{}).get('code'),stdout.get('text'),stderr.get('text'))
        advertised=(0,'HWUPDATE_CAPS slot_aware=1\n','')
        # The first reviewed slot-aware binary advertised its argument
        # in the exact usage response before it gained --caps.
        advertised_usage=(2,'','usage: HWUPDATE.ELF BOOT_ID32 TRANSACTION_ID32 [--require-slot-aware]\n')
        if reply in (advertised,advertised_usage):
            if first_install:raise RuntimeError('This boot already has a slot-aware system receiver')
            return 'slot_aware'
        if reply==(2,'','usage: HWUPDATE.ELF BOOT_ID32 TRANSACTION_ID32\n'):
            store=guest.get('writable_desktop',{})
            if (first_install and guest.get('boot_id')==boot and 'ac_shutdown_record' in guest and
                    guest.get('recovery_only',False) is False and store.get('ready') is True and
                    store.get('source_pinned') is True):
                return 'first_install'
            raise RuntimeError('The booted system receiver lacks running-slot support. '
                               'Only a verified pre-slot guarded boot with an explicit first-install attestation is eligible.')
        raise RuntimeError('The running system receiver did not advertise a supported capability')

    def reserve_first_install(self,boot):
        # A repeated legacy activation before reboot can select the running
        # slot. Preserve this claim across host retries and session reattaches.
        registry=Path(self.bridge.registry)
        claim=registry/f'.slot-aware-first-install-{boot}.json'
        try:
            fd=os.open(claim,os.O_CREAT|os.O_EXCL|os.O_WRONLY|os.O_NOFOLLOW,0o600)
        except FileExistsError as error:
            raise RuntimeError('A first-install attempt already exists for this boot; inspect '+str(claim)) from error
        with os.fdopen(fd,'w') as stream:
            json.dump({'boot_id':boot,'transaction_id':self.receipt['transaction_id'],
                       'receipt':str(self.output.resolve()/'receipt.json')},stream)
            stream.write('\n');stream.flush();os.fsync(stream.fileno())
        directory=os.open(registry,os.O_RDONLY|os.O_DIRECTORY)
        try:os.fsync(directory)
        finally:os.close(directory)
        self.save(first_install_claim=str(claim))

    def execute(self,session,key_file,kernel_path,initrd_path,activate=False,restart=False,expected_payload_id=None,port=1235,boot_timeout_s=120,helper='usb',pace_bytes_per_second=None,*,first_slot_aware_install=None):
        helpers={'usb':'/boot/HWUPDATE.ELF','system':'/boot/HWUPSYS.ELF'}
        if helper not in helpers:raise ValueError('Expected usb or system update helper')
        if first_slot_aware_install is not None and (helper!='system' or not activate or
                not isinstance(first_slot_aware_install,str) or
                not re.fullmatch(r'[0-9a-f]{32}',first_slot_aware_install)):
            raise ValueError('First slot-aware installation requires explicit system activation and a boot ID')
        validate_pacing(pace_bytes_per_second)
        if isinstance(boot_timeout_s,bool) or not isinstance(boot_timeout_s,int) or not 1<=boot_timeout_s<=900:raise ValueError('Boot observation timeout must be 1..900 seconds')
        if restart and (not activate or not expected_payload_id):raise ValueError('Restart requires activation and an expected payload ID')
        if expected_payload_id and (not re.fullmatch(r'[a-zA-Z0-9_.-]{1,95}',expected_payload_id)):raise ValueError('Invalid expected payload ID')
        if isinstance(port,bool) or not isinstance(port,int) or not 1<=port<=65535:raise ValueError('Invalid upload port')
        kernel=read_payload(kernel_path,KERNEL_MAX);initrd=read_payload(initrd_path,INITRD_MAX)
        if len(kernel)<64 or len(initrd)<512:raise ValueError('Payload is too short')
        with Path(key_file).open() as source:key=source.read(33)
        if not re.fullmatch(r'[0-9a-f]{32}',key):raise ValueError('Expected provisioned 32-character key without a newline')
        data=self.bridge.load_session(session);boot=data['guest_boot_id']
        doctor=self.bridge.doctor(session)
        if doctor.get('probe_stage')!='verified' or not doctor.get('reachable') or not doctor.get('identity_matches'):raise RuntimeError('Original guest boot probe is incomplete or not reachable/matching')
        if first_slot_aware_install is not None and first_slot_aware_install!=boot:
            raise RuntimeError('First-install attestation does not match the verified boot')
        if restart and not doctor.get('capabilities',{}).get('power',{}).get('reset'):raise RuntimeError('Guest does not advertise a supported restart')
        # An existing output directory is never reused, even after lost replies.
        self.output.mkdir(parents=True,exist_ok=False,mode=0o700)
        tx=uuid.uuid4().hex;power_id=uuid.uuid4().hex
        self.save(state='prepared',session=session,guest_boot_id=boot,transaction_id=tx,power_request_id=power_id,
            activate_requested=activate,restart_requested=restart,expected_payload_id=expected_payload_id,boot_timeout_s=boot_timeout_s,helper=helper,pace_bytes_per_second=pace_bytes_per_second,
            kernel={'path':str(Path(kernel_path).resolve()),'bytes':len(kernel),'sha256':hashlib.sha256(kernel).hexdigest()},
            initrd={'path':str(Path(initrd_path).resolve()),'bytes':len(initrd),'sha256':hashlib.sha256(initrd).hexdigest()})
        try:
            receiver_mode='usb'
            if helper=='system':
                receiver_mode=self.system_receiver_mode(session,boot,doctor.get('guest',{}),
                    first_slot_aware_install is not None)
            if receiver_mode=='first_install':self.reserve_first_install(boot)
            self.save(state='receiver_launch_dispatched',receiver_mode=receiver_mode)
            argv=[helpers[helper],boot,tx]
            if receiver_mode=='slot_aware':argv.append('--require-slot-aware')
            self.bridge.guest_run(session,tx,argv,cwd='/tmp',uid=0,wait_s=2)
            try:
                listening=self.wait_job(session,tx,lambda r:'HWUPDATE_LISTENING' in r.get('stdout',{}).get('text',''))
            except RuntimeError as error:
                job=error.args[0] if error.args else None
                if helper=='system' and isinstance(job,dict) and job.get('exit',{}).get('code')==2:
                    raise RuntimeError('The booted system receiver refused its negotiated launch arguments') from error
                raise
            self.save(state='receiver_listening',job=listening)
            upload_start=self.clock()
            if pace_bytes_per_second is None:result=self.uploader(data['host'],port,key,boot,tx,kernel,initrd,activate)
            else:result=self.uploader(data['host'],port,key,boot,tx,kernel,initrd,activate,pace_bytes_per_second=pace_bytes_per_second)
            self.save(state='upload_reply',upload=result,upload_phase_s=self.clock()-upload_start)
            job=self.wait_job(session,tx,lambda r:r.get('state')=='completed')
            self.save(job=job)
            if not activate:
                if result.get('state')!='staged' or job.get('exit',{}).get('code')!=22:raise RuntimeError('Unexpected staged-only completion')
                self.save(state='staged_only',storage_modified=False);return self.receipt
            if result.get('state')!='activated' or job.get('exit',{}).get('code')!=0:raise RuntimeError('Activation was not confirmed by both upload and guest job')
            match=re.search(r'HWUPDATE_ACTIVATED generation=(\d+) slot=([01]) transaction=([0-9a-f]{32})\b',job.get('stdout',{}).get('text',''))
            if not match or (int(match[1]),int(match[2]),match[3])!=(result['generation'],result['slot'],tx):raise RuntimeError('Activation job/response mismatch')
            self.save(state='activated',generation=result['generation'],slot=result['slot'])
            if not restart:return self.receipt
            self.save(state='restart_dispatched')
            status=self.bridge.power(session,power_id,'restart');self.save(power=status)
            # Windowless processes get a bounded SIGTERM/SIGKILL escalation in
            # the guest (about 15 s after the last window closes); an open
            # window, such as a save prompt, is never escalated.
            end=self.clock()+90
            while status.get('state')!='ready':
                if status.get('state') in ('failed','unsupported','cancelled'):
                    raise RuntimeError('Restart did not reach checked ready state: '+power_blocker_summary(status))
                if self.clock()>=end:
                    raise TimeoutError('Restart did not reach checked ready state within 90 s: '+power_blocker_summary(status)+
                        f'; the request is still pending in the guest; cancel with hw_power {power_id} cancel or resolve the blockers')
                # Half-second polling keeps the single-client guest bridge free for a cancel.
                self.sleep(.5);status=self.bridge.power(session,power_id,'status');self.save(power=status)
            self.save(state='restart_commit_dispatched')
            try:
                committed=self.bridge.power(session,power_id,'commit');self.save(power_commit=committed)
                if committed.get('error',0):raise RuntimeError('Restart commit was refused')
            except (OSError,TimeoutError,ConnectionError):
                # The reset can drop its own acknowledgement. Observe, do not replay.
                self.save(commit_reply_missing=True)
            self.save(state='awaiting_new_boot')
            end=self.clock()+boot_timeout_s;fresh=None
            while self.clock()<end:
                observed=self.bridge.doctor(session)
                if observed.get('reachable') and observed.get('identity_matches') is False:
                    fresh=self.bridge.attach(data['host'],data['port'])
                    if fresh['guest_boot_id']==boot:raise RuntimeError('Attach did not establish a different boot')
                    break
                self.sleep(1)
            if fresh is None:raise TimeoutError('Restart not verified; inspect the original power request, do not replay it')
            self.save(state='new_boot_observed',new_session=fresh['session'],new_guest_boot_id=fresh['guest_boot_id'])
            verify=uuid.uuid4().hex;self.save(verification_job_id=verify)
            self.bridge.guest_run(fresh['session'],verify,['/boot/HWSTATUS.ELF'],cwd='/tmp',uid=0,wait_s=2)
            verified=self.wait_job(fresh['session'],verify,lambda r:r.get('state')=='completed')
            self.save(verification_job=verified)
            if verified.get('exit',{}).get('code')!=0 or ('HARDWARE_PAYLOAD_ID '+expected_payload_id+'\n') not in verified.get('stdout',{}).get('text',''):
                raise RuntimeError('New boot did not confirm the expected initrd payload ID')
            self.save(state='boot_verified');return self.receipt
        except Exception as error:
            prior=self.receipt.get('state');self.save(state='needs_inspection',last_stage=prior,error=str(error),
                recovery='Inspect the same transaction_id and power_request_id. No mutation was retried.')
            try:self.save(job_observation=self.bridge.job(session,tx,wait_s=2))
            except Exception as observation:self.save(job_observation_error=str(observation))
            raise
