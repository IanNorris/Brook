"""Bounded binary data plane. Public results contain metadata and paths only."""
import hashlib,json,os,socket,stat,struct,zlib
from pathlib import Path

LIMIT=1024**3
def identifier(value):
    if not isinstance(value,str) or len(value)!=32 or any(c not in '0123456789abcdef' for c in value):
        raise ValueError('Expected 32 lowercase hex characters')
    return value

def line(connection):
    data=bytearray()
    while len(data)<4096:
        byte=connection.recv(1)
        if not byte:raise ConnectionError('Bulk connection closed; inspect the original guest job before retrying')
        if byte==b'\n':
            result=json.loads(data)
            if not isinstance(result,dict):raise ValueError('Invalid bulk response')
            if result.get('state')=='failed':raise RuntimeError(result)
            return result
        data+=byte
    raise ValueError('Oversized bulk response')

def authenticate(connection,key,boot,transaction):
    connection.sendall(b'BRBULK01'+b''.join(identifier(v).encode() for v in (key,boot,transaction)))
    ready=line(connection)
    if ready!={'state':'ready','bulk_api':1}:raise ValueError('Unsupported bulk receiver')

def upload(host,port,key,boot,transaction,path,finalize=True):
    path=Path(path).resolve(strict=True)
    with path.open('rb') as source:
        before=os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0<=before.st_size<=LIMIT:raise ValueError('Source must be a regular file of at most1GiB')
        digest=hashlib.file_digest(source,'sha256').digest();source.seek(0)
        with socket.create_connection((host,port),timeout=30) as connection:
            connection.settimeout(60);authenticate(connection,key,boot,transaction)
            connection.sendall(struct.pack('<Q',before.st_size)+digest)
            sent=0
            while sent<before.st_size:
                data=source.read(min(65536,before.st_size-sent))
                if not data:raise OSError('Source shortened during transfer')
                connection.sendall(data);sent+=len(data)
            after=os.fstat(source.fileno())
            if (before.st_size,before.st_mtime_ns,before.st_ctime_ns)!=(after.st_size,after.st_mtime_ns,after.st_ctime_ns):raise OSError('Source changed during transfer')
            staged=line(connection)
            if staged!={'state':'staged','published':False}:raise ValueError('Receiver did not verify staging')
            if not finalize:return staged
            connection.sendall(b'FINALIZE');completed=line(connection)
            if (completed.get('state'),completed.get('transaction_id'),completed.get('action'),completed.get('bytes'),completed.get('sha256'))!=('completed',transaction,'put',sent,digest.hex()):raise ValueError('Final receipt mismatch')
            return completed

def download(host,port,key,boot,transaction,path,action='get'):
    if action not in ('get','frame'):raise ValueError('Invalid download action')
    path=Path(path).absolute();partial=path.with_name(path.name+'.part')
    # Exclusive temporary creation; final no-replace publication occurs only
    # after payload hash AND the final guest receipt have both been verified.
    fd=os.open(partial,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    try:
        with os.fdopen(fd,'wb') as output,socket.create_connection((host,port),timeout=30) as connection:
            connection.settimeout(60);authenticate(connection,key,boot,transaction);meta=line(connection)
            count=meta.get('bytes')
            if meta.get('state')!='payload' or isinstance(count,bool) or not isinstance(count,int) or not 0<=count<=LIMIT:raise ValueError('Invalid binary extent')
            digest=meta.get('sha256')
            if not isinstance(digest,str) or len(digest)!=64 or any(c not in '0123456789abcdef' for c in digest):raise ValueError('Invalid payload hash')
            if action=='frame':
                w,h=meta.get('width'),meta.get('height')
                if isinstance(w,bool) or isinstance(h,bool) or not isinstance(w,int) or not isinstance(h,int) or not 0<w<=4096 or not 0<h<=2160 or meta.get('format')!=1 or count!=w*h*4:raise ValueError('Invalid frame geometry')
            remaining=count;hasher=hashlib.sha256()
            while remaining:
                data=connection.recv(min(65536,remaining))
                if not data:raise ConnectionError('Truncated binary payload')
                output.write(data);hasher.update(data);remaining-=len(data)
            if hasher.hexdigest()!=digest:raise ValueError('Payload SHA256 mismatch')
            completed=line(connection)
            if (completed.get('state'),completed.get('transaction_id'),completed.get('action'),completed.get('bytes'),completed.get('sha256'))!=('completed',transaction,action,count,digest):raise ValueError('Final receipt mismatch')
            output.flush();os.fsync(output.fileno())
        os.link(partial,path)
        return {**completed,'artifact_path':str(path),'payload':meta}
    finally:
        # Keep no unverified artifact under the public name. Never remove an
        # existing destination belonging to the user.
        partial.unlink(missing_ok=True)

def frame_png(raw,output,metadata):
    """Convert verified packed BGRA bytes on the host; no image JSON/base64."""
    w,h=metadata['width'],metadata['height']
    if metadata.get('format')!=1 or not 0<w<=4096 or not 0<h<=2160 or Path(raw).stat().st_size!=w*h*4:raise ValueError('Invalid frame artifact')
    def chunk(stream,kind,data):
        stream.write(struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff))
    with open(raw,'rb') as source,open(output,'xb') as target:
        target.write(b'\x89PNG\r\n\x1a\n');chunk(target,b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0));compressor=zlib.compressobj()
        for _ in range(h):
            bgra=source.read(w*4);rgb=bytearray(w*3)
            rgb[0::3]=bgra[2::4];rgb[1::3]=bgra[1::4];rgb[2::3]=bgra[0::4]
            data=compressor.compress(b'\0'+rgb)
            if data:chunk(target,b'IDAT',data)
        chunk(target,b'IDAT',compressor.flush());chunk(target,b'IEND',b'')
    return str(Path(output).absolute())
