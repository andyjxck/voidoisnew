#!/usr/bin/env python3
"""Tiny nRF52832 boot emulator for VoidOS (unicorn). Runs Reset_Handler
through init and the main loop with stub peripherals, and reports how far
it gets: faults, fault_blink, SCB reset, watchdog starvation."""
import struct, subprocess, sys
from unicorn import *
from unicorn.arm_const import *

elf = sys.argv[1]
binf = sys.argv[2]
max_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 6000

syms = {}
for line in subprocess.run(['arm-none-eabi-nm', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
rev = sorted((v & ~1, k) for k, v in syms.items())
import bisect
def sym(a):
    i = bisect.bisect_right(rev, (a, 'zzzz')) - 1
    return f"{rev[i][1]}+{a-rev[i][0]:#x}" if i >= 0 else hex(a)

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x0, 0x80000)
uc.mem_map(0x10000000, 0x2000)
uc.mem_map(0x20000000, 0x10000)
uc.mem_map(0x40000000, 0x40000)
uc.mem_map(0x50000000, 0x1000)
uc.mem_map(0xE0000000, 0x100000)
data = open(binf, 'rb').read()
uc.mem_write(0x8020, data)
uc.mem_write(0x10000000, b'\x00' * 0x2000)
uc.mem_write(0x7BFE8, b'\xff' * 0x18)

state = {'ticks': 0, 'wdt_last': 0, 'wdt_max': 0, 'reset': None, 'resets': []}
TICK_PER_READ = 2

OVERRIDE_ONE = {
    0x40003118,  # SPIM0 EVENTS_END
    0x40003110,  # SPIM0 ENDRX
    0x40003120,  # ENDTX
    0x40003104,  # STOPPED
    0x40004104,  # TWIM1 STOPPED
    0x40004148, 0x4000414C, 0x40004150, 0x40004160, 0x40004164,  # TWIM SUSPENDED/RXSTARTED/TXSTARTED/LASTRX/LASTTX
    0x40007100, 0x40007104, 0x4000710C, 0x40007110, 0x40007114,  # SAADC STARTED/END/DONE/RESULTDONE/CALDONE
    0x40007118,  # SAADC STOPPED
    0x4000E400,  # nope
    0x4001E400,  # NVMC READY
    0x4000D100,  # RNG VALRDY
    0x40000100, 0x40000104,  # CLOCK HFCLKSTARTED LFCLKSTARTED
    0x4000E100, 0x4000E104,  # ECB END
    0x4000F100, 0x4000F104, 0x4000F108,  # AAR/CCM
}

def on_read(uc, access, addr, size, value, ud):
    if addr in (0x40011504, 0x4000B504):  # RTC1/RTC0 COUNTER
        state['ticks'] += TICK_PER_READ
        uc.mem_write(addr, struct.pack('<I', state['ticks'] & 0xFFFFFF))
    elif addr in OVERRIDE_ONE:
        uc.mem_write(addr, struct.pack('<I', 1))
    elif addr == 0x40004124:  # TWIM ERROR
        uc.mem_write(addr, b'\0\0\0\0')
    elif addr == 0x40000418:  # LFCLKSTAT running
        uc.mem_write(addr, struct.pack('<I', 1 << 16 | 1))
    elif addr == 0x4000040C:  # HFCLKSTAT
        uc.mem_write(addr, struct.pack('<I', 1 << 16 | 1))
    elif addr == 0x4000D508:  # RNG VALUE
        state['rng'] = (state.get('rng', 7) * 1103515245 + 12345) & 0xff
        uc.mem_write(addr, struct.pack('<I', state['rng']))

def on_write(uc, access, addr, size, value, ud):
    t = state['ticks']
    if addr == 0x40010600:  # WDT RR0
        gap = t - state['wdt_last']
        state['wdt_max'] = max(state['wdt_max'], gap)
        state['wdt_last'] = t
    elif addr == 0xE000ED0C and (value & 0xFFFF0004) == 0x05FA0004:
        state['reset'] = 'SCB_AIRCR system reset'
        uc.emu_stop()

uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=0x40000000, end=0x4003FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0x40000000, end=0x4003FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0xE000ED0C, end=0xE000ED0F)

watch = {}
for n in ['fault_blink', 'HardFault_Handler', 'Default_Handler', 'main', 'hal_init',
          'display_init', 'lvgl_port_init', 'ui_init', 'ble_init', 'hal_boot_confirm',
          'lv_timer_handler', 'spi_flash_init', 'sensors_init', 'touch_init', 'persist_load']:
    if n in syms:
        watch[syms[n] & ~1] = n
seen = []
def on_code(uc, addr, size, ud):
    n = watch.get(addr)
    if n:
        ms = state['ticks'] * 1000 // 32768
        if n not in [s for s, _ in seen] or n in ('fault_blink',):
            seen.append((n, ms))
            print(f"  t={ms:6d} ms  enter {n}")
        if n in ('fault_blink', 'HardFault_Handler', 'Default_Handler'):
            lr = uc.reg_read(UC_ARM_REG_LR)
            print('   r0=', uc.reg_read(UC_ARM_REG_R0), 'lr=', sym(lr))
            state['reset'] = n
            uc.emu_stop()
for a in watch:
    uc.hook_add(UC_HOOK_CODE, on_code, begin=a, end=a + 1)

sp, pc = struct.unpack('<II', data[:8])
uc.reg_write(UC_ARM_REG_SP, sp)
pc_now = pc
while True:
    try:
        uc.emu_start(pc_now | 1, 0xFFFFFFFF, count=2_000_000)
    except UcError as e:
        p = uc.reg_read(UC_ARM_REG_PC)
        # 2.2.3: hal_delay_ms sleeps in WFE; unicorn has no WFE, so treat
        # it as a NOP (the RTC model advances on every read, so the
        # delay loop still ends) and count the sleeps
        hints = (b'\x20\xbf', b'\x30\xbf')
        if bytes(uc.mem_read(p, 2)) in hints:
            state['wfe'] = state.get('wfe', 0) + 1
            pc_now = p + 2
            continue
        if bytes(uc.mem_read(p - 2, 2)) in hints:   # PC already past it
            state['wfe'] = state.get('wfe', 0) + 1
            pc_now = p
            continue
        print('  EMU ERROR', e, 'at', sym(p), 'lr', sym(uc.reg_read(UC_ARM_REG_LR)))
        break
    if state['reset']:
        break
    pc_now = uc.reg_read(UC_ARM_REG_PC)
    ms = state['ticks'] * 1000 // 32768
    if ms > max_ms:
        break
ms = state['ticks'] * 1000 // 32768
print(f"end t={ms} ms pc={sym(uc.reg_read(UC_ARM_REG_PC))} reset={state['reset']} max WDT gap={state['wdt_max']*1000//32768} ms")
print('WFE sleeps:', state.get('wfe', 0), 'in', ms, 'ms')
print('stack low-water: sp now', hex(uc.reg_read(UC_ARM_REG_SP)))
