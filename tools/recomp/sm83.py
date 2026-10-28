"""SM83 (Game Boy CPU) opcode table.

Each opcode is described once, as a C statement template built from the
helpers in runtime/cpu_ops.h.  Both the fallback interpreter and the static
recompiler are generated from this table, so they share identical semantics.

Template placeholders:
  {n8}   unsigned 8-bit immediate
  {n16}  16-bit immediate
  {e8}   signed 8-bit immediate (as a C int expression)
"""

from dataclasses import dataclass, field

R8 = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
R16 = ["bc", "de", "hl", "sp"]
R16_STK = ["bc", "de", "hl", "af"]
CONDS = ["nz", "z", "nc", "c"]
COND_C = {
    "nz": "!(R.f & FZ)",
    "z": "(R.f & FZ)",
    "nc": "!(R.f & FC)",
    "c": "(R.f & FC)",
}
ALU = ["add", "adc", "sub", "sbc", "and", "xor", "or", "cp"]
CB_ROT = ["rlc", "rrc", "rl", "rr", "sla", "sra", "swap", "srl"]

ILLEGAL = {0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD}


@dataclass
class Op:
    opcode: int
    length: int
    mnem: str          # disassembly format, using the same placeholders
    cycles: int        # T-cycles (branch not taken for conditionals)
    kind: str = "op"   # op|jp|jr|call|ret|reti|rst|jphl|halt|stop|ei|di|illegal
    code: str = ""     # C template (only for kind == "op")
    cond: str = None   # C condition for conditional flow ops
    cycles_taken: int = 0
    target: int = None  # fixed target (rst)
    writes_mem: bool = False  # may write memory (for bank-switch detection)
    sets_a: bool = False      # modifies A (for constant tracking)


def r_get(r):
    return "RD(HL)" if r == 6 else f"R.{R8[r]}"


def r_set(r, expr):
    return f"WR(HL, {expr});" if r == 6 else f"R.{R8[r]} = {expr};"


def r16_get(i):
    return "R.sp" if i == 3 else i_upper(R16[i])


def i_upper(name):
    return name.upper()


def r16_set(i, expr):
    return f"R.sp = (uint16_t)({expr});" if i == 3 else f"SET_{R16[i].upper()}({expr});"


