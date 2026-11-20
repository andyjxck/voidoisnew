#!/usr/bin/env python3
"""Boot VoidOS in the emulator with a working external SPI NOR flash (2.4.1).

Like boot_emu.py, but SPIM0 transfers with the flash's CS low go to a 4 MB
NOR model (read, page program, sector/block erase, status, JEDEC id), so
the settings record, the bonds, the history log and the DFU slot behave
as on the watch. The NOR image can be loaded from a file and written back,
so one firmware can leave its saved data behind and another boot on it.

It reports every fault, an SCB reset, watchdog starvation, whether the
image confirmed itself (hal_boot_confirm, the MCUBoot image_ok word at
0x7BFE8) and how deep the stack went.

usage: boot_nor.py app.out app.bin ms [nor_in|-] [nor_out|-] [--quiet]
exit:  0 = confirmed and still running at the end, 1 = anything else
"""
import bisect, struct, subprocess, sys
from unicorn import *
from unicorn.arm_const import *

args = [a for a in sys.argv[1:] if not a.startswith('--')]
quiet = '--quiet' in sys.argv
elf, binf = args[0], args[1]
max_ms = int(args[2]) if len(args) > 2 else 6000
nor_in = args[3] if len(args) > 3 and args[3] != '-' else None
nor_out = args[4] if len(args) > 4 and args[4] != '-' else None

syms = {}
nm_out = '' if elf == 'none' else subprocess.run(
    ['arm-none-eabi-nm', elf], capture_output=True, text=True).stdout
for line in nm_out.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
rev = sorted((v & ~1, k) for k, v in syms.items())
def sym(a):
    if not rev:
        return hex(a)
    i = bisect.bisect_right(rev, (a, 'zzzz')) - 1
    return f"{rev[i][1]}+{a-rev[i][0]:#x}" if i >= 0 else hex(a)

NOR_SIZE = 4 * 1024 * 1024
nor = bytearray(open(nor_in, 'rb').read()) if nor_in else bytearray(b'\xff' * NOR_SIZE)
assert len(nor) == NOR_SIZE

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x0, 0x80000)
uc.mem_map(0x10000000, 0x2000)
uc.mem_map(0x20000000, 0x10000)
uc.mem_map(0x40000000, 0x40000)
uc.mem_map(0x50000000, 0x1000)
uc.mem_map(0xE0000000, 0x100000)
uc.mem_write(0x0, b'\xff' * 0x80000)          # erased internal flash
data = open(binf, 'rb').read()
uc.mem_write(0x8020, data)
uc.mem_write(0x10000000, b'\x00' * 0x2000)
RAM_PAINT = 0xA5
uc.mem_write(0x20000000, bytes([RAM_PAINT]) * 0x10000)

state = {'ticks': 0, 'wdt_last': 0, 'wdt_max': 0, 'reset': None,
         'confirm_ms': None, 'fault': None}
TICK_PER_READ = 2
FLASH_CS = 1 << 5

OVERRIDE_ONE = {
    0x40003118, 0x40003110, 0x40003120, 0x40003104, 0x40004104,
    0x40004148, 0x4000414C, 0x40004150, 0x40004160, 0x40004164,
    0x40007100, 0x40007104, 0x4000710C, 0x40007110, 0x40007114, 0x40007118,
    0x4001E400, 0x4000D100, 0x40000100, 0x40000104,
    0x4000E100, 0x4000E104, 0x4000F100, 0x4000F104, 0x4000F108,
}

# ---- NOR model: one transaction per CS low ------------------------------
norst = {'cs': False, 'cmd': None, 'addr': 0, 'n': 0, 'wel': False,
         'jedec_i': 0, 'ops': 0}
def nor_begin():
    norst.update(cs=True, cmd=None, addr=0, n=0)
