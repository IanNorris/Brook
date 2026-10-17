"""Selectable VM observations; component snapshots are deliberately not called atomic."""
import os
import hashlib
from pathlib import Path
import re
import time
from jobs import validate_id

COMPONENTS={'frame','windows','processes','launches','errors'}
MASK={'windows':1,'processes':2,'launches':4}
ERROR=re.compile(rb'\[FAULT\] User fault vec=|KERNEL (?:PANIC|FAULT)|ASSERT(?:ION)?(?: FAILED|:)|exit_group timeout|dispatch failed|(?:virtio|ext2|EXT2).*?(?:error|timeout|corrupt)|(?:error|failed):|Segmentation fault',re.I)


def validate_cursor(cursor):
    if cursor is None:return
    if not isinstance(cursor,dict) or set(cursor)!={'session','generation','guest_boot_id','file_identity','offset','discard_until_newline','source','anchor_sha256'}:
        raise ValueError('Use the exact errors next_cursor returned by vm_observe')
    validate_id(cursor['session'])
    if not isinstance(cursor['generation'],str) or not cursor['generation']:raise ValueError('Invalid error cursor generation')
    if cursor['guest_boot_id'] is not None:validate_id(cursor['guest_boot_id'])
    if not isinstance(cursor['file_identity'],str) or not re.fullmatch(r'[0-9]+:[0-9]+',cursor['file_identity']):raise ValueError('Invalid error cursor file identity')
    if not isinstance(cursor['offset'],str) or not re.fullmatch(r'0|[1-9][0-9]{0,18}',cursor['offset']) or int(cursor['offset'])>2**63-1:raise ValueError('Invalid error cursor offset')
    if not isinstance(cursor['anchor_sha256'],str) or not re.fullmatch(r'[0-9a-f]{64}',cursor['anchor_sha256']):raise ValueError('Invalid error cursor anchor')
    if not isinstance(cursor['discard_until_newline'],bool) or cursor['source']!='serial':raise ValueError('Invalid error cursor source/state')


def read_errors(data,boot_id,cursor=None,limit_bytes=65536,limit_events=64):
    validate_cursor(cursor)
    if isinstance(limit_bytes,bool) or not isinstance(limit_bytes,int) or not 1024<=limit_bytes<=262144:raise ValueError('error_limit_bytes must be 1024–262144')
    if isinstance(limit_events,bool) or not isinstance(limit_events,int) or not 1<=limit_events<=128:raise ValueError('error_limit_events must be 1–128')
    if cursor and (cursor['session']!=data['session'] or cursor['generation']!=data['generation']):raise ValueError('Stale error cursor session/generation')
    started=time.monotonic();events=[];discarded=0;reset=None;incomplete=False
    with Path(data['serial_log']).open('rb') as f:
        stat=os.fstat(f.fileno());identity=f'{stat.st_dev}:{stat.st_ino}';size=stat.st_size
        origin=max(0,data.get('generation_log_offset',0))
        if origin>size:origin=0
        anchor_matches=True
        if cursor and cursor['file_identity']==identity and int(cursor['offset'])<=size:
            offset=int(cursor['offset']);f.seek(max(0,offset-64))
            anchor_matches=hashlib.sha256(f.read(min(64,offset))).hexdigest()==cursor['anchor_sha256']
        if cursor and cursor['file_identity']==identity and int(cursor['offset'])<=size and anchor_matches:
            start=int(cursor['offset']);discard=cursor['discard_until_newline'];excluded=0
        else:
            reset=('file_rotated' if cursor and cursor['file_identity']!=identity else 'file_rewritten' if cursor and not anchor_matches else 'file_truncated' if cursor else 'initial_tail')
            if cursor:origin=0
            start=max(origin,size-limit_bytes);discard=start>origin;excluded=start-origin
        f.seek(start);chunk=f.read(min(limit_bytes,max(0,size-start)));consumed=0
        if discard:
            newline=chunk.find(b'\n')
            consumed=len(chunk) if newline<0 else newline+1
            discarded+=consumed;discard=newline<0
        while consumed<len(chunk) and not discard:
            newline=chunk.find(b'\n',consumed)
            if newline<0:
                # Keep a short unfinished line for the next call. An oversized
                # line must make bounded progress without classifying its suffix.
                incomplete=True
                if consumed==0 and len(chunk)==limit_bytes:
                    discarded+=len(chunk);consumed=len(chunk);discard=True
                break
            line=chunk[consumed:newline].rstrip(b'\r');offset=start+consumed;consumed=newline+1
            if ERROR.search(line):
                events.append({'sequence':str(offset),'end_offset':str(start+consumed),
                    'classification':'panic' if re.search(rb'KERNEL (?:PANIC|FAULT)',line,re.I) else 'user_fault' if b'[FAULT] User fault vec=' in line else 'diagnostic_match',
                    'text':line[:2048].decode(errors='replace'),'text_truncated_bytes':max(0,len(line)-2048),
                    'guest_tick_ms':None})
                if len(events)>=limit_events:break
        next_offset=start+consumed;f.seek(max(0,next_offset-64))
        anchor=hashlib.sha256(f.read(min(64,next_offset))).hexdigest()
    return {'source':'serial','detection':'bounded serial-pattern scan; not a lossless guest event stream',
        'scope':'Lines can span guest boots; per-line guest boot/timestamps are unknown',
        'events':events,'next_cursor':{'session':data['session'],'generation':data['generation'],
            'guest_boot_id':boot_id,'file_identity':identity,'offset':str(next_offset),
            'discard_until_newline':discard,'source':'serial','anchor_sha256':anchor},
        'reset_reason':reset,'guest_boot_changed':bool(cursor and cursor['guest_boot_id'] and boot_id and cursor['guest_boot_id']!=boot_id),
        'guest_boot_verified':boot_id is not None,'history_excluded_bytes':excluded,
        'dropped_bytes':None if reset in {'file_rotated','file_truncated','file_rewritten'} else excluded+discarded,
        'discarded_bytes_this_read':discarded,'pending_line_bytes':len(chunk)-consumed if incomplete else 0,
        'more':next_offset<size,'file_size_at_start':size,
        'capture_started_host_monotonic':started,'capture_ended_host_monotonic':time.monotonic()}


