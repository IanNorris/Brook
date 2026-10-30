"""One boot-pinned transfer, bounded polling, no implicit mutation replay."""
import hashlib,json,stat,uuid
from pathlib import Path
from hardware_update_workflow import UpdateRun
from hardware_bulk_protocol import identifier,upload,download,frame_png,LIMIT

class BulkRun(UpdateRun):
    def execute(self,session,key_file,action,source=None,artifact_id=None,port=1236,request_id=None,keep_raw=False):
        if action not in ('put','get','frame'):raise ValueError('Expected put/get/frame')
        if not isinstance(keep_raw,bool) or (keep_raw and action!='frame'):raise ValueError('keep_raw is only for frame capture')
        if isinstance(port,bool) or not isinstance(port,int) or not 1<=port<=65535:raise ValueError('Invalid bulk port')
        if (action=='put')!=(source is not None):raise ValueError('Only put takes a host source path')
        if (action=='get')!=(artifact_id is not None):raise ValueError('Only get takes a guest artifact ID')
        if artifact_id:identifier(artifact_id)
        tx=identifier(request_id) if request_id is not None else uuid.uuid4().hex
        with Path(key_file).open() as key_stream:key=identifier(key_stream.read(33))
        data=self.bridge.load_session(session);boot=data['guest_boot_id']
        doctor=self.bridge.doctor(session)
        if not doctor.get('reachable') or not doctor.get('identity_matches'):raise RuntimeError('Pinned guest boot is not reachable/matching')
        if doctor.get('guest',{}).get('bulk_helper_api')!=1:raise RuntimeError('Guest has no opted-in binary helper; update its image first')
        if action=='frame' and doctor.get('guest',{}).get('completed_frame_copy_api')!=1:raise RuntimeError('Guest lacks completed-frame copy API')
        if source:
            source=Path(source).resolve(strict=True);info=source.stat()
            if not stat.S_ISREG(info.st_mode) or not 0<=info.st_size<=LIMIT:raise ValueError('Source must be a regular file of at most1GiB')
        self.output.mkdir(parents=True,exist_ok=False,mode=0o700)
        self.save(state='prepared',session=session,guest_boot_id=boot,transaction_id=tx,action=action,
            source=str(source) if source else None,artifact_id=artifact_id)
        try:
            args=['/boot/HWBULK.ELF',boot,tx,action]+([artifact_id] if artifact_id else [])
            self.save(state='receiver_launch_dispatched')
            self.bridge.guest_run(session,tx,args,cwd='/tmp',uid=0,wait_s=2)
            listening=self.wait_job(session,tx,lambda r:'HWBULK_LISTENING' in r.get('stdout',{}).get('text',''))
            self.save(state='receiver_listening',job=listening)
            if action=='put':result=upload(data['host'],port,key,boot,tx,source)
            else:result=download(data['host'],port,key,boot,tx,self.output/'artifact.bin',action)
            self.save(state='binary_reply',transfer=result)
            job=self.wait_job(session,tx,lambda r:r.get('state')=='completed')
            self.save(job=job)
            if job.get('exit',{}).get('code')!=0:raise RuntimeError('Receiver job did not complete successfully')
            stdout=job.get('stdout',{}).get('text','')
            records=[json.loads(line[len('HWBULK_COMPLETED '):]) for line in stdout.splitlines() if line.startswith('HWBULK_COMPLETED ')]
            if len(records)!=1 or any(records[0].get(k)!=result.get(k) for k in ('state','transaction_id','action','bytes','sha256','guest_path')):raise RuntimeError('Guest job/binary receipt mismatch')
            if action=='frame':
                png=frame_png(result['artifact_path'],self.output/'screenshot.png',result['payload'])
                raw=Path(result.pop('artifact_path'))
                if not keep_raw:raw.unlink()
                result['raw_artifact_path']=str(raw) if keep_raw else None
                with Path(png).open('rb') as image:png_hash=hashlib.file_digest(image,'sha256').hexdigest()
                self.save(artifact_path=png,artifact_sha256=png_hash,frame=result['payload'],transfer=result)
            elif action=='get':self.save(artifact_path=result['artifact_path'])
            self.save(state='completed');return self.receipt
        except Exception as error:
            stage=self.receipt.get('state');self.save(state='needs_inspection',last_stage=stage,error=str(error),
                recovery='Inspect the same transaction_id with hw_job. Do not redispatch a lost transfer under a new ID automatically.')
            try:self.save(job_observation=self.bridge.job(session,tx,wait_s=2))
            except Exception as observation:self.save(job_observation_error=str(observation))
            raise
