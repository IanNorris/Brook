"""Host side of the bounded, boot-pinned store stream; no automatic retries."""
import hashlib
import os
import re
import socket
import stat
import struct
from pathlib import Path
from hardware_bulk_protocol import identifier, line


def prepare_source(image, record, expected_sha256):
    if not isinstance(record, bytes) or not 1 <= len(record) <= 96:
        raise ValueError('Invalid bounded store record')
    if not isinstance(expected_sha256, str) or len(expected_sha256) != 64:
        raise ValueError('Expected a SHA256 digest')
    digest = bytes.fromhex(expected_sha256)
    match = re.fullmatch(rb'BRKSTORE1 ([0-9]+) ([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\n?',record)
    if not match or match.group(2)==b'00000000-0000-0000-0000-000000000000':raise ValueError('Invalid store record')
    image = Path(image).resolve(strict=True)
    with image.open('rb') as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or not 16777216 <= before.st_size <= 0xfffffe00 or before.st_size%4096 or int(match.group(1))!=before.st_size:
            raise ValueError('Invalid store image extent')
        if hashlib.file_digest(source, 'sha256').digest() != digest:
            raise ValueError('Source image does not match prepared manifest')
        after=os.fstat(source.fileno())
        identity=lambda s:(s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
        if identity(before)!=identity(after):raise OSError('Source changed during validation')
        return {'path':str(image),'identity':identity(before),'sha256':expected_sha256,'record':record}

def provision(host, port, key, boot, transaction, image, record, expected_sha256, prepared=None):
    for value in (key, boot, transaction): identifier(value)
    prepared=prepared or prepare_source(image,record,expected_sha256)
    image=Path(image).resolve(strict=True)
    if prepared['path']!=str(image) or prepared['record']!=record or prepared['sha256']!=expected_sha256:raise ValueError('Source preparation does not match request')
    digest=bytes.fromhex(expected_sha256)
    with image.open('rb') as source:
        before=os.fstat(source.fileno())
        if (before.st_dev,before.st_ino,before.st_size,before.st_mtime_ns,before.st_ctime_ns)!=prepared['identity']:raise OSError('Prepared source changed')
        source.seek(0)
        with socket.create_connection((host, port), timeout=30) as connection:
            connection.settimeout(1800)  # the receiver hashes the full USB image before staging
            connection.sendall(b'BRSTORE1'+key.encode()+boot.encode()+transaction.encode())
            if line(connection) != {'state':'ready','store_api':1}:
                raise ValueError('Unsupported store receiver')
            connection.sendall(struct.pack('<I', len(record))+digest+record+b'BEGIN001')
            if line(connection) != {'state':'receiving'}:
                raise ValueError('Store receiver refused to begin')
            remaining=before.st_size; sent_hash=hashlib.sha256()
            while remaining:
                data=source.read(min(65536,remaining))
                if not data: raise OSError('Source shortened during transfer')
                connection.sendall(data);sent_hash.update(data);remaining-=len(data)
            after=os.fstat(source.fileno())
            if (before.st_size,before.st_mtime_ns,before.st_ctime_ns)!=(after.st_size,after.st_mtime_ns,after.st_ctime_ns) or sent_hash.digest()!=digest:
                raise OSError('Source changed during transfer; inspect the original job before retrying')
            if line(connection) != {'state':'staged','published':False}:
                raise ValueError('Store receiver did not verify the image')
            connection.sendall(b'FINALIZE')
            result=line(connection)
            if result != {'state':'completed','published':True,'durable':True}:
                raise ValueError('Store publication was not confirmed; inspect the original job')
            return {'transaction_id':transaction,'bytes':before.st_size,'sha256':expected_sha256,**result}