class Observations:
    def __init__(self,ui,capture,panic_state):
        self.ui,self.capture,self.panic_state=ui,capture,panic_state

    def collect(self,session,include,crop=None,max_width=None,cursor=None,error_limit_bytes=65536,error_limit_events=64):
        if not isinstance(include,list) or not include or len(include)>5 or any(not isinstance(x,str) or x not in COMPONENTS for x in include) or len(set(include))!=len(include):
            raise ValueError('include requires distinct frame/windows/processes/launches/errors components')
        if (crop is not None or max_width is not None) and 'frame' not in include:raise ValueError('crop/max_width require the frame component')
        if cursor is not None and 'errors' not in include:raise ValueError('since_event_cursor requires errors')
        validate_cursor(cursor)
        if isinstance(error_limit_bytes,bool) or not isinstance(error_limit_bytes,int) or not 1024<=error_limit_bytes<=262144:raise ValueError('error_limit_bytes must be 1024–262144')
        if isinstance(error_limit_events,bool) or not isinstance(error_limit_events,int) or not 1<=error_limit_events<=128:raise ValueError('error_limit_events must be 1–128')
        started=time.monotonic();data=self.ui.load_session(session,live=False);generation=data['generation']
        if cursor and (cursor['session']!=session or cursor['generation']!=generation):raise ValueError('Stale error cursor session/generation')
        result={'session':session,'generation':generation,'guest_boot_id':None,'observation_api':1,'components':{},
            'consistency':{'atomic':False,'frame_window_correspondence':'unverified','window_state_revision':None,
                'note':'Each component has its own capture interval; frame pixels, WM values, task counters and log lines can describe different instants'}}
        parts=result['components'];boot=None;caps={};cap_error=None
        live=data.get('state')=='running';panic=False
        if live:
            try:panic=self.panic_state(data)
            except OSError as error:cap_error='Panic-state check unavailable: '+str(error)
        if live and not panic and cap_error is None:
            try:
                _,caps=self.ui.capabilities(session,time.monotonic()+20);boot=caps['boot_id']
                result['guest_boot_id']=boot
            except (OSError,ValueError,RuntimeError) as error:cap_error=str(error)
        elif cap_error is None:cap_error='Guest has panicked' if panic else 'Guest is stopped'
        result['guest_status']={'state':'panicked' if panic else 'stopped' if not live else 'unavailable' if cap_error else 'running'}
        if cap_error:result['guest_status']['detail']=cap_error
        if live and cap_error and not panic:result['identity_error']=cap_error
        def component(name,fn):
            begin=time.monotonic()
            try:parts[name]={'state':'ok','result':fn()}
            except (OSError,ValueError,RuntimeError) as error:parts[name]={'state':'unavailable','error':str(error)}
            parts[name].update(capture_started_host_monotonic=begin,capture_ended_host_monotonic=time.monotonic())
        if 'frame' in include:
            if live and (panic or (boot and caps.get('frame_epoch_guard')==1)):
                def frame():
                    with self.ui.session_lock(session) as current:
                        if current['generation']!=generation:raise RuntimeError('Guest generation changed')
                        return self.capture(session,current,crop,max_width,boot_id=boot)
                component('frame',frame)
                if panic and parts['frame']['state']=='ok':parts['frame'].update(state='unverified',error='Panic framebuffer uses cached geometry whose current-boot provenance is unverified')
            else:parts['frame']={'state':'unavailable','error':cap_error or 'Guest boot-guarded frame capture unsupported'}
        mask=sum(MASK[x] for x in include if x in MASK)
        if mask:
            begin=time.monotonic()
            try:
                if cap_error:raise RuntimeError(cap_error)
                if caps.get('observation_api')!=1:raise RuntimeError('Guest observation bundles unsupported; update the kernel')
                reply=self.ui.query(session,generation,f'bridge ui observe {mask}',time.monotonic()+20,boot_id=boot)
                if reply.get('observation_api')!=1:raise RuntimeError('Invalid guest observation reply')
                for name in include:
                    if name not in MASK:continue
                    value=reply[name]
                    if not isinstance(value,dict):raise RuntimeError('Missing guest observation component')
                    parts[name]={'state':'unavailable','error':value['error']} if 'error' in value else {'state':'ok','result':value}
                    parts[name].update(capture_started_host_monotonic=begin,capture_ended_host_monotonic=time.monotonic())
                self.references(parts,generation,boot)
            except (OSError,ValueError,RuntimeError,KeyError) as error:
                for name in include:
                    if name in MASK:parts[name]={'state':'unavailable','error':str(error),
                        'capture_started_host_monotonic':begin,'capture_ended_host_monotonic':time.monotonic()}
        if 'errors' in include:
            component('errors',lambda:read_errors(data,boot,cursor,error_limit_bytes,error_limit_events))
        # Every guarded component excludes another boot individually. Check the
        # end too, so an intervening reboot cannot produce a plausible mixed bundle.
        result['identity_verified_at_end']=False
        try:
            current=self.ui.load_session(session,live=False)
            if current['generation']!=generation:raise RuntimeError('Guest generation changed during observation')
            if boot:
                identity=self.ui.query(session,generation,'bridge epoch',time.monotonic()+10)
                if identity.get('boot_id')!=boot:raise RuntimeError('Guest boot changed during observation')
                result['identity_verified_at_end']=True
        except (OSError,ValueError,RuntimeError) as error:
            result['identity_error']=str(error)
            for name,part in parts.items():
                if name!='errors' and part['state']=='ok':part.update(state='stale',error=str(error))
        ended=time.monotonic();result.update(capture_started_host_monotonic=started,capture_ended_host_monotonic=ended,readonly_transport_retries=self.ui.read_retries)
        for part in parts.values():
            if 'capture_ended_host_monotonic' in part:part['age_at_bundle_end_s']=max(0,ended-part['capture_ended_host_monotonic'])
        result['state']='completed' if 'identity_error' not in result and all(x['state']=='ok' for x in parts.values()) else 'partial'
        return result

    @staticmethod
    def references(parts,generation,boot):
        def ref(pid,inc):return {'pid':pid,'incarnation':inc,'session_generation':generation,'guest_boot_id':boot}
        for row in (parts.get('windows',{}).get('result') or {}).get('windows',[]):
            row['reference']={'window_id':row['window_id'],'incarnation':row['incarnation'],'session_generation':generation,'guest_boot_id':boot}
            for field in ('client','provider'):
                if row.get(field):row[field]['reference']=ref(row[field]['pid'],row[field]['incarnation'])
        for row in (parts.get('processes',{}).get('result') or {}).get('tasks',[]):
            row['reference']=ref(row['pid'],row['incarnation']);row['group_reference']=ref(row['tgid'],row['group_incarnation'])
        for row in (parts.get('launches',{}).get('result') or {}).get('launches',[]):
            row['reference']={'token':row['token'],'session_generation':generation,'guest_boot_id':boot}
            if row.get('window'):row['window_reference']={**row['window'],'session_generation':generation,'guest_boot_id':boot}
