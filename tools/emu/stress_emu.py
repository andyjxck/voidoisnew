#!/usr/bin/env python3
"""Peak-RAM run for VoidOS in the boot emulator (unicorn).

Boots the real firmware image exactly like boot_emu.py, with every byte of
free RAM (from `end` up to the stack top) painted with 0xA5 first. Once
the main loop runs it drives the busiest paths through the real code, from
the main loop as the NimBLE host would:

  1. the burst of notifications iOS sends on connect: 8 Notification
     Source adds plus full Data Source replies (every field at the length
     the watch asks for, curly quotes and accents) -> the store, the
     pop-up and the alerts detail view render
  2. the alerts list and detail pages
  3. music: a long title that scrolls, on the music screen and the face

Then it reads back how far the stack reached and where the sbrk heap
ends, and reports the gap between them.

The host's receive path (HCI -> L2CAP -> ATT -> GAP event -> ancs) is
skipped by calling ancs_on_notify directly, so that call runs with SP
lowered by RX_PATH bytes, the summed frame sizes of that path from
-fstack-usage. Interrupts do not run here; their worst case is added
separately (see ISR_RESERVE).

usage: stress_emu.py voidos-app.out voidos-app.bin [rx_path_bytes] [isr_bytes]
"""
import bisect, struct, subprocess, sys
from unicorn import *
from unicorn.arm_const import *

elf, binf = sys.argv[1], sys.argv[2]
RX_PATH = int(sys.argv[3]) if len(sys.argv) > 3 else 0
ISR_RESERVE = int(sys.argv[4]) if len(sys.argv) > 4 else 0

syms = {}
for line in subprocess.run(['arm-none-eabi-nm', elf], capture_output=True,
                           text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
rev = sorted((v & ~1, k) for k, v in syms.items())
def sym(a):
    i = bisect.bisect_right(rev, (a, 'zzzz')) - 1
    return f"{rev[i][1]}+{a-rev[i][0]:#x}" if i >= 0 else hex(a)

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x0, 0x80000)
uc.mem_map(0x10000000, 0x2000)
uc.mem_map(0x20000000, 0x10000)
uc.mem_map(0x30000000, 0x10000)          # scratch: test data, not watch RAM
uc.mem_map(0x40000000, 0x40000)
uc.mem_map(0x50000000, 0x1000)
uc.mem_map(0xE0000000, 0x100000)
data = open(binf, 'rb').read()
uc.mem_write(0x8020, data)
uc.mem_write(0x10000000, b'\x00' * 0x2000)
uc.mem_write(0x7BFE8, b'\xff' * 0x18)
MAGIC = 0x7F000                           # return address for injected calls
uc.mem_write(MAGIC, b'\x00\xbf' * 8)      # nops

RAM_END = 0x20010000
END = syms['end']
PAINT = 0xA5
uc.mem_write(END, bytes([PAINT]) * (RAM_END - 256 - END))

state = {'ticks': 0, 'wdt_last': 0, 'wdt_max': 0, 'reset': None}
TICK_PER_READ = 2
OVERRIDE_ONE = {
    0x40003118, 0x40003110, 0x40003120, 0x40003104, 0x40004104,
    0x40004148, 0x4000414C, 0x40004150, 0x40004160, 0x40004164,
    0x40007100, 0x40007104, 0x4000710C, 0x40007110, 0x40007114, 0x40007118,
    0x4000E400, 0x4001E400, 0x4000D100, 0x40000100, 0x40000104,
    0x4000E100, 0x4000E104, 0x4000F100, 0x4000F104, 0x4000F108,
}
def on_read(uc, access, addr, size, value, ud):
    if addr in (0x40011504, 0x4000B504):
        state['ticks'] += TICK_PER_READ
        uc.mem_write(addr, struct.pack('<I', state['ticks'] & 0xFFFFFF))
    elif addr in OVERRIDE_ONE:
        uc.mem_write(addr, struct.pack('<I', 1))
    elif addr == 0x40004124:
        uc.mem_write(addr, b'\0\0\0\0')
    elif addr in (0x40000418, 0x4000040C):
        uc.mem_write(addr, struct.pack('<I', 1 << 16 | 1))
    elif addr == 0x4000D508:
        state['rng'] = (state.get('rng', 7) * 1103515245 + 12345) & 0xff
        uc.mem_write(addr, struct.pack('<I', state['rng']))
def on_write(uc, access, addr, size, value, ud):
    t = state['ticks']
    if addr == 0x40010600:
        state['wdt_max'] = max(state['wdt_max'], t - state['wdt_last'])
        state['wdt_last'] = t
    elif addr == 0xE000ED0C and (value & 0xFFFF0004) == 0x05FA0004:
        state['reset'] = 'SCB_AIRCR system reset'
        uc.emu_stop()
uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=0x40000000, end=0x4003FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0x40000000, end=0x4003FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0xE000ED0C, end=0xE000ED0F)

