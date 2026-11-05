"""Retrieve one fixed-journal crash packet through a boot-pinned hardware bridge."""
import hashlib
import re
import zlib
from pathlib import Path
from hardware_bulk_protocol import identifier
from hardware_bulk_workflow import BulkRun
from hardware_update_workflow import UpdateRun


class PanicRun(UpdateRun):
    def execute(self,session,key_file,request_id,port=1236):
        export_id=identifier(request_id)
        if isinstance(port,bool) or not isinstance(port,int) or not 1<=port<=65535:
            raise ValueError('Invalid bulk port')
        with Path(key_file).open() as stream:
            identifier(stream.read(33))
        transfer_id=hashlib.sha256(('brook-panic-transfer-v1:'+export_id).encode()).hexdigest()[:32]
        data=self.bridge.load_session(session)
        boot=identifier(data['guest_boot_id'])
        doctor=self.bridge.doctor(session)
        if doctor.get('probe_stage')!='verified' or not doctor.get('reachable') or not doctor.get('identity_matches'):
            raise RuntimeError('Pinned guest boot probe is incomplete or not reachable/matching')
        if doctor.get('guest',{}).get('bulk_helper_api')!=1:
            raise RuntimeError('Guest has no opted-in binary transfer helper')
        self.output.mkdir(parents=True,exist_ok=False,mode=0o700)
        self.save(state='prepared',session=session,guest_boot_id=boot,export_job_id=export_id,
                  artifact_id=export_id,transfer_request_id=transfer_id)
        try:
            self.save(state='export_dispatched')
            self.bridge.guest_run(session,export_id,['/boot/PANICLOG.ELF','--bridge',export_id],cwd='/tmp',uid=0,wait_s=2)
            job=self.wait_job(session,export_id,lambda r:r.get('state')=='completed')
            self.save(state='export_completed',export_job=job)
            if job.get('exit',{}).get('code')!=0:raise RuntimeError('Panic journal export failed')
            output=job.get('stdout',{}).get('text','')
            match=re.fullmatch(r'PANICLOG: sequence=([1-9][0-9]*) bytes=([1-9][0-9]*) crc32=([0-9a-f]{8}) output=([0-9a-f]{32})\n',output)
            if not match or match[4]!=export_id:raise RuntimeError('Invalid panic export receipt')
            sequence,size,crc=int(match[1]),int(match[2]),int(match[3],16)
            if size>65536:raise RuntimeError('Panic packet exceeds journal capacity')
            self.save(state='transfer_dispatched',sequence=sequence,packet_bytes=size,packet_crc32=f'{crc:08x}')
            result=BulkRun(self.bridge,self.output/'transfer').execute(session,key_file,'get',artifact_id=export_id,port=port,request_id=transfer_id)
            artifact=Path(result['artifact_path'])
            payload=artifact.read_bytes()
            if len(payload)!=size or zlib.crc32(payload)!=crc:
                raise RuntimeError('Host packet does not match the exported journal record')
            self.save(state='completed',artifact_path=str(artifact),artifact_sha256=hashlib.sha256(payload).hexdigest(),
                      transfer_receipt=str(self.output/'transfer'/'receipt.json'))
            return self.receipt
        except Exception as error:
            previous=self.receipt.get('state')
            self.save(state='needs_inspection',last_stage=previous,error=str(error),
                      recovery='Inspect the original export job and transfer receipt; do not replay automatically.')
            raise