def nor_byte(b):
    """one MOSI byte in, the MISO byte out"""
    s = norst
    if s['cmd'] is None:
        s['cmd'] = b
        s['n'] = 0
        if b == 0x06:
            s['wel'] = True
        elif b == 0x04:
            s['wel'] = False
        return 0xFF
    c = s['cmd']
    s['n'] += 1
    n = s['n']
    if c in (0x03, 0x0B, 0x02, 0x20, 0xD8, 0x52):
        if n <= 3:
            s['addr'] = ((s['addr'] << 8) | b) & 0xFFFFFF
            if n == 3 and c in (0x20, 0x52, 0xD8):
                size = {0x20: 0x1000, 0x52: 0x8000, 0xD8: 0x10000}[c]
                if s['wel']:
                    base = s['addr'] & ~(size - 1) % NOR_SIZE
                    base = (s['addr'] % NOR_SIZE) & ~(size - 1)
                    nor[base:base + size] = b'\xff' * size
                    s['ops'] += 1
                s['wel'] = False
            return 0xFF
        if c == 0x0B and n == 4:
            return 0xFF                       # dummy byte
        a = s['addr'] % NOR_SIZE
        if c in (0x03, 0x0B):
            s['addr'] += 1
            return nor[a]
        if c == 0x02:
            if s['wel']:
                page = a & ~0xFF
                off = (s['addr'] - (s['addr'] & ~0xFF)) & 0xFF
                nor[page + off] &= b
                s['addr'] = (s['addr'] & ~0xFF) | ((off + 1) & 0xFF)
            return 0xFF
        return 0xFF
    if c == 0x05:
        return 0x00                           # never busy
    if c == 0x9F:
        return [0x0B, 0x40, 0x16][(n - 1) % 3]
    if c == 0xAB:
        return 0x15 if n >= 4 else 0xFF
    return 0xFF
def nor_end():
    if norst['cmd'] == 0x02:
        norst['wel'] = False
        norst['ops'] += 1
    norst['cs'] = False

def spim_start():
    if not norst['cs']:
        return
    tp, tn, rp, rn = (struct.unpack('<I', uc.mem_read(a, 4))[0] for a in
                      (0x40003544, 0x40003548, 0x40003534, 0x40003538))
    n = max(tn, rn)
    tx = bytes(uc.mem_read(tp, tn)) if tn else b''
    out = bytearray()
    for i in range(n):
        out.append(nor_byte(tx[i] if i < tn else 0xFF))
    if rn and rp:
        uc.mem_write(rp, bytes(out[:rn]))

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
        gap = t - state['wdt_last']
        state['wdt_max'] = max(state['wdt_max'], gap)
        state['wdt_last'] = t
    elif addr == 0x40003010 and value:
        spim_start()
    elif addr == 0xE000ED0C and (value & 0xFFFF0004) == 0x05FA0004:
        state['reset'] = 'SCB_AIRCR system reset'
        uc.emu_stop()

def on_gpio(uc, access, addr, size, value, ud):
    if addr == 0x5000050C and value & FLASH_CS:      # OUTCLR: CS low
        nor_begin()
    elif addr == 0x50000508 and value & FLASH_CS:    # OUTSET: CS high
        if norst['cs']:
            nor_end()

uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=0x40000000, end=0x4003FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0x40000000, end=0x4003FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0xE000ED0C, end=0xE000ED0F)
uc.hook_add(UC_HOOK_MEM_WRITE, on_gpio, begin=0x50000508, end=0x5000050F)

watch = {}
for n in ['fault_blink', 'HardFault_Handler', 'Default_Handler',
          'MemManage_Handler', 'BusFault_Handler', 'UsageFault_Handler',
          'main', 'hal_init', 'lvgl_port_init', 'ui_init', 'ble_init',
          'hal_boot_confirm', 'persist_load', 'ui_tick', 'goal_show', 'hal_motor', 'goal_poke', 'hal_clock_restore',
          'lv_timer_handler']:
    if n in syms:
        watch[syms[n] & ~1] = n
