"""Durable host receipts for one maintenance upload; mutations are never retried.

The bridge is injected so this module has no host-desktop or QEMU dependencies.
A failed run is inspected via its original guest job / power request IDs, rather
than restarted with a new transaction. Reboot success requires both a changed
boot epoch and the expected payload ID from a new boot-pinned guest job.
"""
import hashlib,json,os,re,time,uuid
from pathlib import Path
from hardware_update_protocol import upload,read_payload,KERNEL_MAX,INITRD_MAX

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

    def execute(self,session,key_file,kernel_path,initrd_path,activate=False,restart=False,expected_payload_id=None,port=1235):
        if restart and (not activate or not expected_payload_id):raise ValueError('Restart requires activation and an expected payload ID')
        if expected_payload_id and (not re.fullmatch(r'[a-zA-Z0-9_.-]{1,95}',expected_payload_id)):raise ValueError('Invalid expected payload ID')
        if isinstance(port,bool) or not isinstance(port,int) or not 1<=port<=65535:raise ValueError('Invalid upload port')
        kernel=read_payload(kernel_path,KERNEL_MAX);initrd=read_payload(initrd_path,INITRD_MAX)
        if len(kernel)<64 or len(initrd)<512:raise ValueError('Payload is too short')
        with Path(key_file).open() as source:key=source.read(33)
        if not re.fullmatch(r'[0-9a-f]{32}',key):raise ValueError('Expected provisioned 32-character key without a newline')
        data=self.bridge.load_session(session);boot=data['guest_boot_id']
        doctor=self.bridge.doctor(session)
        if not doctor.get('reachable') or not doctor.get('identity_matches'):raise RuntimeError('Original guest boot is not reachable/matching')
        if restart and not doctor.get('capabilities',{}).get('power',{}).get('reset'):raise RuntimeError('Guest does not advertise a supported restart')
        # An existing output directory is never reused, even after lost replies.
        self.output.mkdir(parents=True,exist_ok=False,mode=0o700)
        tx=uuid.uuid4().hex;power_id=uuid.uuid4().hex
        self.save(state='prepared',session=session,guest_boot_id=boot,transaction_id=tx,power_request_id=power_id,
            activate_requested=activate,restart_requested=restart,expected_payload_id=expected_payload_id,
            kernel={'path':str(Path(kernel_path).resolve()),'bytes':len(kernel),'sha256':hashlib.sha256(kernel).hexdigest()},
            initrd={'path':str(Path(initrd_path).resolve()),'bytes':len(initrd),'sha256':hashlib.sha256(initrd).hexdigest()})
        try:
            self.save(state='receiver_launch_dispatched')
            self.bridge.guest_run(session,tx,['/boot/HWUPDATE.ELF',boot,tx],cwd='/tmp',uid=0,wait_s=2)
            listening=self.wait_job(session,tx,lambda r:'HWUPDATE_LISTENING' in r.get('stdout',{}).get('text',''))
            self.save(state='receiver_listening',job=listening)
            result=self.uploader(data['host'],port,key,boot,tx,kernel,initrd,activate)
            self.save(state='upload_reply',upload=result)
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
            end=self.clock()+90
            while status.get('state')!='ready':
                if status.get('state') in ('failed','unsupported') or self.clock()>=end:raise RuntimeError('Restart did not reach checked ready state')
                self.sleep(.1);status=self.bridge.power(session,power_id,'status');self.save(power=status)
            self.save(state='restart_commit_dispatched')
            try:
                committed=self.bridge.power(session,power_id,'commit');self.save(power_commit=committed)
                if committed.get('error',0):raise RuntimeError('Restart commit was refused')
            except (OSError,TimeoutError,ConnectionError):
                # The reset can drop its own acknowledgement. Observe, do not replay.
                self.save(commit_reply_missing=True)
            self.save(state='awaiting_new_boot')
            end=self.clock()+120;fresh=None
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