faults = {syms[n] & ~1: n for n in ('fault_blink', 'HardFault_Handler',
                                    'Default_Handler') if n in syms}
def on_fault(uc, addr, size, ud):
    state['reset'] = f"{faults[addr]} (lr {sym(uc.reg_read(UC_ARM_REG_LR))})"
    uc.emu_stop()
for a in faults:
    uc.hook_add(UC_HOOK_CODE, on_fault, begin=a, end=a + 1)

UI_TICK = syms['ui_tick'] & ~1
state['want_stop'] = False
def on_ui_tick(uc, addr, size, ud):
    if state['want_stop']:
        state['want_stop'] = False
        uc.emu_stop()
uc.hook_add(UC_HOOK_CODE, on_ui_tick, begin=UI_TICK, end=UI_TICK + 1)

def now_ms():
    return state['ticks'] * 1000 // 32768

def run_to(ms_target):
    while not state['reset'] and now_ms() < ms_target:
        try:
            uc.emu_start(uc.reg_read(UC_ARM_REG_PC) | 1, 0xFFFFFFFF,
                         count=2_000_000)
        except UcError as e:
            state['reset'] = f"EMU ERROR {e} at {sym(uc.reg_read(UC_ARM_REG_PC))}"
    return not state['reset']

def stop_at_ui_tick():
    """run on until the main loop is about to call ui_tick"""
    state['want_stop'] = True
    while not state['reset']:
        try:
            uc.emu_start(uc.reg_read(UC_ARM_REG_PC) | 1, 0xFFFFFFFF,
                         count=2_000_000)
        except UcError as e:
            state['reset'] = f"EMU ERROR {e}"
        if uc.reg_read(UC_ARM_REG_PC) == UI_TICK and not state['want_stop']:
            return True
    return False

def call(name, *args, sp_drop=0):
    """call a firmware function from the main loop (we are stopped at
    ui_tick's entry, a call boundary), with SP lowered by sp_drop"""
    sp, lr, pc = (uc.reg_read(r) for r in
                  (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC))
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                     UC_ARM_REG_R3), args):
        uc.reg_write(r, v & 0xFFFFFFFF)
    nsp = (sp - sp_drop - 4 * len(args[4:])) & ~7
    for i, v in enumerate(args[4:]):          # AAPCS: 5th arg on up
        uc.mem_write(nsp + 4 * i, struct.pack('<I', v & 0xFFFFFFFF))
    uc.reg_write(UC_ARM_REG_SP, nsp)
    uc.reg_write(UC_ARM_REG_LR, MAGIC | 1)
    try:
        uc.emu_start(syms[name] | 1, MAGIC, count=50_000_000)
    except UcError as e:
        state['reset'] = f"EMU ERROR in {name}: {e} at {sym(uc.reg_read(UC_ARM_REG_PC))}"
    if uc.reg_read(UC_ARM_REG_PC) != MAGIC and not state['reset']:
        state['reset'] = f"{name} did not return (pc {sym(uc.reg_read(UC_ARM_REG_PC))})"
    r0 = uc.reg_read(UC_ARM_REG_R0)
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, lr)
    uc.reg_write(UC_ARM_REG_PC, pc)
    return r0

# ---- scratch allocator -------------------------------------------------
scr = {'p': 0x30000000}
def put(b):
    a = scr['p']
    uc.mem_write(a, bytes(b))
    scr['p'] = (a + len(b) + 7) & ~7
    return a
def cstr(s):
    return put(s.encode() + b'\0')

def mbuf(payload):
    """a one-segment os_mbuf with a packet header, as NimBLE hands it"""
    d = put(payload)
    om = struct.pack('<IBBHII', d, 0, 8, len(payload), 0, 0)
    pk = struct.pack('<HHI', len(payload), 0, 0)
    return put(om + pk)

H_NS, H_DS = 0x30, 0x33
def ancs_notify(handle, payload):
    return call('ancs_on_notify', handle, mbuf(payload), sp_drop=RX_PATH)

def ns_add(uid, cat=4):
    return bytes([0, 0, cat, 1]) + struct.pack('<I', uid)

def ds_reply(uid, app, title, sub, msg, date):
    b = bytes([0]) + struct.pack('<I', uid)
    for i, v in ((0, app), (1, title), (2, sub), (3, msg), (5, date)):
        e = v.encode()
        b += bytes([i]) + struct.pack('<H', len(e)) + e
    return b

W = 'W' * 2
TITLE = 'Alexandra Montgomery-Wellington \u2019s group chat \u00e9t\u00e9 '   # long, accents
SUB = 'Re: the plan for Saturday \u201cmorning\u201d session'
MSG = ('\u201cHey! Are we still on for the 7am session tomorrow? I can '
       'bring the bands and the foam roller \u2014 also caf\u00e9 after?\u201d '
       'Let me know before 10 tonight so I can sort the car.')
