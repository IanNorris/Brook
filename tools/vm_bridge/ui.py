"""Semantic compositor requests with durable identities and read-only recovery."""
import hashlib
import json
import re
import time

from jobs import Jobs, GuestJobError, validate_id


class UiBlocked(RuntimeError):
    pass


class Ui(Jobs):
    directory_name = 'ui'
    actions = {'focus':0, 'minimize':1, 'maximize':2, 'restore':3, 'close':4}
    restores = {'previous':0, 'normal':1, 'maximized':2}

    def before_exchange(self, session, command):
        if command.startswith(('bridge ui window ','bridge ui launch ')):
            path=self.registry/session/'control.json'
            if path.exists() and json.loads(path.read_text()).get('user_takeover'):
                raise UiBlocked('Human takeover is active')

    def capabilities(self, session, deadline=None):
        data=self.load_session(session)
        caps=self.query(session,data['generation'],'bridge ui caps',deadline)
        if caps.get('ui_api')!=1:
            raise RuntimeError('Guest semantic window controls are unsupported; update the guest kernel')
        caps['boot_id']=self.boot_identity(session,data['generation'],deadline)
        return data,caps

    def windows(self, session, deadline=None, generation=None, boot_id=None):
        if generation is None:
            data,caps=self.capabilities(session,deadline)
            generation,boot_id=data['generation'],caps['boot_id']
        started=time.monotonic()
        reply=self.query(session,generation,'bridge ui windows',deadline,boot_id=boot_id)
        for row in reply['windows']:
            row['reference']={'window_id':row['window_id'],'incarnation':row['incarnation'],
                'session_generation':generation,'guest_boot_id':boot_id}
        return {'session':session,'generation':generation,'guest_boot_id':boot_id,**reply,
            'capture_started_host_monotonic':started,'capture_ended_host_monotonic':time.monotonic(),
            'consistency':'Window values captured under one WM lock; process liveness sampled afterwards; no frame correspondence asserted'}

    def apps(self,session):
        deadline=time.monotonic()+20
        data,caps=self.capabilities(session,deadline)
        if caps.get('apps_api')!=1:raise RuntimeError('Guest app discovery is unsupported; update the guest kernel')
        generation,boot=data['generation'],caps['boot_id']
        started=time.monotonic()
        reply=self.query(session,generation,'bridge ui apps',deadline,boot_id=boot)
        for app in reply['apps']:
            app['reference']={'app_id':app['app_id'],'catalog_generation':reply['catalog_generation'],
                'session_generation':generation,'guest_boot_id':boot}
        return {'session':session,'generation':generation,'guest_boot_id':boot,**reply,
            'capture_started_host_monotonic':started,'capture_ended_host_monotonic':time.monotonic()}

    @staticmethod
    def validate_app(app):
        if not isinstance(app,dict) or set(app)!={'app_id','catalog_generation','session_generation','guest_boot_id'}:
            raise ValueError('Use an exact app reference returned by vm_apps')
        validate_id(app['app_id']);validate_id(app['guest_boot_id'])
        if app['guest_boot_id']=='0'*32:raise ValueError('Guest boot identity is unset')
        gen=app['catalog_generation']
        if isinstance(gen,bool) or not isinstance(gen,int) or not 1<=gen<=2**32-1:
            raise ValueError('Invalid catalog generation')
        if not isinstance(app['session_generation'],str) or not app['session_generation']:
            raise ValueError('Invalid session generation')

    def launch(self,session,app,request_id,wait_for='window',wait_s=2,cancel=None):
        deadline=time.monotonic()+20
        self.validate_app(app);validate_id(request_id);self.validate_wait(wait_s)
        if wait_for not in {'admitted','dispatched','window'}:raise ValueError('Invalid launch wait condition')
        data,caps=self.capabilities(session,deadline)
        if caps.get('launch_api')!=1:raise RuntimeError('Guest semantic launcher is unsupported; update the guest kernel')
        boot=caps['boot_id']
        if data['generation']!=app['session_generation']:raise ValueError('Stale session generation')
        if boot!=app['guest_boot_id']:raise ValueError('Stale guest boot')
        arguments={'kind':'launch','app':app}
        fingerprint=hashlib.sha256(json.dumps(arguments,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        path=self.path(session,request_id)
        with self.lock(path,deadline=deadline):
            if path.exists():
                operation=json.loads(path.read_text())
                if operation['fingerprint']!=fingerprint:raise ValueError('request_id is already bound to different arguments')
            else:
                operation={'session':session,'generation':data['generation'],'guest_boot_id':boot,
                    'operation_id':request_id,'request_id':request_id,'fingerprint':fingerprint,
                    **arguments,'state':'queued','effect':'queued','stage':'created',
                    'created_host_monotonic':time.monotonic()}
                self.write_json(path,operation)
                operation.update(stage='dispatching',effect='unknown')
                self.write_json(path,operation)
                try:
                    reply=self.query(session,data['generation'],
                        f"bridge ui launch {request_id} {app['app_id']} {app['catalog_generation']}",deadline,boot_id=boot)
                    self.update(operation,reply)
                except (OSError,ValueError,RuntimeError) as error:
                    state='lost' if str(error)=='Stale guest boot' else 'rejected' if isinstance(error,(UiBlocked,GuestJobError)) else 'unknown'
                    operation.update(state=state,effect='rejected' if state=='rejected' else 'unknown',error=str(error))
                    if state=='rejected':operation['stage']='host_rejected'
                self.write_json(path,operation)
        return self.status(session,request_id,wait_s,wait_for,cancel,deadline)

    @staticmethod
    def validate_reference(window):
        if not isinstance(window,dict) or set(window)!={'window_id','incarnation','session_generation','guest_boot_id'}:
            raise ValueError('Use an exact window reference returned by vm_windows')
        ident=window['window_id']
        if isinstance(ident,bool) or not isinstance(ident,int) or not 1<=ident<=256:
            raise ValueError('Invalid window_id')
        if not isinstance(window['incarnation'],str) or not re.fullmatch(r'[1-9][0-9]{0,19}',window['incarnation']) or int(window['incarnation'])>2**64-1:
            raise ValueError('Window incarnation must be a nonzero uint64 decimal string')
        if not isinstance(window['session_generation'],str) or not window['session_generation']:
            raise ValueError('Invalid session generation')
        validate_id(window['guest_boot_id'])
        if window['guest_boot_id']=='0'*32:raise ValueError('Guest boot identity is unset')

    def submit(self,session,window,action,request_id,restore_target=None,wait_for='applied',wait_s=2,cancel=None):
        deadline=time.monotonic()+20
        self.validate_reference(window);validate_id(request_id);self.validate_wait(wait_s)
        if action not in self.actions:raise ValueError('Unsupported window action')
        if action=='restore':
            if restore_target not in self.restores:raise ValueError('restore_target must be previous, normal or maximized')
        elif restore_target is not None:raise ValueError('restore_target requires action restore')
        if wait_for not in {'applied','closed','client_group_exit'} or (wait_for!='applied' and action!='close'):
            raise ValueError('wait_for must be applied, or closed/client_group_exit for close')
        data=self.load_session(session)
        if data['generation']!=window['session_generation']:raise ValueError('Stale session generation')
        # Reject changed boots before allocating a durable operation identity.
        boot=self.boot_identity(session,data['generation'],deadline)
        if boot!=window['guest_boot_id']:raise ValueError('Stale guest boot')
        arguments={'window':window,'action':action,'restore_target':restore_target}
        fingerprint=hashlib.sha256(json.dumps(arguments,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        path=self.path(session,request_id)
        with self.lock(path,deadline=deadline):
            if path.exists():
                operation=json.loads(path.read_text())
                if operation['fingerprint']!=fingerprint:raise ValueError('request_id is already bound to different arguments')
            else:
                operation={'session':session,'generation':data['generation'],'guest_boot_id':boot,
                    'operation_id':request_id,'request_id':request_id,'fingerprint':fingerprint,
                    **arguments,'state':'queued','effect':'queued','stage':'created',
                    'created_host_monotonic':time.monotonic()}
                self.write_json(path,operation)
                # Persist dispatch intent before crossing the transport. An
                # interrupted/unknown mutation is recovered only by status.
                operation.update(stage='dispatching',effect='unknown')
                self.write_json(path,operation)
                try:
                    command=f"bridge ui window {request_id} {window['window_id']} {window['incarnation']} {self.actions[action]} {self.restores.get(restore_target,0)}"
                    reply=self.query(session,data['generation'],command,deadline,boot_id=boot)
                    self.update(operation,reply)
                except (OSError,ValueError,RuntimeError) as error:
                    state='lost' if str(error)=='Stale guest boot' else 'rejected' if isinstance(error,(UiBlocked,GuestJobError)) else 'unknown'
                    operation.update(state=state,effect='rejected' if state=='rejected' else 'unknown',error=str(error))
                    if state=='rejected':operation['stage']='host_rejected'
                self.write_json(path,operation)
        return self.status(session,request_id,wait_s,wait_for,cancel,deadline)

    @staticmethod
    def update(operation,reply):
        operation.update(guest=reply,state=reply['state'],stage=reply['state'],
            effect='confirmed' if reply['state']=='completed' else 'rejected' if reply['state']=='rejected' else reply['state'])
        if operation.get('kind')=='launch':
            launch=reply.get('launch') or {}
            operation.update(admission_state=reply['state'],coalesced=reply.get('coalesced',False),launch=launch or None)
            if launch:
                operation.update(state=launch['state'],effect='confirmed' if launch.get('dispatch_succeeded') else
                    'rejected' if launch.get('dispatch_finished') or launch.get('cancelled') else 'unknown')
            operation['effect_scope']='Launcher admission and raw dispatch; window presence is correlation, not application readiness or activation acknowledgement'
            operation.pop('error',None)
            return
        operation.pop('error',None)
        operation['effect_scope']='WM state applied / graceful close enqueued; client acknowledgement and exit are separate'

    def status(self,session,operation_id,wait_s=0,wait_for='applied',cancel=None,_deadline=None):
        self.validate_wait(wait_s)
        if wait_for not in {'applied','closed','client_group_exit','admitted','dispatched','window'}:raise ValueError('Invalid wait condition')
        deadline=time.monotonic()+20 if _deadline is None else _deadline
        end=min(deadline,time.monotonic()+wait_s)
        path=self.path(session,operation_id)
        observation=None;conditions={};timed_out=False
        while True:
            with self.lock(path,deadline=deadline):
                operation=json.loads(path.read_text())
                is_launch=operation.get('kind')=='launch'
                if is_launch and wait_for not in {'applied','admitted','dispatched','window'}:
                    raise ValueError('Invalid launch wait condition')
                if not is_launch and wait_for not in {'applied','closed','client_group_exit'}:
                    raise ValueError('Invalid window wait condition')
                data=self.load_session(session,live=False)
                if data['generation']!=operation['generation']:
                    operation.update(state='lost',effect='unknown',error='Guest generation changed')
                elif data.get('state')!='running':
                    terminal=operation['state'] in {'completed','rejected','window_observed','failed','cancelled','timed_out'}
                    # Shutdown loses future observation, not established dispatch facts.
                    known_dispatch=is_launch and (operation.get('launch') or {}).get('dispatch_finished')
                    return {**operation,'recorded_state':operation['state'],
                        'state':operation['state'] if terminal else 'lost',
                        'effect':operation['effect'] if terminal or known_dispatch else 'unknown',
                        'cached':True,'current_window':None,
                        'error':'Guest is stopped; returning last recorded dispatch evidence; current windows unavailable',
                        'wait_condition':wait_for,'wait_condition_met':False}
                elif operation['stage']!='host_rejected':
                    try:
                        reply=self.query(session,operation['generation'],f'bridge ui status {operation_id}',deadline,boot_id=operation['guest_boot_id'])
                        self.update(operation,reply)
                        operation.pop('observation_error',None)
                    except (OSError,ValueError,RuntimeError) as error:
                        operation['observation_error']=str(error)
                        if str(error)=='Stale guest boot':operation.update(state='lost',effect='unknown')
                self.write_json(path,operation)
            if operation['state']=='lost':break
            if is_launch:
                launch=operation.get('launch') or {}
                admitted=operation.get('admission_state')=='completed'
                dispatched=launch.get('dispatch_finished') is True and launch.get('dispatch_succeeded') is True
                conditions={'applied':admitted,'admitted':admitted,'dispatched':dispatched,'window':False}
            try:
                observation=self.windows(session,deadline,operation['generation'],operation['guest_boot_id'])
                if is_launch:
                    launch=operation.get('launch') or {}
                    ref=launch.get('window') or {}
                    current=next((row for row in observation['windows'] if row['window_id']==ref.get('window_id') and row['incarnation']==ref.get('incarnation')),None)
                    admitted=operation.get('admission_state')=='completed'
                    dispatched=launch.get('dispatch_finished') is True and launch.get('dispatch_succeeded') is True
                    conditions={'applied':admitted,'admitted':admitted,'dispatched':dispatched,
                        'window':dispatched and current is not None}
                    if ref:
                        launch['window_reference']={**ref,'session_generation':operation['generation'],
                            'guest_boot_id':operation['guest_boot_id']}
                else:
                    ref=operation['window']
                    current=next((row for row in observation['windows'] if row['window_id']==ref['window_id'] and row['incarnation']==ref['incarnation']),None)
                    applied=operation['state']=='completed'
                    conditions={'applied':applied,'closed':applied and current is None,'client_group_exit':False}
                    client=operation.get('guest',{}).get('applied_window') or {}
                    client=client.get('client')
                    if applied and client is not None and wait_for=='client_group_exit':
                        live=self.query(session,operation['generation'],f"bridge ui process {client['pid']} {client['incarnation']}",deadline,boot_id=operation['guest_boot_id'])
                        conditions['client_group_exit']=live['live_group_tasks']==0
                        operation['client_group_observation']=live
                operation['current_window']=current
            except (OSError,ValueError,RuntimeError) as error:
                operation['observation_error']=str(error)
                if str(error)=='Stale guest boot':operation.update(state='lost',effect='unknown');conditions={}
                break
            if conditions.get(wait_for) or operation['state'] in {'rejected','lost','failed','cancelled','timed_out'}:break
            if time.monotonic()>=end:timed_out=wait_s>0;break
            delay=min(.1,max(0,end-time.monotonic()))
            if cancel:
                if cancel.wait(delay):operation['wait_cancelled']=True;break
            else:time.sleep(delay)
        return {**operation,'observation':observation,'conditions':conditions,'wait_condition':wait_for,
            'wait_condition_met':conditions.get(wait_for,False),'wait_timed_out':timed_out,
            'client_exit_scope':'Exact registered connection-origin group, not all application descendants',
            'readonly_transport_retries':self.read_retries}

    def takeover(self,session,enabled):
        deadline=time.monotonic()+5
        data,caps=self.capabilities(session,deadline)
        return self.query(session,data['generation'],f'bridge ui takeover {int(enabled)}',deadline,boot_id=caps['boot_id'])
