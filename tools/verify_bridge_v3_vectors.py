"""Check the bridge primitives with pinned C2SP Wycheproof vector files.

Run from the pinned development shell. Pass the CMake bridge_v3_interop shared
library and the directory holding testvectors_v1 from
https://github.com/C2SP/wycheproof/tree/65ca494727a52944c204441c860b039e6786b99b/testvectors_v1
"""
import argparse
import ctypes as c
import json
from pathlib import Path

from nacl.bindings import crypto_aead_chacha20poly1305_ietf_decrypt, crypto_scalarmult
from nacl.exceptions import CryptoError
from nacl.signing import VerifyKey


class AeadContext(c.Structure):
    _fields_ = [('counter', c.c_uint64), ('key', c.c_ubyte * 32), ('nonce', c.c_ubyte * 8)]


def vectors(directory, primitive):
    document = json.loads((directory / (primitive + '_test.json')).read_bytes())
    for group in document['testGroups']:
        for test in group['tests']:
            yield group, test


def ed25519(library, group, test):
    signature = bytes.fromhex(test['sig'])
    message = bytes.fromhex(test['msg'])
    key = bytes.fromhex(group['publicKey']['pk'])
    native = len(signature) == 64 and not library.crypto_ed25519_check_strict(signature, key, message, len(message))
    try:
        VerifyKey(key).verify(message, signature)
        host = True
    except (ValueError, CryptoError):
        host = False
    if test['result'] == 'acceptable':
        return
    expected = test['result'] == 'valid'
    assert expected == native == host, (test['tcId'], expected, native, host)


def x25519(library, _group, test):
    private = bytes.fromhex(test['private'])
    public = bytes.fromhex(test['public'])
    expected = bytes.fromhex(test['shared'])
    assert len(private) == len(public) == len(expected) == 32
    output = c.create_string_buffer(32)
    library.crypto_x25519(output, private, public)
    assert output.raw == expected, test['tcId']
    if any(expected):
        assert crypto_scalarmult(private, public) == expected, test['tcId']
    else:
        try:
            crypto_scalarmult(private, public)
        except CryptoError:
            return
        raise AssertionError(('host accepted all-zero X25519', test['tcId']))


def chacha20_poly1305(library, _group, test):
    key, iv, aad, expected, cipher, tag = (
        bytes.fromhex(test[field]) for field in ('key', 'iv', 'aad', 'msg', 'ct', 'tag')
    )
    native = False
    host = False
    if len(iv) == 12 and len(tag) == 16:
        context = AeadContext()
        library.crypto_aead_init_ietf(c.byref(context), key, iv)
        output = c.create_string_buffer(len(cipher))
        native = not library.crypto_aead_read(c.byref(context), output, tag, aad, len(aad), cipher, len(cipher))
        if native:
            assert output.raw == expected, test['tcId']
        try:
            plain = crypto_aead_chacha20poly1305_ietf_decrypt(cipher + tag, aad, iv, key)
            host = True
        except CryptoError:
            pass
        else:
            assert plain == expected, test['tcId']
    valid = test['result'] == 'valid'
    assert valid == native == host, (test['tcId'], valid, native, host)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('library', type=Path)
    parser.add_argument('vectors', type=Path)
    args = parser.parse_args()
    library = c.CDLL(str(args.library.resolve()))
    library.crypto_ed25519_check_strict.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_size_t]
    library.crypto_ed25519_check_strict.restype = c.c_int
    library.crypto_x25519.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p]
    library.crypto_x25519.restype = None
    library.crypto_aead_init_ietf.argtypes = [c.POINTER(AeadContext), c.c_char_p, c.c_char_p]
    library.crypto_aead_init_ietf.restype = None
    library.crypto_aead_read.argtypes = [c.POINTER(AeadContext), c.c_void_p, c.c_char_p,
                                         c.c_char_p, c.c_size_t, c.c_char_p, c.c_size_t]
    library.crypto_aead_read.restype = c.c_int
    for check in (ed25519, x25519, chacha20_poly1305):
        count = 0
        for group, test in vectors(args.vectors, check.__name__):
            check(library, group, test)
            count += 1
        print(f'{check.__name__} {count} cases passed')


if __name__ == '__main__':
    main()
