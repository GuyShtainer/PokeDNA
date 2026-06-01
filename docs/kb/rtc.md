# Cartridge RTC (Seiko S-3511A over GPIO)

How the toolkit reads wall-clock date/time from the GBA gamepak's GPIO-mapped Seiko **S-3511A** real-time clock — the same RTC Pokémon Ruby/Sapphire/Emerald carts use and that the EZ-Flash Omega DE emulates — via a bit-banged serial protocol, with strict range validation and **no fallback**. The single consumer is FatFs's `get_fattime()`, which timestamps created/modified files (see [./fatfs-fileops.md](./fatfs-fileops.md)).

Vendored in this toolkit: `source/gba_rtc.{c,h}` and `lib/fatfs/diskio_write.c`.

## Load-bearing gotchas

| Gotcha | Why it bites | Cite |
|---|---|---|
| **Write `GPIO_CTRL` (`0x080000C8`) = 1 first.** | Until you do, the GPIO pins aren't readable/writable and reads return 0. | `source/gba_rtc.c:73` |
| **The redundant identical `GPIO_DATA` writes ARE the timing delay.** | `write_cmd` repeats the same write 3×, `read_byte` 4×, to let the slow serial clock settle (lifted from hardware-proven pokeemerald `siirtc.c`). Do **not** "optimize" them away. | `source/gba_rtc.c:36-39`, `source/gba_rtc.c:47-51` |
| **SIO data sits on bit1 (`0x2`), not bit0.** | Shift the command bit **left by 1** when presenting it; shift `GPIO_DATA` **right by 1** when sampling. | `source/gba_rtc.c:35`, `source/gba_rtc.c:52` |
| **DIR register must flip between command and reply.** | Command bytes are sent **MSB-first** with SIO an *output*; reply bytes are read **LSB-first** with SIO an *input*. | `source/gba_rtc.c:64-66` |
| **Range check IS the presence test — there is no detect call.** | The Omega DE only answers the GPIO RTC for ROMs it treats as RTC-enabled. Unrecognized homebrew gets garbage; the only signal "no RTC" is the range check returning `false`. | `source/gba_rtc.c:12-14`, `source/gba_rtc.c:96-98` |
| **No fallback / no fake date.** | On any implausible reading `gba_rtc_get` returns `false`; the caller then writes a *zero* (unset) FatFs timestamp rather than fabricating one. | `lib/fatfs/diskio_write.c:81-92` |
| **2000–2099 only.** | Year is 2 BCD digits (0..99); the century is hard-coded as `2000 + y`. | `source/gba_rtc.c:100` |
| **2-second DOS resolution.** | The FatFs DOS datetime stores `second / 2`, so sub-2-second precision is lost. | `lib/fatfs/diskio_write.c:89` |
| **`raw[3]` (day-of-week) is read but never used.** | It is clocked off the chip but not validated or returned; `GbaRtcTime` has no weekday field. | `source/gba_rtc.c:78` |

## The three GPIO registers

The RTC is reached through three consecutive 16-bit registers mapped in gamepak ROM space (accessible only because the cart header enables GPIO). Accessed as `volatile uint16_t`.

| Address | Macro | Role |
|---|---|---|
| `0x080000C4` | `GPIO_DATA` | pin data (read sampled levels / drive output levels) |
| `0x080000C6` | `GPIO_DIR`  | per-pin direction, **1 = output** |
| `0x080000C8` | `GPIO_CTRL` | read-enable / control — **write 1 before any access** |

Cite: `source/gba_rtc.c:18-20`, `source/gba_rtc.c:73`.

### Pin bits (within `GPIO_DATA` / `GPIO_DIR`)

| Bit | Value | Pin |
|---|---|---|
| 0 | `0x1` | `PIN_SCK` — serial clock |
| 1 | `0x2` | `PIN_SIO` — serial data (this is why data lives on bit1) |
| 2 | `0x4` | `PIN_CS` — chip select |

Cite: `source/gba_rtc.c:22-24`.

