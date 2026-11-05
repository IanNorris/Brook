#!/usr/bin/env python3
"""Exercise the actual local network diagnostic against a loopback fixture."""
import socket
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]


class DiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory()
        cls.binary=Path(cls.directory.name)/'netcheck'
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        str(ROOT/'tools/hardware_network_check.c'),'-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):cls.directory.cleanup()

    def run_fixture(self,complete,fragmented=False):
        with socket.socket() as server:
            server.bind(('127.0.0.1',0));server.listen();port=server.getsockname()[1]
            seen=[]
            def serve():
                connection,_=server.accept()
                with connection:
                    command=connection.recv(1024);seen.append(command)
                    token=command.split(b' ',1)[0][1:]
                    reply=b'BRIDGE_BEGIN '+token+b'\n{}\n'
                    if complete:reply+=b'BRIDGE_END '+token+b'\n'
                    if fragmented:
                        for byte in reply:connection.sendall(bytes([byte]))
                    else:connection.sendall(reply)
            thread=threading.Thread(target=serve);thread.start()
            result=subprocess.run([str(self.binary),'--port',str(port)],capture_output=True,text=True,timeout=10)
            thread.join(5);self.assertFalse(thread.is_alive())
            self.assertEqual(len(seen),1)
            self.assertRegex(seen[0],rb'^@[0-9]+ bridge hardware caps\n$')
            return result

    def test_complete_framed_reply(self):
        result=self.run_fixture(True,fragmented=True)
        self.assertEqual(result.returncode,0,result.stdout)
        self.assertIn('framed reply received',result.stdout)

    def test_incomplete_reply_is_failure(self):
        result=self.run_fixture(False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('no complete framed reply',result.stdout)

    def test_invalid_arguments(self):
        self.assertEqual(subprocess.run([str(self.binary),'--port','0'],capture_output=True).returncode,2)


if __name__=='__main__':unittest.main()
