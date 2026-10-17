"""Durable host identities and bounded guest job transport; never a host shell."""
import codecs
import contextlib
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import stat
import threading
import time
import uuid


class GuestJobError(RuntimeError):
    pass


def request_bytes(argv, cwd='/data/home', env=None, uid=1000):
    env = {} if env is None else env
    if not isinstance(argv, list) or not 1 <= len(argv) <= 64:
        raise ValueError('argv must contain 1–64 strings')
    if not isinstance(cwd, str) or not cwd.startswith('/') or len(cwd.encode()) > 255:
        raise ValueError('cwd must be an absolute guest path of at most 255 bytes')
    if not isinstance(env, dict) or len(env) > 64:
        raise ValueError('env must contain at most 64 overrides')
    if isinstance(uid, bool) or not isinstance(uid, int) or uid not in (0, 1000):
        raise ValueError('uid must be 1000 (desktop) or explicit maintenance uid 0')
    strings = []
    for value in argv + [cwd]:
        if not isinstance(value, str) or '\0' in value:
            raise ValueError('Arguments and cwd must be NUL-free strings')
        strings.append(value)
    if not argv[0].startswith('/') or len(argv[0].encode()) > 255:
        raise ValueError('argv[0] must be an absolute guest executable path of at most 255 bytes')
    if any(not isinstance(key,str) for key in env):
        raise ValueError('Invalid environment name')
    for key, value in sorted(env.items()):
        if not isinstance(key, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', key):
            raise ValueError('Invalid environment name')
        if not isinstance(value, str) or '\0' in value:
            raise ValueError('Environment values must be NUL-free strings')
        strings.append(key + '=' + value)
    result = b'BJ01' + struct.pack('<III', uid, len(argv), len(env))
    result += b''.join(value.encode('utf-8') + b'\0' for value in strings)
    if len(result) > 32768:
        raise ValueError('Structured request exceeds 32768 bytes')
    return result


def validate_id(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{32}', value):
        raise ValueError('request_id/operation_id must be 32 lowercase hexadecimal characters')


class Jobs:
    directory_name = "jobs"
    def __init__(self, registry, load_session, session_lock, guest, write_json):
        self.registry = Path(registry)
        self.load_session, self.session_lock = load_session, session_lock
        self.guest, self.write_json = guest, write_json
        self.read_retries=0

    def path(self, session, operation):
        validate_id(operation)
        self.load_session(session, live=False)
        run=self.registry/session
        directory = run / self.directory_name
        if run.is_symlink() or directory.is_symlink():
            raise ValueError('Job registry must not be a symlink')
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory / (operation + '.json')
        if path.is_symlink():
            raise ValueError('Job descriptor must not be a symlink')
        return path

    @contextlib.contextmanager
    def lock(self, path, blocking=True, deadline=None):
        flags = os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK
        with os.fdopen(os.open(path.with_suffix('.lock'), flags, 0o600), 'a') as lock:
            info=os.fstat(lock.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid():
                raise ValueError('Job lock must be a regular file owned by this user')
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | (0 if blocking and deadline is None else fcntl.LOCK_NB))
                    break
                except BlockingIOError:
                    if not blocking:
                        yield False
                        return
                    if time.monotonic()>=deadline:
                        raise TimeoutError('Job registry lock deadline expired')
                    time.sleep(min(.01,max(0,deadline-time.monotonic())))
            try:
                yield True
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def query(self, session, generation, command, deadline=None, boot_id=None):
        if boot_id is not None:
            validate_id(boot_id)
            if boot_id=="0"*32:raise ValueError("Guest boot identity is unset")
            command += " " + boot_id
        readonly=command in {'bridge job caps','bridge ui caps','bridge epoch'} or command.startswith(('bridge job status ','bridge job read ','bridge ui windows ','bridge ui process ','bridge ui status '))
        for attempt in range(3 if readonly else 1):
            try:
                return self.exchange(session,generation,command,deadline)
            except OSError:
                if not readonly or attempt==2 or (deadline is not None and time.monotonic()>=deadline):raise
                self.read_retries+=1
                delay=.1*(attempt+1)
                if deadline is not None:delay=min(delay,max(0,deadline-time.monotonic()))
                time.sleep(delay)

    def before_exchange(self, session, command):
        pass

    def exchange(self, session, generation, command, deadline=None):
        remaining=10 if deadline is None else min(10,deadline-time.monotonic())
        if remaining<=0:raise TimeoutError('Inline job observation deadline expired')
        with self.session_lock(session,timeout=remaining) as data:
            if data['generation'] != generation:
                raise RuntimeError('Guest generation changed; operation targets are stale')
            self.before_exchange(session,command)
            remaining=3 if deadline is None else min(3,deadline-time.monotonic())
            if remaining<=0:raise TimeoutError('Inline job observation deadline expired')
            reply = self.guest(data, command, timeout=remaining)
        for field in ('job_error','ui_error','epoch_error'):
            if field in reply:raise GuestJobError(reply[field])
        return reply

    def capabilities(self, session, deadline=None):
        data = self.load_session(session)
        reply = self.query(session, data['generation'], 'bridge job caps',deadline)
        if reply.get('job_api') != 2:
            raise RuntimeError('Guest command jobs are unsupported; update the guest kernel')
        reply["boot_id"] = self.boot_identity(session,data["generation"],deadline)
        return data, reply

    def boot_identity(self,session,generation,deadline=None):
        reply=self.query(session,generation,"bridge epoch",deadline)
        if reply.get("boot_id")=="0"*32:
            # A new proposal on every initialization attempt: never resurrect a
            # persisted operation token after reboot or a lost init reply.
            reply=self.query(session,generation,"bridge epoch init "+uuid.uuid4().hex,deadline)
        token=reply.get("boot_id")
        validate_id(token)
        if token=="0"*32:raise RuntimeError("Guest boot identity is unset")
        return token

    def submit(self, session, request_id, argv, cwd='/data/home', env=None, uid=1000,
               wait_s=0, cancel=None):
        deadline=time.monotonic()+20
        payload = request_bytes(argv, cwd, env, uid)
        self.validate_wait(wait_s)
        path = self.path(session, request_id)
        data,caps = self.capabilities(session,deadline)
        generation = data['generation']
        boot_id = caps['boot_id']
        fingerprint = hashlib.sha256(generation.encode() + b'\0' + boot_id.encode() + b'\0' + payload).hexdigest()
        # Serialize only identity creation; uploads have their own operation lock.
        with self.lock(path.parent / 'registry',deadline=deadline):
            if path.exists():
                existing = json.loads(path.read_text())
                if existing['fingerprint'] != fingerprint:
                    raise ValueError('request_id is already bound to different arguments or generation/guest boot')
                created = False
            else:
                operation = {'session': session, 'generation': generation, 'guest_boot_id':boot_id,
                    'operation_id': request_id, 'request_id': request_id,
                    'fingerprint': fingerprint, 'state': 'queued', 'effect': 'queued',
                    'stage': 'created', 'created_host_monotonic': time.monotonic()}
                self.write_json(path, operation)
                created = True
        # An explicit identical re-submission may recover interrupted staging.
        # It never bypasses a live uploader or blindly repeats a commit.
        resume=created
        if not created:
            with self.lock(path,blocking=False) as acquired:
                if acquired:
                    existing=json.loads(path.read_text())
                    resume=existing['stage']!='committed' and existing['state']!='failed'
                    if resume:
                        existing.update(state='queued',resume_requested=True)
                        self.write_json(path,existing)
        if resume:
            # A normal one-shot CLI exit or stdio EOF must not abandon staging
            # after returning the queued operation ID. A forced process exit is
            # recovered through the same persisted identity and guest checks.
            threading.Thread(target=self.upload, args=(path, payload), daemon=False).start()
        return self.status(session, request_id, wait_s=wait_s, cancel=cancel,_deadline=deadline)

    def upload(self, path, payload):
        with self.lock(path):
            operation = json.loads(path.read_text())
            session, generation, ident = (operation[k] for k in ('session', 'generation', 'operation_id'))
            boot_id=operation.get('guest_boot_id')
            def stage(value):
                operation.update(stage=value, effect='unknown')
                self.write_json(path, operation)
            try:
                try:
                    observed=self.query(session,generation,f'bridge job status {ident}',boot_id=boot_id)
                except GuestJobError as error:
                    if str(error)!='Unknown job':raise
                    observed=None
                if observed and observed['state']!='uploading':
                    operation.update(stage='committed',effect='confirmed',state=observed['state'],guest=observed)
                    self.write_json(path,operation)
                    return
                stage('begin_dispatched')
                begun=self.query(session, generation, f'bridge job begin {ident} {len(payload)}',boot_id=boot_id)
                received=begun.get('received',0)
                if not 0<=received<=len(payload):
                    raise RuntimeError('Guest request upload extent mismatch')
                for offset in range(0, len(payload), 64):
                    stage(f'put_dispatched:{offset}')
                    reply = self.query(session, generation,
                        f'bridge job put {ident} {offset} {payload[offset:offset+64].hex()}',boot_id=boot_id)
                    acknowledged=reply.get('received',-1)
                    if not max(received,min(offset+64,len(payload)))<=acknowledged<=len(payload):
                        raise RuntimeError('Guest request upload acknowledgement mismatch')
                    received=acknowledged
                stage('commit_dispatched')
                reply = self.query(session, generation, f'bridge job commit {ident}',boot_id=boot_id)
                operation.update(stage='committed', effect='confirmed', state=reply['state'], guest=reply)
            except Exception as error:
                # A timeout never means that a mutation did not happen.
                operation.update(state='lost' if str(error)=='Stale guest boot' else 'unknown', effect='unknown', error=str(error))
            self.write_json(path, operation)

    @staticmethod
    def validate_wait(wait_s):
        if isinstance(wait_s, bool) or not isinstance(wait_s, (int, float)) or not math.isfinite(wait_s) or not 0 <= wait_s <= 20:
            raise ValueError('wait_s must be between 0 and 20 seconds')

    def stream(self, session, generation, ident, name, offset, limit, deadline=None, boot_id=None):
        size=0;pieces=[];gaps=[]
        decoder = codecs.getincrementaldecoder('utf-8')('replace')
        dropped = 0
        cursor, start, eof = offset, offset, False
        read_timeout=False;read_error=None
        while size < limit:
            try:
                reply = self.query(session, generation,
                    f'bridge job read {ident} {name} {cursor} {min(1024, limit-size)}',deadline,boot_id=boot_id)
            except (OSError,RuntimeError,ValueError) as error:
                read_timeout=isinstance(error,TimeoutError);read_error=str(error);break
            block = bytes.fromhex(reply['hex'])
            if reply['start']<cursor or reply['next']!=reply['start']+len(block) or reply['dropped']!=reply['start']-cursor:
                raise RuntimeError('Guest output cursor acknowledgement mismatch')
            if size==0:
                start = reply['start']
            if reply['start']!=cursor:
                pieces.append(decoder.decode(b'',final=True))
                decoder=codecs.getincrementaldecoder('utf-8')('replace')
                gaps.append({'after_offset':cursor,'before_offset':reply['start'],
                             'dropped_bytes':reply['dropped']})
            dropped += reply['dropped']
            size+=len(block)
            cursor, eof = reply['next'], reply['eof']
            pieces.append(decoder.decode(block,final=eof))
            if not block or eof:
                break
        pending = len(decoder.getstate()[0])
        return {'text': ''.join(pieces), 'requested_offset': offset, 'start_offset': start,
                'next_offset': cursor-pending, 'dropped_bytes': dropped,
                'eof': eof, 'pending_utf8_bytes': pending, 'gaps':gaps,'decoding': 'utf8_replace',
                'read_timed_out':read_timeout,'read_error':read_error}

    def status(self, session, operation_id, stdout_offset=0, stderr_offset=0,
               output_limit=4096, wait_s=0, cancel=None,_deadline=None):
        self.validate_wait(wait_s)
        if any(isinstance(v, bool) or not isinstance(v, int) or v < 0
               for v in (stdout_offset, stderr_offset, output_limit)) or not 1 <= output_limit <= 16384:
            raise ValueError('Nonnegative byte cursors and output_limit 1–16384 required')
        path = self.path(session, operation_id)
        inline_deadline=time.monotonic()+20 if _deadline is None else _deadline
        deadline = min(time.monotonic() + wait_s,inline_deadline)
        while True:
            operation = json.loads(path.read_text())
            data = self.load_session(session, live=False)
            if data['generation'] != operation['generation'] or not operation.get('guest_boot_id'):
                return {**operation, 'state': 'lost', 'effect': 'unknown', 'error': 'Guest generation changed'}
            if data.get('state') != 'running':
                terminal = operation['state'] in {'completed', 'failed'}
                return {**operation, 'state': operation['state'] if terminal else 'lost',
                        'cached': True, 'error': 'Guest is stopped; returning last recorded evidence'}
            if operation['stage']=='created':
                try:
                    current=self.query(session,operation['generation'],'bridge epoch',inline_deadline)
                    if current.get('boot_id')!=operation['guest_boot_id']:
                        return {**operation,'state':'lost','effect':'unknown','error':'Guest boot changed before upload'}
                except (OSError,RuntimeError,ValueError) as error:
                    operation['observation_error']=str(error)
                    break
            with self.lock(path, blocking=False) as acquired:
                if acquired:operation=json.loads(path.read_text())
                if acquired and operation['stage'] != 'created':
                    try:
                        reply = self.query(session, operation['generation'], f'bridge job status {operation_id}',inline_deadline,boot_id=operation['guest_boot_id'])
                    except (OSError,RuntimeError,ValueError) as error:
                        operation['observation_error']=str(error)
                        if str(error)=='Stale guest boot':
                            operation.update(state='lost',effect='unknown')
                            self.write_json(path,operation)
                        break
                    operation.update(guest=reply, state=reply['state'])
                    if reply['state'] in {'queued', 'running', 'completed', 'failed'}:
                        operation.update(stage='committed', effect='confirmed')
                        operation.pop('error',None)
                    if reply.get('exit_collected'):
                        status = reply['wait_status']
                        operation['exit'] = {'code': (status >> 8) & 255 if not status & 127 else None,
                                             'signal': status & 127 or None, 'wait_status': status}
                    self.write_json(path, operation)
            if operation['state'] in {'completed', 'failed', 'unknown', 'lost'} or time.monotonic() >= deadline:
                break
            if cancel and cancel.wait(min(.1, max(0, deadline-time.monotonic()))):
                operation['wait_cancelled'] = True
                break
            if not cancel:
                time.sleep(min(.1, max(0, deadline-time.monotonic())))
        if operation.get('stage') == 'committed' and operation['state'] not in {'failed','lost'}:
            operation['stdout'] = self.stream(session, operation['generation'], operation_id, 'out', stdout_offset, output_limit,inline_deadline,boot_id=operation['guest_boot_id'])
            operation['stderr'] = self.stream(session, operation['generation'], operation_id, 'err', stderr_offset, output_limit,inline_deadline,boot_id=operation['guest_boot_id'])
        if any(operation.get(name,{}).get('read_error')=='Stale guest boot' for name in ('stdout','stderr')):
            operation.update(state='lost',effect='unknown',error='Guest boot changed during output read')
        operation['wait_timed_out'] = (operation['state'] not in {'completed', 'failed'} and wait_s > 0
            and not operation.get('wait_cancelled') and time.monotonic()>=deadline)
        operation['readonly_transport_retries']=self.read_retries
        return operation
