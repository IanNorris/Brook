"""Host side of the opt-in RAM maintenance upload protocol; no implicit retries."""
import hashlib, json, socket, struct, zlib
from pathlib import Path
from hardware_bulk_protocol import apply_pacing,validate_pacing
KERNEL_MAX=8*1024*1024
INITRD_MAX=64*1024*1024

def record(generation,slot,kernel,initrd):
    if not (0<generation<2**64 and slot in (0,1) and 64<=len(kernel)<=KERNEL_MAX and 512<=len(initrd)<=INITRD_MAX):
        raise ValueError('Invalid generation, slot or payload length')
    raw=struct.pack('<8sQIIQQ',b'BROOKUP1',generation,slot,0,len(kernel),len(initrd))
    raw+=hashlib.sha256(kernel).digest()+hashlib.sha256(initrd).digest()+bytes(20)
    return raw+struct.pack('<I',zlib.crc32(raw))

def read_payload(path,limit):
    path=Path(path).resolve(strict=True)
    with path.open('rb') as stream:
        payload=stream.read(limit+1)
    if len(payload)>limit:raise ValueError('Payload exceeds capacity: '+str(path))
    return payload

def line(connection):
    raw=bytearray()
    while len(raw)<1024:
        value=connection.recv(1)
        if not value:raise ConnectionError('Updater disconnected; inspect the original guest job before retrying')
        if value==b'\n':return json.loads(raw)
        raw+=value
    raise ValueError('Oversize updater response')

def authenticate(connection,key,boot_id,transaction_id):
    for value in (key,boot_id,transaction_id):
        if len(value)!=32 or any(c not in '0123456789abcdef' for c in value):raise ValueError('Expected 32 lowercase hex characters')
    connection.sendall(b'BRUPLOAD'+key.encode()+boot_id.encode()+transaction_id.encode())
    plan=line(connection)
    if plan.get('state')!='ready':raise RuntimeError(plan)
    return plan

def upload(host,port,key,boot_id,transaction_id,kernel,initrd,activate=False,*,pace_bytes_per_second=None):
    validate_pacing(pace_bytes_per_second)
    with socket.create_connection((host,port),timeout=30) as connection:
        apply_pacing(connection,pace_bytes_per_second)
        connection.settimeout(180)
        plan=authenticate(connection,key,boot_id,transaction_id)
        connection.sendall(record(plan['generation'],plan['slot'],kernel,initrd))
        connection.sendall(kernel);connection.sendall(initrd)
        staged=line(connection)
        if staged.get('state')!='staged':raise RuntimeError(staged)
        if not activate:return {'plan':plan,**staged}
        connection.sendall(b'ACTIVATE')
        activated=line(connection)
        if activated.get('state')!='activated':raise RuntimeError(activated)
        return {'plan':plan,**activated}

def promote(host,port,key,boot_id,transaction_id,expected_slot,kernel,initrd,activate=False,*,expected_generation=None):
    """Select a verified older payload; caller must persist the transaction ID.

    A missing reply after ACTIVATE is uncertain, never automatically retried.
    The exact local payload identity must match the on-disk older selector.
    """
    for value in (key,boot_id,transaction_id):
        if len(value)!=32 or any(c not in '0123456789abcdef' for c in value):raise ValueError('Expected 32 lowercase hex characters')
    if expected_slot not in (0,1):raise ValueError('Expected slot 0 or 1')
    with socket.create_connection((host,port),timeout=30) as connection:
        connection.settimeout(180)
        connection.sendall(b'BRPROMOT'+key.encode()+boot_id.encode()+transaction_id.encode())
        plan=line(connection)
        if (plan.get('state')!='promote_ready' or plan.get('slot')!=expected_slot or
            (expected_generation is not None and plan.get('generation')!=expected_generation)):raise RuntimeError(plan)
        connection.sendall(record(plan['generation'],expected_slot,kernel,initrd))
        staged=line(connection)
        if staged.get('state')!='staged':raise RuntimeError(staged)
        if not activate:return {'plan':plan,**staged}
        connection.sendall(b'ACTIVATE')
        result=line(connection)
        if result.get('state')!='promoted' or result.get('generation')!=plan['generation'] or result.get('slot')!=expected_slot:raise RuntimeError(result)
        return {'plan':plan,**result}
