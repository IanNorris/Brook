import threading
import time
import unittest

from presence import Presence, PresenceGone


class Clock:
    def __init__(self):self.now=1000.0
    def __call__(self):return self.now


class PresenceTests(unittest.TestCase):
    def setUp(self):
        self.clock=Clock();self.sent=[];self.replies={}
        self.presence=Presence(self.send,interval=10,idle_detach=1800,clock=self.clock)
    def send(self,kind,session,command):
        self.sent.append((kind,session,command))
        reply=self.replies.get(session,{'presence_api':1,'state':'connected'})
        if isinstance(reply,Exception):raise reply
        return reply

    def test_touched_session_beats_with_interval(self):
        self.presence.touch('hw','a'*32);self.presence.touch('xx','b'*32);self.presence.touch('vm',None)
        self.assertEqual(self.presence.tick(),{'hw:'+'a'*32:'beating'})
        self.assertEqual(self.sent,[('hw','a'*32,'bridge presence beat 10')])
        self.assertEqual(self.presence.entry('hw','a'*32)['state'],'beating')

    def test_transport_loss_is_retried_and_recovers(self):
        self.presence.touch('vm','a'*32);self.replies['a'*32]=ConnectionRefusedError()
        self.assertEqual(self.presence.tick(),{'vm:'+'a'*32:'unreachable'})
        self.assertEqual(self.presence.tick(),{'vm:'+'a'*32:'unreachable'})
        self.assertEqual(self.presence.entry('vm','a'*32)['failures'],2)
        self.replies['a'*32]=TimeoutError()
        self.assertEqual(self.presence.tick(),{'vm:'+'a'*32:'unreachable'})
        del self.replies['a'*32]
        self.assertEqual(self.presence.tick(),{'vm:'+'a'*32:'beating'})
        self.assertEqual(self.presence.entry('vm','a'*32)['failures'],0)

    def test_gone_and_unsupported_sessions_are_dropped(self):
        self.presence.touch('hw','a'*32);self.presence.touch('hw','b'*32)
        self.replies['a'*32]=PresenceGone('boot changed');self.replies['b'*32]={'reply':'unknown command'}
        self.assertEqual(self.presence.tick(),{'hw:'+'a'*32:'gone','hw:'+'b'*32:'unsupported'})
        self.assertEqual(self.presence.tick(),{})
        self.assertEqual(len(self.sent),2)

    def test_idle_session_detaches_once(self):
        self.presence.touch('vm','a'*32);self.presence.tick()
        self.clock.now+=1799;self.presence.touch('vm','a'*32);self.clock.now+=1799
        self.assertEqual(self.presence.tick(),{'vm:'+'a'*32:'beating'})
        self.clock.now+=1
        self.assertEqual(self.presence.tick(),{'vm:'+'a'*32:'idle_detached'})
        self.assertEqual(self.sent[-1],('vm','a'*32,'bridge presence detach'))
        self.assertEqual(self.presence.tick(),{})

    def test_close_detaches_every_target_best_effort(self):
        self.presence.touch('vm','a'*32);self.presence.touch('hw','b'*32)
        self.replies['b'*32]=OSError('down')
        self.presence.close()
        self.assertEqual(sorted(c for c in self.sent if c[2]=='bridge presence detach'),
                         [('hw','b'*32,'bridge presence detach'),('vm','a'*32,'bridge presence detach')])
        self.assertEqual(self.presence.status(),{})

    def test_forget_waits_for_an_in_flight_beat(self):
        started=threading.Event();release=threading.Event();order=[]
        def slow(kind,session,command):
            started.set();release.wait(2);order.append('beat');return {'presence_api':1}
        presence=Presence(slow,interval=10,clock=self.clock);presence.touch('hw','a'*32)
        worker=threading.Thread(target=presence.tick);worker.start();started.wait(2)
        forgetter=threading.Thread(target=lambda:(presence.forget('hw','a'*32),order.append('forgotten')))
        forgetter.start();time.sleep(.05);self.assertEqual(order,[])
        release.set();worker.join(2);forgetter.join(2)
        self.assertEqual(order,['beat','forgotten'])
        self.assertEqual(presence.tick(),{})

    def test_background_thread_beats_new_session_immediately(self):
        beats=threading.Event()
        def send(kind,session,command):
            if 'beat' in command:beats.set()
            return {'presence_api':1}
        presence=Presence(send,interval=60).start()
        try:
            presence.touch('vm','a'*32)
            self.assertTrue(beats.wait(2))
        finally:presence.close()


if __name__=='__main__':
    unittest.main()
