#!/usr/bin/env python3
"""Read Linux x86-64 clock state without changing time or discipline settings."""
import ctypes
import json
from pathlib import Path
import platform
import time


def snapshot():
    values={}
    for name in ('CLOCK_MONOTONIC','CLOCK_MONOTONIC_RAW','CLOCK_BOOTTIME','CLOCK_REALTIME'):
        try:values[name]=time.clock_gettime_ns(getattr(time,name))
        except (AttributeError,OSError) as error:values[name]=dict(error=str(error))
    source=Path('/sys/devices/system/clocksource/clocksource0/current_clocksource')
    if source.exists():values['clocksource']=source.read_text().strip()
    if platform.system()!='Linux' or platform.machine()!='x86_64':
        values['discipline']=dict(error='adjtimex layout is only verified for Linux x86-64')
        return values
    class Timeval(ctypes.Structure):
        _fields_=[('seconds',ctypes.c_long),('microseconds',ctypes.c_long)]
    class Timex(ctypes.Structure):
        _fields_=[('modes',ctypes.c_uint),('offset',ctypes.c_long),('freq',ctypes.c_long),
                  ('maxerror',ctypes.c_long),('esterror',ctypes.c_long),('status',ctypes.c_int),
                  ('constant',ctypes.c_long),('precision',ctypes.c_long),('tolerance',ctypes.c_long),
                  ('time',Timeval),('tick',ctypes.c_long),('ppsfreq',ctypes.c_long),
                  ('jitter',ctypes.c_long),('shift',ctypes.c_int),('stabil',ctypes.c_long),
                  ('jitcnt',ctypes.c_long),('calcnt',ctypes.c_long),('errcnt',ctypes.c_long),
                  ('stbcnt',ctypes.c_long),('tai',ctypes.c_int),('reserved',ctypes.c_int*11)]
    assert ctypes.sizeof(Timex)==208
    state=Timex() # modes zero is query-only; never pass a time adjustment.
    libc=ctypes.CDLL(None,use_errno=True);call=libc.adjtimex
    call.argtypes=[ctypes.POINTER(Timex)];call.restype=ctypes.c_int
    result=call(ctypes.byref(state))
    values['discipline']=dict(return_state=result,errno=ctypes.get_errno() if result<0 else 0,
        modes=state.modes,offset=state.offset,freq_scaled_ppm=state.freq,freq_ppm=state.freq/65536,
        tick_us=state.tick,status=state.status,tolerance_scaled_ppm=state.tolerance,
        maxerror=state.maxerror,esterror=state.esterror,tai=state.tai)
    return values


if __name__=='__main__':print(json.dumps(snapshot()))
