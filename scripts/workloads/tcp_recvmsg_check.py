#!/usr/bin/env python3
"""Linux/Brook TCP recvmsg framing, scatter, nonblocking and copyout control."""
import ctypes as c
import errno
import json
import socket
import threading
import time


def main():
    cases = {}
    observations = {}

    def pair():
        listener = socket.socket()
        sender = socket.socket()
        try:
            listener.settimeout(3)
            sender.settimeout(3)
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            sender.connect(listener.getsockname())
            receiver, _ = listener.accept()
            receiver.settimeout(3)
            return sender, receiver
        except BaseException:
            sender.close()
            raise
        finally:
            listener.close()

    def report():
        print('TCP_RECVMSG_RESULT', json.dumps(dict(passed=all(cases.values()), cases=cases, observations=observations)), flush=True)
        return int(not all(cases.values()))

    # This deliberately small binary prefix also fails promptly on the old
    # UDP-dispatch bug, without being decoded as an enormous datagram length.
    payload = b'\x01\0\0\0\0\0\0\0\0\0' + bytes(range(22))
    for mode in ('single', 'scatter'):
        sender, receiver = pair()
        try:
            sender.sendall(payload)
            if mode == 'single':
                data, ancillary, flags, _ = receiver.recvmsg(8)
            else:
                first, second = bytearray(3), bytearray(5)
                count, ancillary, flags, _ = receiver.recvmsg_into([bytearray(), first, second])
                data = (bytes(first) + bytes(second))[:count]
            cases[mode + '_prefix'] = data == payload[:8] and not ancillary and flags == 0
            if not cases[mode + '_prefix']:
                return report()
            rest = bytearray()
            while len(rest) < len(payload) - len(data):
                chunk, _, _, _ = receiver.recvmsg(len(payload) - len(data) - len(rest))
                if not chunk:
                    break
                rest.extend(chunk)
            cases[mode + '_remainder'] = data + rest == payload
            receiver.setblocking(True)
            started = time.monotonic()
            try:
                receiver.recvmsg(8, 0, socket.MSG_DONTWAIT)
                cases[mode + '_dontwait'] = False
            except BlockingIOError as error:
                cases[mode + '_dontwait'] = error.errno == errno.EAGAIN and time.monotonic() - started < 1
            receiver.settimeout(3)
            sender.close()
            cases[mode + '_eof'] = receiver.recvmsg(8)[0] == b''
        finally:
            sender.close()
            receiver.close()

    class Iovec(c.Structure):
        _fields_ = [('base', c.c_void_p), ('length', c.c_size_t)]

    class Message(c.Structure):
        _fields_ = [('name', c.c_void_p), ('name_length', c.c_uint),
                    ('vectors', c.POINTER(Iovec)), ('count', c.c_size_t),
                    ('control', c.c_void_p), ('control_length', c.c_size_t), ('flags', c.c_int)]

    libc = c.CDLL(None, use_errno=True)
    raw = libc.recvmsg
    raw.restype = c.c_ssize_t
    raw.argtypes = [c.c_int, c.POINTER(Message), c.c_int]
    sender, receiver = pair()
    try:
        sender.sendall(payload)
        receiver.setblocking(True)
        empty = Message()
        cases['zero_vectors'] = raw(receiver.fileno(), c.byref(empty), socket.MSG_DONTWAIT) == 0
        bad = Iovec(1, 4)
        message = Message(vectors=c.pointer(bad), count=1)
        c.set_errno(0)
        cases['bad_buffer_efault'] = raw(receiver.fileno(), c.byref(message), socket.MSG_DONTWAIT) == -1 and c.get_errno() == errno.EFAULT
        receiver.settimeout(3)
        cases['bad_buffer_preserves_stream'] = receiver.recvmsg(len(payload))[0] == payload
    finally:
        sender.close()
        receiver.close()

    sender, receiver = pair()
    try:
        data = bytes(range(256)) * 32
        sender.sendall(data)
        result = bytearray()
        while len(result) < len(data):
            a, b = bytearray(511), bytearray(1601)
            count, _, flags, _ = receiver.recvmsg_into([a, b])
            if not count or flags:
                break
            result.extend((a + b)[:count])
        cases['multisegment_scatter'] = result == data
    finally:
        sender.close()
        receiver.close()

    # Two real blocking receives share one stream. Losing readiness to the
    # other reader must resume waiting, rather than report an early timeout.
    competing = True
    for _ in range(8):
        sender, receiver = pair()
        receiver.setblocking(True)
        barrier = threading.Barrier(3)
        results = []

        def read_one():
            buffer = c.create_string_buffer(1)
            vector = Iovec(c.cast(buffer, c.c_void_p), 1)
            message = Message(vectors=c.pointer(vector), count=1)
            barrier.wait(timeout=3)
            result = raw(receiver.fileno(), c.byref(message), 0)
            results.append((result, buffer.raw))

        readers = [threading.Thread(target=read_one) for _ in range(2)]
        try:
            for thread in readers:
                thread.start()
            barrier.wait(timeout=3)
            sender.sendall(b'A')
            time.sleep(.15)
            sender.sendall(b'B')
            for thread in readers:
                thread.join(timeout=3)
            competing &= sorted(results) == [(1, b'A'), (1, b'B')]
        finally:
            sender.close()
            for thread in readers:
                thread.join(timeout=3)
            receiver.close()
    cases['competing_blocking_readers'] = competing

    sender, receiver = pair()
    data = bytes((i * 17 + (i >> 8) * 31 + (i >> 16) * 97) & 255 for i in range(131072))
    send_errors = []

    def send_large():
        try:
            sender.sendall(data)
        except OSError as error:
            send_errors.append(error.errno)
        finally:
            sender.close()

    writer = threading.Thread(target=send_large)
    try:
        writer.start()
        result = bytearray()
        counts = []
        while len(result) < len(data):
            a, b = bytearray(65536), bytearray(65536)
            count, _, flags, _ = receiver.recvmsg_into([a, b])
            if not count or flags:
                break
            counts.append(count)
            result.extend((a + b)[:count])
        writer.join(timeout=3)
        cases['large_receive_followed_by_complete_drain'] = result == data and not send_errors and not writer.is_alive()
        cases['large_receive_counts_valid'] = bool(counts) and all(0 < n <= 131072 for n in counts)
        observations['largest_receive'] = max(counts, default=0)
    finally:
        receiver.close()
        sender.close()
        writer.join(timeout=3)

    # A stream fix must leave the datagram receiver's framing intact.
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        receiver.settimeout(3)
        receiver.bind(('127.0.0.1', 0))
        sender.sendto(b'udp-one', receiver.getsockname())
        sender.sendto(b'udp-two', receiver.getsockname())
        cases['udp_first_record'] = receiver.recvmsg(32)[0] == b'udp-one'
        cases['udp_second_record'] = receiver.recvmsg(32)[0] == b'udp-two'
    finally:
        sender.close()
        receiver.close()
    return report()


if __name__ == '__main__':
    raise SystemExit(main())