if not syms:
    # no ELF: watch the fault vectors from the image's own vector table
    vt = struct.unpack('<16I', data[:64])
    for i, n in ((3, 'HardFault_Handler'), (4, 'MemManage_Handler'),
                 (5, 'BusFault_Handler'), (6, 'UsageFault_Handler')):
        watch[vt[i] & ~1] = n
seen = set()
def on_code(uc, addr, size, ud):
    n = watch.get(addr)
    if not n:
        return
    ms = state['ticks'] * 1000 // 32768
    if n not in seen:
        seen.add(n)
        if not quiet:
            print(f"  t={ms:6d} ms  enter {n}")
    if n == 'hal_boot_confirm' and state['confirm_ms'] is None:
        state['confirm_ms'] = ms
    if n in ('fault_blink', 'HardFault_Handler', 'Default_Handler',
             'MemManage_Handler', 'BusFault_Handler', 'UsageFault_Handler'):
        state['fault'] = f"{n} lr={sym(uc.reg_read(UC_ARM_REG_LR))}"
        state['reset'] = n
        uc.emu_stop()
for a in watch:
    uc.hook_add(UC_HOOK_CODE, on_code, begin=a, end=a + 1)

sp, pc = struct.unpack('<II', data[:8])
uc.reg_write(UC_ARM_REG_SP, sp)
pc_now = pc
emu_err = None
while True:
    try:
        uc.emu_start(pc_now | 1, 0xFFFFFFFF, count=2_000_000)
    except UcError as e:
        p = uc.reg_read(UC_ARM_REG_PC)
        hints = (b'\x20\xbf', b'\x30\xbf')
        if bytes(uc.mem_read(p, 2)) in hints:
            pc_now = p + 2
            continue
        if bytes(uc.mem_read(p - 2, 2)) in hints:
            pc_now = p
            continue
        emu_err = f"{e} at {sym(p)} lr {sym(uc.reg_read(UC_ARM_REG_LR))}"
        break
    if state['reset']:
        break
    pc_now = uc.reg_read(UC_ARM_REG_PC)
    if state['ticks'] * 1000 // 32768 > max_ms:
        break

ms = state['ticks'] * 1000 // 32768
ram = bytes(uc.mem_read(0x20000000, 0x10000))
end = syms.get('end', syms.get('_ebss', 0x20000000)) - 0x20000000
low = end
while low < 0x10000 and ram[low] == RAM_PAINT:
    low += 1
# skip the sbrk heap: the lowest stack byte is the last unpainted run
free_run, best = 0, (0, end)
i = end
while i < 0x10000:
    if ram[i] == RAM_PAINT:
        j = i
        while j < 0x10000 and ram[j] == RAM_PAINT:
            j += 1
        if j - i > best[0]:
            best = (j - i, i)
        i = j
    else:
        i += 1
stack_low = 0x20000000 + best[1] + best[0]
print(f"end t={ms} ms reset={state['reset']} fault={state['fault']} "
      f"emu_error={emu_err}")
print(f"confirmed={'yes at %d ms' % state['confirm_ms'] if state['confirm_ms'] is not None else 'no'} "
      f"image_ok={struct.unpack('<I', uc.mem_read(0x7BFE8, 4))[0]:#x} "
      f"max WDT gap={state['wdt_max']*1000//32768} ms nor_ops={norst['ops']}")
print(f"RAM: static end {0x20000000+end:#x}, deepest stack {stack_low:#x} "
      f"({0x20010000 - stack_low} B used), untouched gap {best[0]} B")
if nor_out:
    open(nor_out, 'wb').write(nor)
if state['confirm_ms'] is None and struct.unpack('<I', uc.mem_read(0x7BFE8, 4))[0] == 1:
    state['confirm_ms'] = -1          # no symbols: image_ok tells
ok = (state['confirm_ms'] is not None and not state['reset'] and not emu_err)
sys.exit(0 if ok else 1)