## Command bytes

S-3511A command byte = fixed high nibble `0110`, then a 3-bit register select, then the R/W bit (`1` = read).

| Command | Value | Reply |
|---|---|---|
| `CMD_STATUS_READ` | `0x63` | 1 byte — control/status register |
| `CMD_DATETIME_READ` | `0x65` | 7 BCD bytes — date + time |

Status bit 6 `STATUS_24HOUR` (`0x40`): `1` = 24-hour mode, `0` = 12-hour mode.

Cite: `source/gba_rtc.c:26-30`.

## The bit-bang transaction

`rtc_txn(cmd, buf, n)` (`source/gba_rtc.c:61-70`) drives one full command-then-reply cycle:

1. `GPIO_DATA = PIN_SCK` (SCK=1, CS=0), then `| PIN_CS` to **select** the chip.
2. `GPIO_DIR = PIN_SCK | PIN_SIO | PIN_CS` — SIO is an **output** while sending the command.
3. `write_cmd(cmd)` — clock the 8 command bits **MSB-first**.
4. `GPIO_DIR = PIN_SCK | PIN_CS` — flip SIO to an **input** for the reply (SCK & CS stay outputs).
5. `read_byte()` × `n` — read each reply byte **LSB-first**.
6. `GPIO_DATA = PIN_SCK` (CS dropped) then `= 0` to **deselect**.

```c
/* write_cmd: MSB-first, SIO is output (source/gba_rtc.c:33-41) */
uint16_t b = (uint16_t)((value >> (7 - i)) & 1) << 1;  /* bit onto SIO (bit1) */
GPIO_DATA = b | PIN_CS;            /* SCK=0, present bit  */
GPIO_DATA = b | PIN_CS;            /* (settling delay)    */
GPIO_DATA = b | PIN_CS;
GPIO_DATA = b | PIN_CS | PIN_SCK;  /* SCK=1 -> clocked in */

/* read_byte: LSB-first, SIO is input (source/gba_rtc.c:44-56) */
GPIO_DATA = PIN_CS;                /* SCK=0 (×4 settling) */
GPIO_DATA = PIN_CS | PIN_SCK;      /* SCK=1               */
uint8_t b = (uint8_t)((GPIO_DATA >> 1) & 1);   /* sample SIO on bit1 */
v = (uint8_t)((v >> 1) | (b << 7));            /* accumulate LSB-first */
```

## The 7 BCD datetime bytes

`CMD_DATETIME_READ` returns 7 BCD bytes. BCD decode is `((v >> 4) * 10) + (v & 0x0F)` (`source/gba_rtc.c:58`).

| Idx | Field | Mask before decode | Notes |
|---|---|---|---|
| `raw[0]` | year | full byte (0..99) | century added as `2000 + y` |
| `raw[1]` | month | `& 0x1F` | 1..12 |
| `raw[2]` | day | `& 0x3F` | 1..31 |
| `raw[3]` | day-of-week | — | **read but unused** |
| `raw[4]` | hour | see below | 12/24h dependent |
| `raw[5]` | minute | `& 0x7F` | 0..59 |
| `raw[6]` | second | `& 0x7F` | 0..59 |

Cite: `source/gba_rtc.c:78-85`.

### 12/24-hour handling (`source/gba_rtc.c:87-94`)

```c
if (status & STATUS_24HOUR) {
  h = bcd(raw[4] & 0x3F);                 /* 24-hour: hour is 0..23 directly */
} else {
  bool pm = (raw[4] & 0x80) != 0;         /* bit7 = PM flag */
  uint8_t h12 = bcd(raw[4] & 0x1F) % 12;  /* 12 -> 0 */
  h = (uint8_t)(pm ? h12 + 12 : h12);     /* 12 AM->0, 12 PM->12 */
}
```

## Range validation → the "no fallback" design

After decoding, the reading is rejected (function returns `false`) if any field is implausible. There is no separate presence/detect path: a `false` return *is* "no RTC exposed to this homebrew."

