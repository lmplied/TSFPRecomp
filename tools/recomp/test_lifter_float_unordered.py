"""
Self-check: conditional jumps after a float compare when an operand is NaN.

Run: py -3 tools/recomp/test_lifter_float_unordered.py

comiss/ucomiss and fcomi/fucomi(p) set ZF, PF and CF together for an
unordered result, so `jb`, `jbe`, `je` and `jp` are taken on a NaN and `ja`,
`jae`, `jne` and `jnp` are not. The lifter spelled them as C's own <, <=,
==, != (and g_fp_cmp compared with 0), which agree with the hardware only for
ordered operands: on a NaN, `jbe` fell through and `jne` was taken.
TimeSplitters: Future Perfect hung in play-test 3 with a NaN ray in its
collision code.

Each condition _make_condition builds is evaluated, as Python, for ordered
and unordered operand pairs and compared with the branch the flags give.
"""

import math
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import _make_condition  # noqa: E402


class _Op:
    def __init__(self, reg):
        self.type = "reg"
        self.reg = reg
        self.size = 16
        self.mem_size = None
        self.imm = 0


# The branch each jcc takes, from (ZF, PF, CF): every unsigned and parity
# condition the lifter's COND_MAP knows.
TAKEN = {
    "ja": lambda z, p, c: not c and not z,
    "jae": lambda z, p, c: not c, "jnb": lambda z, p, c: not c,
    "jb": lambda z, p, c: c, "jnae": lambda z, p, c: c,
    "jbe": lambda z, p, c: c or z, "jna": lambda z, p, c: c or z,
    "je": lambda z, p, c: z, "jz": lambda z, p, c: z,
    "jne": lambda z, p, c: not z, "jnz": lambda z, p, c: not z,
    "jp": lambda z, p, c: p, "jnp": lambda z, p, c: not p,
}

CASES = [(1.0, 2.0), (2.0, 1.0), (2.0, 2.0), (math.nan, 1.0), (1.0, math.nan),
         (math.nan, math.nan), (-0.0, 0.0), (math.inf, math.inf)]


def flags(a, b):
    """(ZF, PF, CF) after comiss a, b / fcomi with st0 = a."""
    if a != a or b != b:
        return True, True, True
    return a == b, False, a < b


def fcmp(a, b):
    """RECOMP_FCMP (templates/runtime/recomp_types.h)."""
    if a != a or b != b:
        return 2
    return -1 if a < b else (1 if a > b else 0)


def as_python(expr):
    expr = re.sub(r"/\*.*?\*/", "", expr)
    expr = expr.replace("||", " or ").replace("&&", " and ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    return expr


def check(setter, ops):
    bad = []
    for jcc, want_of in TAKEN.items():
        r = _make_condition(jcc, setter, ops)
        assert r is not None, (setter, jcc)
        expr = as_python(r[0])
        for a, b in CASES:
            env = {"_fca": a, "_fcb": b, "g_fp_cmp": fcmp(a, b)}
            got = bool(eval(expr, env))
            want = bool(want_of(*flags(a, b)))
            if got != want:
                bad.append((jcc, a, b, got, want, r[0]))
    assert not bad, "\n".join(map(str, bad))


def test_sse_compares():
    for setter in ("comiss", "ucomiss", "comisd", "ucomisd"):
        check(setter, [_Op("xmm0"), _Op("xmm1")])


def test_fpu_compares():
    for setter in ("fcomi", "fcomip", "fucomi", "fucomip", "fcompi", "fucompi", "sahf"):
        check(setter, [])


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
