#!/usr/bin/env python3
"""Export the installed MIT KEMAR data for reproducible offline filter design."""
import ctypes as c
import hashlib
import json
import math
from pathlib import Path

path = Path('/usr/share/libmysofa/MIT_KEMAR_normal_pinna.sofa')
lib = c.CDLL('libmysofa.so.1')
fp = c.POINTER(c.c_float)
lib.mysofa_open.argtypes = [c.c_char_p, c.c_float, c.POINTER(c.c_int), c.POINTER(c.c_int)]
lib.mysofa_open.restype = c.c_void_p
lib.mysofa_getfilter_float.argtypes = [c.c_void_p, c.c_float, c.c_float, c.c_float,
                                      fp, fp, fp, fp]
lib.mysofa_close.argtypes = [c.c_void_p]
n, error = c.c_int(), c.c_int()
handle = lib.mysofa_open(bytes(path), 48000, c.byref(n), c.byref(error))
if not handle or error.value or not 0 < n.value < 4096:
    raise RuntimeError(f'Cannot open KEMAR: {error.value}, length {n.value}')
result = {'sample_rate': 48000, 'source': str(path),
          'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'filters': {}}
try:
    for degrees in range(-180, 181, 1):
        a = math.radians(degrees)
        left, right = (c.c_float * n.value)(), (c.c_float * n.value)()
        dl, dr = c.c_float(), c.c_float()
        lib.mysofa_getfilter_float(handle, 1.4 * math.cos(a), 1.4 * math.sin(a), 0,
                                  left, right, c.byref(dl), c.byref(dr))
        result['filters'][str(degrees)] = {'left': list(left), 'right': list(right),
                                         'delay_left': dl.value, 'delay_right': dr.value}
finally:
    lib.mysofa_close(handle)
print(json.dumps(result, separators=(',', ':')))