```c
/* source/gba_rtc.c:96-98 */
if (y > 99 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
    h > 23 || mi > 59 || se > 59)
  return false;   /* implausible -> treat as "no RTC" */
```

On success, `year = 2000 + y` and the struct is filled (`source/gba_rtc.c:100-106`).

### Downstream: `get_fattime()` returns 0 when unset

The sole caller packs a valid reading into the FatFs DOS datetime `DWORD`, or returns `0` (unset) when the read fails — never a fabricated date. This is the "RTC only, no fallback" policy in practice; see [./safety-pipeline.md](./safety-pipeline.md) for the broader no-fake-data stance.

```c
/* lib/fatfs/diskio_write.c:81-92 */
DWORD get_fattime(void) {
  GbaRtcTime t;
  if (gba_rtc_get(&t)) {
    return ((DWORD)(t.year - 1980) << 25)
         | ((DWORD)t.month  << 21)
         | ((DWORD)t.day    << 16)
         | ((DWORD)t.hour   << 11)
         | ((DWORD)t.minute << 5)
         | ((DWORD)(t.second / 2));   /* 2s DOS resolution */
  }
  return 0;   /* unset timestamp, not a fake date */
}
```

## Public API

```c
/* source/gba_rtc.h:8-15 */
typedef struct {
  uint16_t year;    /* full year, e.g. 2026 */
  uint8_t  month;   /* 1..12 */
  uint8_t  day;     /* 1..31 */
  uint8_t  hour;    /* 0..23 */
  uint8_t  minute;  /* 0..59 */
  uint8_t  second;  /* 0..59 */
} GbaRtcTime;

/* source/gba_rtc.h:21 */
bool gba_rtc_get(GbaRtcTime* out);
```

`gba_rtc_get` returns `true` and fills `*out` on an in-range reading, `false` otherwise. The helpers `write_cmd` / `read_byte` / `bcd` / `rtc_txn` are file-local statics; only `gba_rtc_get` is exported. The header does **not** declare the GPIO macros or pin constants — re-derive them from `gba_rtc.c` if you reuse the protocol elsewhere.

## Hardware notes

- **Chip:** Seiko S-3511A, identical to the part on Ruby/Sapphire/Emerald carts; the EZ-Flash Omega DE emulates it (see [./flashcart-io.md](./flashcart-io.md)).
- **Mapping:** GPIO lives in gamepak ROM space at `0x080000C4/C6/C8`, available only because the cartridge header enables GPIO.
- **Timing & protocol** are lifted from pokeemerald `siirtc.c`, which is hardware-validated on real S-3511A silicon — hence the deliberate settling-delay writes (see [./hardware-validation.md](./hardware-validation.md)).
- **Caveat:** the Omega DE only answers the GPIO RTC for ROMs it treats as RTC-enabled; plain homebrew may read garbage, which the range check filters.

## Sources

External / upstream:
- pokeemerald `siirtc.c` — hardware-proven S-3511A protocol and settling-delay timing this implementation follows: https://github.com/pret/pokeemerald/blob/master/src/siirtc.c
- GBATEK — GBA cartridge GPIO / RTC register map (`0x080000C4/C6/C8`): https://problemkaputt.de/gbatek.htm#gbacartridgegpio

Key source files (this toolkit):
- `source/gba_rtc.c` — GPIO regs, pin bits, bit-bang transaction, BCD decode, range validation.
- `source/gba_rtc.h` — `GbaRtcTime` struct and `gba_rtc_get` signature.
- `lib/fatfs/diskio_write.c` — `get_fattime()`, the no-fallback FatFs timestamp hook.

See also: [../CAPABILITIES.md](../CAPABILITIES.md), [../ROADMAP.md](../ROADMAP.md), [./fatfs-fileops.md](./fatfs-fileops.md), [./flashcart-io.md](./flashcart-io.md), [./safety-pipeline.md](./safety-pipeline.md), [./hardware-validation.md](./hardware-validation.md).
