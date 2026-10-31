"""One boot-pinned provisioning attempt; persist IDs and never replay mutations."""
import json,re,uuid
from pathlib import Path
from hardware_update_workflow import UpdateRun
from hardware_store_protocol import provision,prepare_source
from hardware_bulk_protocol import identifier

class StoreRun(UpdateRun):
    def execute(self,session,key_file,image,record,sha256,port=1237,helper='/boot/HWSTORE.ELF'):
        if isinstance(port,bool) or not isinstance(port,int) or not 1<=port<=65535:raise ValueError('Invalid store port')
        if helper!='/boot/HWSTORE.ELF' and not re.fullmatch(r'/data/\.brook-transfer/[0-9a-f]{32}\.bin',helper):raise ValueError('Invalid fixed helper path')
        image=Path(image).resolve(strict=True);record=Path(record).read_bytes()
        with Path(key_file).open() as f:key=identifier(f.read(33))
        prepared=prepare_source(image,record,sha256)
        data=self.bridge.load_session(session);boot=identifier(data['guest_boot_id']);doctor=self.bridge.doctor(session)
        if not doctor.get('reachable') or not doctor.get('identity_matches'):raise RuntimeError('Pinned guest boot is not reachable/matching')
        tx=uuid.uuid4().hex;self.output.mkdir(parents=True,exist_ok=False,mode=0o700)
        self.save(state='prepared',session=session,guest_boot_id=boot,transaction_id=tx,image=str(image),record=record.decode('ascii'),sha256=sha256,helper=helper)
        try:
            self.save(state='receiver_launch_dispatched')
            self.bridge.guest_run(session,tx,[helper,boot,tx],cwd='/tmp',uid=0,wait_s=2)
            job=self.wait_job(session,tx,lambda r:'HWSTORE_LISTENING' in r.get('stdout',{}).get('text',''))
            self.save(state='receiver_listening',job=job)
            result=provision(data['host'],port,key,boot,tx,image,record,sha256,prepared=prepared)
            self.save(state='binary_reply',transfer=result)
            job=self.wait_job(session,tx,lambda r:r.get('state')=='completed')
            output=job.get('stdout',{}).get('text','')
            receipt=f'HWSTORE_RESULT transaction={tx} published=1 durable=1 error=0'
            if job.get('exit',{}).get('code')!=0 or receipt not in output:raise RuntimeError('Guest receipt does not confirm durable marker publication')
            self.save(state='completed',job=job);return self.receipt
        except Exception as error:
            stage=self.receipt.get('state');self.save(state='needs_inspection',last_stage=stage,error=str(error),recovery='Inspect original transaction_id; never automatically replay.')
            try:self.save(job_observation=self.bridge.job(session,tx,wait_s=2))
            except Exception as observation:self.save(job_observation_error=str(observation))
            raise