def build_main():
    ops = {}

    def add(op):
        ops[op.opcode] = op

    for o in ILLEGAL:
        add(Op(o, 1, "illegal", 4, kind="illegal"))

    add(Op(0x00, 1, "nop", 4, code=""))
    add(Op(0x10, 2, "stop", 4, kind="stop"))
    add(Op(0x76, 1, "halt", 4, kind="halt"))
    add(Op(0xF3, 1, "di", 4, kind="di"))
    add(Op(0xFB, 1, "ei", 4, kind="ei"))

    for i in range(4):
        add(Op(0x01 + 16 * i, 3, f"ld {R16[i]},{{n16}}", 12, code=r16_set(i, "{n16}")))
        add(Op(0x03 + 16 * i, 1, f"inc {R16[i]}", 8, code=r16_set(i, f"{r16_get(i)} + 1")))
        add(Op(0x0B + 16 * i, 1, f"dec {R16[i]}", 8, code=r16_set(i, f"{r16_get(i)} - 1")))
        add(Op(0x09 + 16 * i, 1, f"add hl,{R16[i]}", 8, code=f"add_hl({r16_get(i)});"))
        add(Op(0xC1 + 16 * i, 1, f"pop {R16_STK[i]}", 12,
               code=("{ uint16_t v = pop16(); R.a = v >> 8; R.f = v & 0xF0; }" if i == 3
                     else f"SET_{R16_STK[i].upper()}(pop16());"),
               sets_a=(i == 3)))
        add(Op(0xC5 + 16 * i, 1, f"push {R16_STK[i]}", 16,
               code=("push16(AF);" if i == 3 else f"push16({R16_STK[i].upper()});"),
               writes_mem=True))

    add(Op(0x02, 1, "ld (bc),a", 8, code="WR(BC, R.a);", writes_mem=True))
    add(Op(0x12, 1, "ld (de),a", 8, code="WR(DE, R.a);", writes_mem=True))
    add(Op(0x22, 1, "ld (hl+),a", 8, code="WR(HL, R.a); SET_HL(HL + 1);", writes_mem=True))
    add(Op(0x32, 1, "ld (hl-),a", 8, code="WR(HL, R.a); SET_HL(HL - 1);", writes_mem=True))
    add(Op(0x0A, 1, "ld a,(bc)", 8, code="R.a = RD(BC);", sets_a=True))
    add(Op(0x1A, 1, "ld a,(de)", 8, code="R.a = RD(DE);", sets_a=True))
    add(Op(0x2A, 1, "ld a,(hl+)", 8, code="R.a = RD(HL); SET_HL(HL + 1);", sets_a=True))
    add(Op(0x3A, 1, "ld a,(hl-)", 8, code="R.a = RD(HL); SET_HL(HL - 1);", sets_a=True))

    for r in range(8):
        cyc = 12 if r == 6 else 4
        add(Op(0x04 + 8 * r, 1, f"inc {R8[r]}", cyc, code=r_set(r, f"inc8({r_get(r)})"),
               writes_mem=(r == 6), sets_a=(r == 7)))
        add(Op(0x05 + 8 * r, 1, f"dec {R8[r]}", cyc, code=r_set(r, f"dec8({r_get(r)})"),
               writes_mem=(r == 6), sets_a=(r == 7)))
        add(Op(0x06 + 8 * r, 2, f"ld {R8[r]},{{n8}}", 12 if r == 6 else 8,
               code=r_set(r, "{n8}"), writes_mem=(r == 6), sets_a=(r == 7)))

    add(Op(0x07, 1, "rlca", 4, code="R.a = rlc(R.a); R.f &= ~FZ;", sets_a=True))
    add(Op(0x0F, 1, "rrca", 4, code="R.a = rrc(R.a); R.f &= ~FZ;", sets_a=True))
    add(Op(0x17, 1, "rla", 4, code="R.a = rl(R.a); R.f &= ~FZ;", sets_a=True))
    add(Op(0x1F, 1, "rra", 4, code="R.a = rr(R.a); R.f &= ~FZ;", sets_a=True))
    add(Op(0x08, 3, "ld ({n16}),sp", 20,
           code="WR({n16}, R.sp & 0xFF); WR((uint16_t)({n16} + 1), R.sp >> 8);", writes_mem=True))
    add(Op(0x27, 1, "daa", 4, code="daa();", sets_a=True))
    add(Op(0x2F, 1, "cpl", 4, code="R.a = ~R.a; R.f |= FN | FH;", sets_a=True))
    add(Op(0x37, 1, "scf", 4, code="R.f = (R.f & FZ) | FC;"))
    add(Op(0x3F, 1, "ccf", 4, code="R.f = (R.f & FZ) | ((R.f & FC) ^ FC);"))

    add(Op(0x18, 2, "jr {e8}", 12, kind="jr"))
    for i, c in enumerate(CONDS):
        add(Op(0x20 + 8 * i, 2, f"jr {c},{{e8}}", 8, kind="jr", cond=COND_C[c], cycles_taken=12))
        add(Op(0xC0 + 8 * i, 1, f"ret {c}", 8, kind="ret", cond=COND_C[c], cycles_taken=20))
        add(Op(0xC2 + 8 * i, 3, f"jp {c},{{n16}}", 12, kind="jp", cond=COND_C[c], cycles_taken=16))
        add(Op(0xC4 + 8 * i, 3, f"call {c},{{n16}}", 12, kind="call", cond=COND_C[c],
               cycles_taken=24, writes_mem=True))

    for d in range(8):
        for s in range(8):
            o = 0x40 + 8 * d + s
            if o == 0x76:
                continue
            cyc = 8 if (d == 6 or s == 6) else 4
            add(Op(o, 1, f"ld {R8[d]},{R8[s]}", cyc, code=r_set(d, r_get(s)),
                   writes_mem=(d == 6), sets_a=(d == 7)))

    for k, name in enumerate(ALU):
        sets_a = name != "cp"
        for s in range(8):
            add(Op(0x80 + 8 * k + s, 1, f"{name} a,{R8[s]}", 8 if s == 6 else 4,
                   code=f"alu_{name}({r_get(s)});", sets_a=sets_a))
        add(Op(0xC6 + 8 * k, 2, f"{name} a,{{n8}}", 8, code=f"alu_{name}({{n8}});", sets_a=sets_a))

    add(Op(0xC3, 3, "jp {n16}", 16, kind="jp"))
    add(Op(0xCD, 3, "call {n16}", 24, kind="call", writes_mem=True))
    add(Op(0xC9, 1, "ret", 16, kind="ret"))
    add(Op(0xD9, 1, "reti", 16, kind="reti"))
    add(Op(0xE9, 1, "jp hl", 4, kind="jphl"))
    for k in range(8):
        add(Op(0xC7 + 8 * k, 1, f"rst {k * 8:02X}h", 16, kind="rst", target=k * 8, writes_mem=True))

    add(Op(0xE0, 2, "ldh ({n8}),a", 12, code="WR(0xFF00 | {n8}, R.a);", writes_mem=True))
    add(Op(0xF0, 2, "ldh a,({n8})", 12, code="R.a = RD(0xFF00 | {n8});", sets_a=True))
    add(Op(0xE2, 1, "ld (c),a", 8, code="WR(0xFF00 | R.c, R.a);", writes_mem=True))
    add(Op(0xF2, 1, "ld a,(c)", 8, code="R.a = RD(0xFF00 | R.c);", sets_a=True))
    add(Op(0xEA, 3, "ld ({n16}),a", 16, code="WR({n16}, R.a);", writes_mem=True))
    add(Op(0xFA, 3, "ld a,({n16})", 16, code="R.a = RD({n16});", sets_a=True))
    add(Op(0xE8, 2, "add sp,{e8}", 16, code="R.sp = sp_plus_e({e8});"))
    add(Op(0xF8, 2, "ld hl,sp{e8:+}", 12, code="SET_HL(sp_plus_e({e8}));"))
    add(Op(0xF9, 1, "ld sp,hl", 8, code="R.sp = HL;"))

    assert len(ops) == 256 - 1, len(ops)  # 0xCB is the prefix
    return ops


