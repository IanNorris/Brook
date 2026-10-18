import copy
from pathlib import Path
import tempfile
import unittest
import sys
sys.path.insert(0,str(Path(__file__).parent/'workloads'))
from close_review_apps import close_apps


def observation(windows=True,tasks=True,*,token=3,truncated=False,boot='a'):
    return {'generation':'generation','guest_boot_id':boot,'identity_verified_at_end':True,'components':{
        'windows':{'state':'ok','result':{'windows':[{'app_id':'codium','launch_id':token,
            'reference':{'window_id':1,'incarnation':'1'}}] if windows else []}},
        'processes':{'state':'ok','result':{'truncated':truncated,'tasks':[
            {'name':'shutdown-helper','launch_id':str(token),'kernel':False}] if tasks else []}}}}


class Bridge:
    def __init__(self,*states,fail_close=False):self.states=list(states);self.last=None;self.closes=[];self.fail_close=fail_close
    def observe(self,*args,**kwargs):
        if self.states:self.last=self.states.pop(0)
        return copy.deepcopy(self.last)
    def window_action(self,session,reference,action,request_id,**kwargs):
        self.closes.append(request_id)
        if self.fail_close:raise TimeoutError('delivery unknown')
        return {'state':'completed'}


class AppExitTests(unittest.TestCase):
    def test_window_exit_does_not_hide_shutdown_helper(self):
        with tempfile.TemporaryDirectory() as tmp:
            b=Bridge(observation(),observation(windows=False),observation(False,False))
            r=close_apps(b,'session',tmp,['codium'],sleep=lambda _:None)
            self.assertTrue(r['ready_for_checked_stop']);self.assertEqual(len(b.closes),1)
            self.assertEqual(r['launch_tokens'],['3'])

    def test_explicit_lineage_survives_missing_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            b=Bridge(observation(False,True),observation(False,False))
            r=close_apps(b,'session',tmp,['codium'],['3'],sleep=lambda _:None)
            self.assertTrue(r['ready_for_checked_stop']);self.assertEqual(b.closes,[])

    def test_unknown_close_reuses_request_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            first=Bridge(observation(),fail_close=True)
            with self.assertRaises(TimeoutError):close_apps(first,'session',tmp,['codium'])
            second=Bridge(observation(),observation(False,False))
            close_apps(second,'session',tmp,['codium'],sleep=lambda _:None)
            self.assertEqual(first.closes,second.closes)

    def test_missing_lineage_and_truncated_census_are_not_acceptance(self):
        for state in (observation(token=0),observation(truncated=True),observation(False,False)):
            with tempfile.TemporaryDirectory() as tmp,self.assertRaises(RuntimeError):
                close_apps(Bridge(state),'session',tmp,['codium'])

    def test_unverified_observation_cannot_accept_exit(self):
        state=observation(False,False);state['identity_verified_at_end']=False
        with tempfile.TemporaryDirectory() as tmp,self.assertRaisesRegex(RuntimeError,'boot identity'):
            close_apps(Bridge(state),'session',tmp,['codium'],['3'])

    def test_timeout_preserves_remaining_tasks_and_boot_change_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(TimeoutError):
                close_apps(Bridge(observation(False,True)),'session',tmp,['codium'],['3'],deadline_s=0)
            with self.assertRaisesRegex(RuntimeError,'another boot'):
                close_apps(Bridge(observation(False,False,boot='different')),'session',tmp,['codium'],['3'])


if __name__=='__main__':unittest.main()