APPS = ['com.apple.MobileSMS', 'net.whatsapp.WhatsApp', 'com.sesame.app.stable',
        'com.burbn.instagram', 'com.google.Gmail', 'ph.telegra.Telegraph',
        'com.apple.mobilecal', 'com.example.someverylongbundle.identifier.beta']

log = []
def step(s):
    log.append(f"  t={now_ms():6d} ms  {s}")
    print(log[-1], flush=True)

# ---- run ---------------------------------------------------------------
sp0 = struct.unpack('<I', data[:4])[0]
uc.reg_write(UC_ARM_REG_SP, sp0)
uc.reg_write(UC_ARM_REG_PC, struct.unpack('<I', data[4:8])[0] & ~1)
ok = run_to(5000)
step(f"booted to the main loop: {'ok' if ok else state['reset']}")

if ok and stop_at_ui_tick():
    uc.mem_write(syms['h_ns_val'], struct.pack('<H', H_NS))
    uc.mem_write(syms['h_ds_val'], struct.pack('<H', H_DS))
    call('ui_wake')
    # 1. connect burst: 8 adds (store holds 6), then every reply
    for i in range(8):
        ancs_notify(H_NS, ns_add(1000 + i))
    for i in range(8):
        rep = ds_reply(1000 + i, APPS[i], TITLE, SUB, MSG, '20261029T054500')
        # the phone sends Data Source replies in MTU-sized pieces
        for o in range(0, len(rep), 182):
            ancs_notify(H_DS, rep[o:o + 182])
    n = call('ancs_count')
    step(f"notification burst fed: store holds {n}")
    ok = run_to(now_ms() + 1500)          # pop-up -> detail view renders
    step(f"pop-up/detail on screen: cur={uc.mem_read(syms['cur'], 1)[0]} "
         f"{'ok' if ok else state['reset']}")

if ok and stop_at_ui_tick():
    # 2. later detail pages, then the list
    for pg in (1, 2):
        uc.mem_write(syms['notif_dpage'], struct.pack('<i', pg))
        call('ui_wake')
        call('rebuild')
        stop_at_ui_tick() and run_to(now_ms() + 600)
        stop_at_ui_tick()
    uc.mem_write(syms['notif_dpage'], struct.pack('<i', 0))
    call('notif_close')
    call('ui_wake')
    call('nav_to', 13)                    # SCR_NOTIF: the list
    ok = run_to(now_ms() + 1000)
    step(f"alerts detail pages + list rendered: {'ok' if ok else state['reset']}")

if ok and stop_at_ui_tick():
    # 3. music, phone linked (ble_connected patched to return 1)
    bc = syms['ble_connected'] & ~1
    orig = bytes(uc.mem_read(bc, 4))
    uc.mem_write(bc, b'\x01\x20\x70\x47')   # movs r0,#1 ; bx lr
    call('phone_music_ams', 1, cstr('The Extremely Long Artist Name Band'),
         cstr('An Absurdly Long Song Title That Has To Scroll Across'),
         cstr('Album'), 42, 245)
    call('ui_wake')
    call('nav_to', 16)                    # SCR_MUSIC
    ok = run_to(now_ms() + 3000)          # title scrolls
    step(f"music screen, scrolling title: {'ok' if ok else state['reset']}")
    if ok and stop_at_ui_tick():
        call('ui_wake')
        call('nav_to', 0)                 # face with the music chip
        ok = run_to(now_ms() + 2000)
        step(f"face with music chip: {'ok' if ok else state['reset']}")
    uc.mem_write(bc, orig)

if ok:
    ok = run_to(max(now_ms() + 2000, 14000))
    step(f"kept running: {'ok' if ok else state['reset']}")

# ---- read back ------------------------------------------------------------
ram = bytes(uc.mem_read(END, RAM_END - END))
brk = struct.unpack('<I', uc.mem_read(syms['brk.0'], 4))[0] or END
low = None
for off in range(brk - END, len(ram) - 256, 4):
    if ram[off:off + 4] != bytes([PAINT]) * 4:
        low = END + off
        break
stack_peak = RAM_END - low if low else 0
print()
print(f"end t={now_ms()} ms reset={state['reset']} max WDT gap={state['wdt_max']*1000//32768} ms")
print(f"free RAM after .data/.bss: {RAM_END - END} B (end {END:#x})")
print(f"sbrk heap in use: {brk - END} B (cap 4096)")
print(f"stack peak: {stack_peak} B (lowest touched {low:#x}), includes"
      f" {RX_PATH} B for the NimBLE receive path" if low else "stack: untouched?")
if low:
    gap = low - brk
    print(f"headroom heap top -> stack low-water: {gap} B")
    print(f"after the {ISR_RESERVE} B interrupt reserve: {gap - ISR_RESERVE} B")