def build_cb():
    ops = {}
    for o in range(256):
        r = o & 7
        hl = r == 6
        grp = o >> 6
        y = (o >> 3) & 7
        if grp == 0:
            name = CB_ROT[y]
            code = r_set(r, f"{name}({r_get(r)})")
            mnem = f"{name} {R8[r]}"
            cyc = 16 if hl else 8
        elif grp == 1:
            code = f"bit_n({y}, {r_get(r)});"
            mnem = f"bit {y},{R8[r]}"
            cyc = 12 if hl else 8
        elif grp == 2:
            code = r_set(r, f"(uint8_t)({r_get(r)} & ~{1 << y:#04x})")
            mnem = f"res {y},{R8[r]}"
            cyc = 16 if hl else 8
        else:
            code = r_set(r, f"(uint8_t)({r_get(r)} | {1 << y:#04x})")
            mnem = f"set {y},{R8[r]}"
            cyc = 16 if hl else 8
        ops[o] = Op(0xCB00 | o, 2, mnem, cyc, code=code,
                    writes_mem=hl and grp != 1, sets_a=(r == 7 and grp != 1))
    return ops


MAIN = build_main()
CB = build_cb()


@dataclass
class Insn:
    addr: int          # CPU address
    op: Op
    n8: int = 0
    n16: int = 0
    raw: bytes = b""

    @property
    def length(self):
        return self.op.length

    @property
    def e8(self):
        return self.n8 - 256 if self.n8 >= 128 else self.n8

    @property
    def next(self):
        return (self.addr + self.op.length) & 0xFFFF

    @property
    def jump_target(self):
        k = self.op.kind
        if k == "jr":
            return (self.next + self.e8) & 0xFFFF
        if k in ("jp", "call"):
            return self.n16
        if k == "rst":
            return self.op.target
        return None

    def text(self):
        m = self.op.mnem
        if self.op.kind == "jr":
            return m.replace("{e8}", f"${self.jump_target:04X}")
        return m.format(n8=f"${self.n8:02X}", n16=f"${self.n16:04X}", e8=self.e8)


def decode(fetch, addr):
    """Decode the instruction at addr.  fetch(a) returns the byte at CPU address a."""
    b0 = fetch(addr)
    if b0 == 0xCB:
        b1 = fetch((addr + 1) & 0xFFFF)
        return Insn(addr, CB[b1], raw=bytes([b0, b1]))
    op = MAIN[b0]
    raw = [b0] + [fetch((addr + i) & 0xFFFF) for i in range(1, op.length)]
    n8 = raw[1] if op.length >= 2 else 0
    n16 = (raw[1] | (raw[2] << 8)) if op.length == 3 else 0
    return Insn(addr, op, n8=n8, n16=n16, raw=bytes(raw))
