"""One boot-pinned transfer, bounded polling, no implicit mutation replay."""
import hashlib,json,re,stat,uuid
from pathlib import Path
from hardware_update_workflow import UpdateRun
from binary_bridge import receiver_key
from hardware_bulk_protocol import identifier,upload,download,disk_download,frame_png,validate_pacing,LIMIT

# Debug-frame provenance (src/shared/brook_debug_frame.h) in bridge terms.
FRAME_SOURCES={1:'cpu-backbuffer',2:'gpu-present',3:'legacy-gpu',4:'gpu-present',5:'gpu-present',6:'direct-client',7:'window-buffer'}
SCANOUT_BUFFERS={0:'unverified',1:'gop',2:'kernel-flip',3:'zero-copy',4:'direct-client',5:'unknown'}
FRAME_CURSOR,FRAME_WINDOW_BUFFER=1,2

TRANSFER_ROOT = '/data/.brook-transfer'
# Run on the guest only. The receiver also validates the transfer directory, but
# its lazy mkdir happens after a capture producer needs to stage its first file.
STAGE_GUEST_FILE = """\
import os
import stat
import sys

source, root, name, limit = sys.argv[1:]
limit = int(limit)
with os.fdopen(os.open(source, os.O_RDONLY | os.O_NONBLOCK), 'rb') as capture:
    metadata = os.fstat(capture.fileno())
    remaining = metadata.st_size
    if not stat.S_ISREG(metadata.st_mode) or not 0 <= remaining <= limit:
        sys.exit('Capture must be a regular file within the bulk size limit')
    try:
        os.mkdir(root, 0o700)
    except FileExistsError:
        pass
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        metadata = os.fstat(directory)
        if metadata.st_uid != os.geteuid() or metadata.st_mode & 0o022:
            sys.exit('Unsafe bulk transfer directory')
        descriptor = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                             0o600, dir_fd=directory)
        try:
            with os.fdopen(descriptor, 'wb') as artifact:
                while remaining:
                    chunk = capture.read(min(remaining, 65536))
                    if not chunk:
                        sys.exit('Capture became shorter while staging')
                    artifact.write(chunk)
                    remaining -= len(chunk)
                if capture.read(1):
                    sys.exit('Capture grew while staging')
                staged = os.fstat(artifact.fileno())
        except BaseException:
            os.unlink(name, dir_fd=directory)
            raise
    finally:
        os.close(directory)
print(f'{staged.st_dev}:{staged.st_ino}')
"""

REMOVE_GUEST_FILE = """\
import os
import stat
import sys

root, name, expected = sys.argv[1:]
directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
try:
    metadata = os.fstat(directory)
    if metadata.st_uid != os.geteuid() or metadata.st_mode & 0o022:
        sys.exit('Unsafe bulk transfer directory')
    descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    try:
        staged = os.fstat(descriptor)
        if not stat.S_ISREG(staged.st_mode) or f'{staged.st_dev}:{staged.st_ino}' != expected:
            sys.exit('Staged capture was replaced')
        os.unlink(name, dir_fd=directory)
    finally:
        os.close(descriptor)
finally:
    os.close(directory)
"""

def describe_frame(payload):
    """Named provenance for a frame payload; older helpers report none."""
    version=payload.get('info_version')
    if not version:return {'source':None,'scanout':None,'provenance':'guest helper predates frame provenance'}
    flags=payload.get('info_flags',0)
    source=FRAME_SOURCES.get(payload.get('source_kind'),'unknown')
    result={'source':source,'scanout':SCANOUT_BUFFERS.get(payload.get('plane'),'unknown') if version>=4 else 'unverified'}
    if version>=4:
        result.update(cursor_composited=bool(flags&1),detiled=bool(flags&2),
                      scanout_surface=f"{payload.get('plane_surface',0):#x}",copy_us=payload.get('copy_us'))
    if source=='window-buffer':
        result.update(scanout='not-applicable',scanout_surface=None)
    return result

def frame_option_argument(options):
    if options is None:return None
    flags,window_id,incarnation=options.get('flags',0),options.get('window_id',0),options.get('incarnation',0)
    for value in (flags,window_id,incarnation):
        if isinstance(value,bool) or not isinstance(value,int) or value<0:raise ValueError('Invalid frame options')
    if flags>FRAME_WINDOW_BUFFER or window_id>256 or incarnation>2**64-1 or \
       (bool(flags&FRAME_WINDOW_BUFFER)!=bool(window_id and incarnation)) or (not flags&FRAME_WINDOW_BUFFER and (window_id or incarnation)):
        raise ValueError('Invalid frame options')
    return f'o:{flags}:{window_id}:{incarnation}'

def clip_rect(rect,width,height):
    x,y,w,h=rect
    if any(isinstance(v,bool) or not isinstance(v,int) for v in (x,y,w,h)):raise ValueError('Invalid crop rectangle')
    x0,y0,x1,y1=max(0,x),max(0,y),min(width,x+w),min(height,y+h)
    if x1<=x0 or y1<=y0:raise ValueError('Window rectangle is entirely off screen')
    return {'x':x0,'y':y0,'width':x1-x0,'height':y1-y0}

