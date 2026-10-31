#!/usr/bin/env python3
"""Generate nimble_port/include/syscfg/syscfg.h for the VoidOS nRF52832
combined (controller+host) NimBLE build.

Defaults come from each package's own syscfg.yml; we emit a #define for
every MYNEWT_VAL name the compiled sources reference, plus explicit
overrides for our target. Run: tools/gen_syscfg.py
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NIMBLE = os.path.join(ROOT, 'vendor/nimble')
OUT = os.path.join(ROOT, 'nimble_port/include/syscfg/syscfg.h')

SEARCH_DIRS = [
    'nimble/host', 'nimble/controller', 'nimble/drivers/nrf5x',
    'nimble/include', 'nimble/transport', 'porting/nimble',
]

# ---- collect every MYNEWT_VAL(NAME) referenced ---------------------------
used = set()
for d in SEARCH_DIRS:
    for dp, _, fns in os.walk(os.path.join(NIMBLE, d)):
        if '/test' in dp:
            continue
        for fn in fns:
            if not fn.endswith(('.c', '.h')):
                continue
            with open(os.path.join(dp, fn), errors='replace') as f:
                src = f.read()
            used.update(re.findall(r'MYNEWT_VAL[A-Z_]*\(([A-Z0-9_]+)\)', src))
            used.update(re.findall(r'MYNEWT_VAL_([A-Z0-9_]+)\b', src))
used -= {'_name', ''}
# choice-prefixed names like MYNEWT_VAL_MCU_TARGET__nRF52832 appear
# through MYNEWT_VAL_CHOICE expansion; keep base names too
choice_bases = {n.split('__')[0] for n in used if '__' in n}
used = {n for n in used if '__' not in n} | choice_bases

# ---- parse yml defaults ---------------------------------------------------
defaults = {}
for dp, _, fns in os.walk(NIMBLE):
    for fn in fns:
        if not fn.startswith('syscfg') or not fn.endswith('.yml'):
            continue
        try:
            with open(os.path.join(dp, fn), errors='replace') as f:
                text = f.read()
        except OSError:
            continue
        in_defs = False
        cur = None
        for line in text.splitlines():
            m = re.match(r'syscfg\.defs:', line)
            if m:
                in_defs = True
                continue
            if not in_defs:
                continue
            m = re.match(r'\s{4}([A-Z0-9_]+):\s*$', line)
            if m:
                cur = m.group(1)
                continue
            m = re.match(r'\s+value:\s*(.*?)\s*$', line)
            if m and cur:
                val = m.group(1)
                if val.startswith("'") and val.endswith("'"):
                    val = val[1:-1]
                if val:
                    defaults.setdefault(cur, val)
                cur = None

# ---- overrides for our target ---------------------------------------------
overrides = {
    'MCU_TARGET': '"nRF52832"',
    'MCU_TARGET__nRF52832': '1',
    'MCU_TARGET__nRF52840': '0',
    'MCU_TARGET__nRF52811': '0',
    'MCU_TARGET__nRF5340_NET': '0',
    'MCU_HFCLK_SOURCE__HFXO': '1',
    'MCU_HFCLK_SOURCE__HFINT': '0',
    'MCU_HFCLK_SOURCE': '"HFXO"',
    'BSP_SIMULATED': '0',
    'OS_SYSVIEW': '0',
    'OS_MAIN_TASK_PRIO': '0',
    'OS_MAIN_STACK_SIZE': '0',
    'TIMER_0': '0', 'TIMER_1': '0', 'TIMER_2': '0',
    'TIMER_3': '0', 'TIMER_4': '0',
    'TIMER_5': '1',          # RTC0 -> os_cputime clock
    'HAL_TIMER_MAX': '0',
    'BLE_CONTROLLER': '1',
    'BLE_LL_PRIO': '0',
    'BLE_LL_STACK_SIZE': '0',
    'BLE_ROLE_PERIPHERAL': '1',
    'BLE_ROLE_BROADCASTER': '1',
    'BLE_ROLE_CENTRAL': '0',
    'BLE_ROLE_OBSERVER': '0',
    'BLE_MAX_CONNECTIONS': '1',
    'BLE_SM': '1',
    'BLE_SM_SC': '1',
    'BLE_SM_LEGACY': '1',
    'BLE_SM_SC_DEBUG_KEYS': '0',
    'BLE_LL_CFG_FEAT_LE_ENCRYPTION': '1',
    'BLE_LL_CFG_FEAT_LE_2M_PHY': '1',
    'BLE_LL_CFG_FEAT_LE_CODED_PHY': '0',
    'BLE_LL_CFG_FEAT_DATA_LEN_EXT': '1',
    'BLE_LL_CFG_FEAT_LE_CSA2': '1',
    'BLE_LL_CFG_FEAT_LL_PRIVACY': '0',
    'BLE_LL_MAX_PKT_SIZE': '251',
    'BLE_LL_NUM_SCAN_DUP_ADVS': '4',
    'BLE_LL_NUM_SCAN_RSP_ADVS': '4',
    'BLE_ATT_PREFERRED_MTU': '256',
    'BLE_L2CAP_SIG_MAX_PROCS': '1',
    'BLE_GATT_MAX_PROCS': '4',
    'BLE_MSYS_1_BLOCK_COUNT': '12',
    'BLE_MSYS_1_BLOCK_SIZE': '292',
    'BLE_MSYS_2_BLOCK_COUNT': '4',
    'BLE_MSYS_2_BLOCK_SIZE': '400',
    'BLE_EXT_ADV': '0',
    'BLE_PERIODIC_ADV': '0',
    'BLE_ISO': '0',
    'BLE_HCI_VS': '1',
    'BLE_HS_PHONY_HCI_ACKS': '0',
    'BLE_HS_REQUIRE_OS': '0',
    'BLE_HS_AUTO_START': '1',
    'BLE_MONITOR_RTT': '0',
    'BLE_MONITOR_UART': '0',
    'BLE_RNG_TIME_CB': '0',
    'BLE_FEM_PA': '0',
    'BLE_FEM_LNA': '0',
    'BLE_STORE_MAX_BONDS': '4',
    'BLE_STORE_MAX_CCCDS': '16',
    'BLE_GATT_DISC_ALL_SVCS': '0',
    'BLE_GATT_DISC_ALL_CHRS': '1',   # ANCS client: find the 3 chars
    'BLE_GATT_DISC_ALL_DSCS': '1',   # ANCS client: find the CCCDs
    'BLE_GATT_DISC_SVC_UUID': '1',   # ANCS client: find the service
    'BLE_GATT_DISC_CHRS_UUID': '0',
    'BLE_GATT_FIND_INC_SVCS': '0',
    'BLE_GATT_INDICATE': '0',
    'BLE_GATT_NOTIFY_MULTIPLE': '0',
    'BLE_GATT_READ': '1',
    'BLE_GATT_READ_UUID': '0',
    'BLE_GATT_READ_LONG': '0',
    'BLE_GATT_READ_MULT': '0',
    'BLE_GATT_READ_MULT_VAR': '0',
    'BLE_GATT_READ_MAX_ATTRS': '0',
    'BLE_GATT_WRITE': '1',           # ANCS client: subscribe + actions
    'BLE_GATT_WRITE_LONG': '0',
    'BLE_GATT_WRITE_MAX_ATTRS': '0',
    'BLE_GATT_WRITE_RELIABLE': '0',
    'BLE_GATT_RESUME_RATE': '1000',
    'BLE_GATT_NOTIFY': '1',           # gates notify RX (ANCS) AND TX (DFU acks)
    'BLE_CONN_SUBRATING': '0',
    'BLE_TRANSPORT': '1',
    'BLE_TRANSPORT_HS': '"native"',
    'BLE_TRANSPORT_HS__native': '1',
    'BLE_TRANSPORT_LL': '"native"',
    'BLE_TRANSPORT_LL__native': '1',
    'BLE_TRANSPORT_EVT_SIZE': '257',
    'BLE_TRANSPORT_EVT_COUNT': '4',
    'BLE_TRANSPORT_EVT_DISCARDABLE_COUNT': '8',
    'BLE_TRANSPORT_ACL_COUNT': '4',
    'BLE_TRANSPORT_ACL_SIZE': '255',
    'BLE_ACL_BUF_COUNT': '4',
    'BLE_ACL_BUF_SIZE': '255',
    'BLE_HCI_EVT_BUF_SIZE': '70',
    'BLE_HCI_EVT_HI_BUF_COUNT': '4',
    'BLE_HCI_EVT_LO_BUF_COUNT': '8',
    'MSYS_1_BLOCK_COUNT': '12',
    'MSYS_1_BLOCK_SIZE': '292',
    'MSYS_2_BLOCK_COUNT': '4',
    'MSYS_2_BLOCK_SIZE': '400',
    'MSYS_1_SANITY_MIN_COUNT': '0',
    'MSYS_2_SANITY_MIN_COUNT': '0',
    'MSYS_SANITY_TIMEOUT': '60000',
    'OS_CPUTIME_FREQ': '32768',
    'OS_CPUTIME_TIMER_NUM': '5',
    'OS_TICKS_PER_SEC': '1000',
    'LOG_LEVEL': '0',
    'TRNG': '0',
    'WATCHDOG_INTERVAL': '0',
    'BLE_LL_PHY': '1',
    'BLE_RPA_TIMEOUT': '300',
    'BLE_STORE_CONFIG_PERSIST': '0',
    'BLE_SVC_GAP_DEVICE_NAME': '"VoidOS"',
    'BLE_SVC_GAP_APPEARANCE': '0x00C0',
    'BLE_VERSION': '50',
    'BLE_EATT_CHAN_NUM': '0',
    'BLE_ISO_BROADCAST_SINK': '0',
    'BLE_ISO_BROADCAST_SOURCE': '0',
    'BLE_ISO_MAX_BIGS': '0',
    'BLE_ISO_MAX_BISES': '0',
}

missing = sorted(n for n in used if n not in defaults and n not in overrides)
if missing:
    print('defaults missing for:', ' '.join(missing), file=sys.stderr)

# ---- resolve MYNEWT_VAL(x) expressions to literals -------------------------
# a define body containing MYNEWT_VAL(...) is unusable: inside #if the
# function-like macro can't be re-entered while already expanding (blue
# paint). newt resolves these at generation time; we do the same.
def resolve(v, depth=0):
    if depth > 20:
        return v
    m = re.search(r'MYNEWT_VAL\(([A-Z0-9_]+)\)', v)
    if not m:
        return v
    name = m.group(1)
    sub = overrides.get(name, defaults.get(name, '0'))
    return resolve(v[:m.start()] + sub + v[m.end():], depth + 1)

for k in list(defaults):
    defaults[k] = resolve(defaults[k])

lines = []
lines.append('/* Auto-generated for the VoidOS nRF52832 NimBLE build by')
lines.append('   tools/gen_syscfg.py. Do not edit; edit the script. */')
lines.append('#ifndef H_MYNEWT_SYSCFG_')
lines.append('#define H_MYNEWT_SYSCFG_')
lines.append('')
lines.append('#define MYNEWT_VAL(_name)                MYNEWT_VAL_ ## _name')
lines.append('#define MYNEWT_VAL_CHOICE(_name, _val)   MYNEWT_VAL_ ## _name ## __ ## _val')
lines.append('')
lines.append('/* ---- target overrides ------------------------------------- */')
for k in sorted(overrides):
    lines.append(f'#define MYNEWT_VAL_{k} ({overrides[k]})')
lines.append('')
lines.append('/* ---- package defaults ---------------------------------------- */')
for n in sorted(used | set(defaults)):
    v = defaults.get(n, '0')
    lines.append(f'#ifndef MYNEWT_VAL_{n}')
    lines.append(f'#define MYNEWT_VAL_{n} ({v})')
    lines.append('#endif')
lines.append('')
lines.append('#endif /* H_MYNEWT_SYSCFG_ */')

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'w') as f:
    f.write('\n'.join(lines) + '\n')
print(f'wrote {OUT}: {len(used)} used names, {len(defaults)} yml defaults')