class BulkRun(UpdateRun):
    def stage_guest_file(self, session, boot, guest_file, artifact_id):
        producer = uuid.uuid4().hex
        self.save(state='producer_launch_dispatched', producer_job_id=producer)
        args = ['/nix/profile/bin/python3', '-c', STAGE_GUEST_FILE, guest_file,
                TRANSFER_ROOT, f'{artifact_id}.bin', str(LIMIT)]
        self.bridge.guest_run(session, producer, args, cwd='/tmp', uid=0, wait_s=2)
        job = self.wait_job(session, producer, lambda reply: reply.get('state') == 'completed')
        self.save(producer_job=job)
        job_identity = (job.get('session'), job.get('guest_boot_id'), job.get('operation_id'))
        if job_identity != (session, boot, producer):
            raise RuntimeError('Capture staging job belongs to a different boot or operation')
        if job.get('exit', {}).get('code') != 0:
            detail = job.get('stderr', {}).get('text', '').strip()
            raise RuntimeError(f'Capture staging job failed (exit {job.get("exit")}): {detail}')
        staged = job.get('stdout', {}).get('text', '').strip()
        if not re.fullmatch(r'[0-9]+:[0-9]+', staged):
            raise RuntimeError('Capture staging job did not report the staged file identity')
        self.save(state='producer_completed', guest_artifact_identity=staged)
        return staged

    def remove_guest_file(self, session, boot, artifact_id, staged):
        cleanup = uuid.uuid4().hex
        self.save(guest_cleanup={'state': 'dispatched', 'job_id': cleanup})
        try:
            args = ['/nix/profile/bin/python3', '-c', REMOVE_GUEST_FILE,
                    TRANSFER_ROOT, f'{artifact_id}.bin', staged]
            self.bridge.guest_run(session, cleanup, args, cwd='/tmp', uid=0, wait_s=2)
            job = self.wait_job(session, cleanup, lambda reply: reply.get('state') == 'completed')
            if (job.get('session'), job.get('guest_boot_id'), job.get('operation_id')) != (session, boot, cleanup):
                raise RuntimeError('Capture cleanup job belongs to a different boot or operation')
            if job.get('exit', {}).get('code') != 0:
                detail = job.get('stderr', {}).get('text', '').strip()
                raise RuntimeError(f'Capture cleanup job failed (exit {job.get("exit")}): {detail}')
        except Exception as error:
            self.save(guest_cleanup={'state': 'needs_inspection', 'job_id': cleanup, 'error': str(error)})
        else:
            self.save(guest_cleanup={'state': 'completed', 'job_id': cleanup})

    def execute(self,session,key_file,action,source=None,artifact_id=None,port=1236,request_id=None,keep_raw=False,disk_identity=None,max_bytes=None,pace_bytes_per_second=None,frame_options=None,crop=None,guest_file=None):
        if action not in ('put','get','frame','disk','getz','framez'):raise ValueError('Expected put/get/frame/disk/getz/framez')
        if action=='disk' and (disk_identity is None or max_bytes is None):raise ValueError('Disk export requires a pinned identity and explicit maximum size')
        if action!='disk' and (disk_identity is not None or max_bytes is not None):raise ValueError('Disk options require disk action')
        if not isinstance(keep_raw,bool) or (keep_raw and action not in ('frame','framez')):raise ValueError('keep_raw is only for frame capture')
        if isinstance(port,bool) or not isinstance(port,int) or not 1<=port<=65535:raise ValueError('Invalid bulk port')
        if (action=='put')!=(source is not None):raise ValueError('Only put takes a host source path')
        if pace_bytes_per_second is not None and action!='put':raise ValueError('Pacing applies only to host uploads')
        validate_pacing(pace_bytes_per_second)
        if guest_file is not None:
            if (action not in ('get', 'getz') or artifact_id is not None or
                    not isinstance(guest_file, str) or not guest_file.startswith('/') or '\0' in guest_file):
                raise ValueError('An absolute guest file path can replace the artifact ID for get/getz')
            artifact_id = uuid.uuid4().hex
        if (action in ('get','getz'))!=(artifact_id is not None):raise ValueError('Only get takes a guest artifact ID')
        if artifact_id:identifier(artifact_id)
        if (frame_options is not None or crop is not None) and action not in ('frame','framez'):raise ValueError('Frame options are only for frame capture')
        option_argument=frame_option_argument(frame_options)
        tx=identifier(request_id) if request_id is not None else uuid.uuid4().hex
        data=self.bridge.load_session(session);boot=data['guest_boot_id']
        key=receiver_key(data,key_file)
        doctor=self.bridge.doctor(session)
        if doctor.get('probe_stage')!='verified' or not doctor.get('reachable') or not doctor.get('identity_matches'):raise RuntimeError('Pinned guest boot probe is incomplete or not reachable/matching')
        if doctor.get('guest',{}).get('bulk_helper_api')!=1:raise RuntimeError('Guest has no opted-in binary helper; update its image first')
        if action in ('frame','framez') and doctor.get('guest',{}).get('completed_frame_copy_api')!=1:raise RuntimeError('Guest lacks completed-frame copy API')
        if source:
            source=Path(source).resolve(strict=True);info=source.stat()
            if not stat.S_ISREG(info.st_mode) or not 0<=info.st_size<=LIMIT:raise ValueError('Source must be a regular file of at most1GiB')
        self.output.mkdir(parents=True,exist_ok=False,mode=0o700)
        self.save(state='prepared',session=session,guest_boot_id=boot,transaction_id=tx,action=action,
            source=str(source) if source else None,artifact_id=artifact_id,pace_bytes_per_second=pace_bytes_per_second)
        if guest_file is not None:
            self.save(guest_file=guest_file, guest_artifact_path=f'{TRANSFER_ROOT}/{artifact_id}.bin')
        try:
            if guest_file is not None:
                staged = self.stage_guest_file(session, boot, guest_file, artifact_id)
            args=['/boot/HWBULKZ.ELF' if action in ('getz','framez') else '/boot/HWBULK.ELF',boot,tx,action]+([artifact_id] if artifact_id else [])+([option_argument] if option_argument else [])
            self.save(state='receiver_launch_dispatched',frame_options=option_argument)
            self.bridge.guest_run(session,tx,args,cwd='/tmp',uid=0,wait_s=2)
            try:listening=self.wait_job(session,tx,lambda r:'HWBULK_LISTENING' in r.get('stdout',{}).get('text',''))
            except RuntimeError as error:
                if option_argument:
                    raise RuntimeError('Guest helper refused frame options (window_area=buffer/cursor need a pair built '
                                       'from work/hw-capture or later): '+str(error)) from error
                raise
            self.save(state='receiver_listening',job=listening)
            binary_start=self.clock()
            if action=='put':
                if pace_bytes_per_second is None:result=upload(data['host'],port,key,boot,tx,source)
                else:result=upload(data['host'],port,key,boot,tx,source,pace_bytes_per_second=pace_bytes_per_second)
            elif action=='disk':
                def event(name,details):
                    self.save(state='disk_'+name,**({'artifact_path':details['artifact_path']} if 'artifact_path' in details else {}),**{'disk_'+name:details})
                result=disk_download(data['host'],port,key,boot,tx,self.output/'disk.img',disk_identity,max_bytes,event)
            else:result=download(data['host'],port,key,boot,tx,self.output/'artifact.bin',action)
            self.save(state='binary_reply',transfer=result,binary_phase_s=self.clock()-binary_start)
            job=self.wait_job(session,tx,lambda r:r.get('state')=='completed')
            self.save(job=job)
            if job.get('exit',{}).get('code')!=0:raise RuntimeError('Receiver job did not complete successfully')
            stdout=job.get('stdout',{}).get('text','')
            records=[json.loads(line[len('HWBULK_COMPLETED '):]) for line in stdout.splitlines() if line.startswith('HWBULK_COMPLETED ')]
            if len(records)!=1 or any(records[0].get(k)!=result.get(k) for k in ('state','transaction_id','action','bytes','sha256','guest_path')):raise RuntimeError('Guest job/binary receipt mismatch')
            if action in ('frame','framez'):
                result['payload'].update(describe_frame(result['payload']))
                png=frame_png(result['artifact_path'],self.output/'screenshot.png',result['payload'])
                if crop is not None:
                    # crop: an unclipped (x,y,width,height) screen rectangle.
                    clipped=clip_rect(crop,result['payload']['width'],result['payload']['height'])
                    self.save(full_artifact_path=png,crop=clipped)
                    png=frame_png(result['artifact_path'],self.output/'window.png',result['payload'],clipped)
                raw=Path(result.pop('artifact_path'))
                if not keep_raw:raw.unlink()
                result['raw_artifact_path']=str(raw) if keep_raw else None
                with Path(png).open('rb') as image:png_hash=hashlib.file_digest(image,'sha256').hexdigest()
                self.save(artifact_path=png,artifact_sha256=png_hash,frame=result['payload'],transfer=result)
            elif action in ('get','getz','disk'):self.save(artifact_path=result['artifact_path'])
            self.save(state='completed')
        except Exception as error:
            stage=self.receipt.get('state')
            producer_pending = stage == 'producer_launch_dispatched'
            observed_job = self.receipt['producer_job_id'] if producer_pending else tx
            observed_field = 'producer_job_id' if producer_pending else 'transaction_id'
            self.save(state='published_needs_inspection' if stage in ('disk_published','disk_published_durability_uncertain') else 'needs_inspection',last_stage=stage,error=str(error),
                recovery=f'Inspect the same {observed_field} with hw_job. Do not redispatch under a new ID automatically.')
            try:self.save(job_observation=self.bridge.job(session,observed_job,wait_s=2))
            except Exception as observation:self.save(job_observation_error=str(observation))
            raise

        if guest_file is not None:
            self.remove_guest_file(session, boot, artifact_id, staged)
        return self.receipt
