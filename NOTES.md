# A serial bootloader for Davis weather consoles (ATmega128)

Self-contained notes. Everything here was established by reverse engineering
captured update sessions and disassembling the stock firmware images; no prior
context is assumed.

Where a conclusion was later disproven, the note says so. Several of the
findings below cost real debugging time precisely because an earlier, plausible
explanation turned out to be wrong, and knowing which turns were dead ends is
usually more useful than the final answer alone.

---

## 1. Goal

Flash a custom serial bootloader into a Davis console built around an
ATmega128, so that the console can be updated with the stock Davis updater over
the serial port (`BOOT` → `C` / `F` / `P` / `A`).

Main use case: a console whose firmware was flashed with a programmer and which
therefore **has no bootloader** — the boot section is blank (`0xFF`). Symptom:
the `BVER` command returns nothing useful.

---

## 2. Hardware

| | |
|---|---|
| MCU | **ATmega128** (not the 1281) |
| Flash | 128 KiB, page size **256 bytes** |
| Service port | **VP2/Envoy: USART0. Vue: USART1.** 19200 baud, 8N1 |
| Clock | VP2 / Envoy: **1.8432 MHz crystal**. Vue: **internal RC 2 MHz** plus a 32.768 kHz watch crystal on TOSC |

Identifying the MCU: the interrupt vector table has 35 four-byte slots
(0x000–0x08B). The ATmega128 has 35 vectors, the ATmega1281 has 57.

---

## 3. Boot section size — and the words-versus-bytes trap

The boot section size is set by fuse H, bits BOOTSZ1:BOOTSZ0:

| BOOTSZ | size (words) | size (bytes) | word address | **boot start (byte)** | application pages |
|---|---|---|---|---|---|
| `11` | 512 | 1024 B | $FE00 | 0x1FC00 | — |
| `10` | **1024** | **2048 B** | $FC00 | **0x1F800** | 504 (0–503) |
| `01` | **2048** | **4096 B** | $F800 | **0x1F000** | 496 (0–495) |
| `00` | 4096 | 8192 B | $F000 | 0x1E000 | — |

Note that the larger binary value selects the **smaller** section. Easy to get
backwards.

**The trap.** The ATmega128 datasheet and the engbedded.com fuse calculator
give the boot section size in **words** and the start as a **word address**.
`avr-gcc --section-start` expects a **byte address**, i.e. twice that. A fuse
calculator showing "1024 words, boot start $FC00" means a **2 KB** section
starting at byte **0x1F800**. Do not read "1k words" as "1 kB".

| Platform | fuse H | BOOTSZ | boot section | `-Wl,--section-start=.text=` |
|---|---|---|---|---|
| Vantage Vue (6351) | `0xD2` | `01` | 4 KiB @ 0x1F000 | `0x1F000` |
| Vantage Pro2 (6312) | `0xD4` | `10` | 2 KiB @ 0x1F800 | `0x1F800` |
| Envoy (6316EU) | `0xD4` | `10` | 2 KiB @ 0x1F800 | `0x1F800` |

Independent confirmation from two directions:

- The VP2 firmware images are 129 024 B = 0x1F800 and take 504 pages. If the
  boot section started at 0x1F000, an update would erase the bootloader itself.
- The application **checks the High fuse itself** before handing over, and the
  constants it compares against are exactly `0xD2` and `0xD4` — see section 7.

---

## 4. Fuses

```
Vue    L=0xE2  H=0xD2  E=0xFF  LB=0xFC     (two units, in agreement)
Envoy  L=0xFD  H=0xD4  E=0xFF  LB=0xFC
```

Decoding H (ATmega128: `OCDEN JTAGEN SPIEN CKOPT EESAVE BOOTSZ1 BOOTSZ0 BOOTRST`):

- `SPIEN = 0` — ISP programming enabled
- `EESAVE = 0` — **chip erase preserves the EEPROM**
- `BOOTRST = 0` — **every reset jumps into the boot section**, including power-on

Decoding L on the Envoy: `CKSEL = 1101` → external crystal 0.9–3.0 MHz → 1.8432 MHz.

Decoding L on the Vue: `L = 0xE2` → `CKSEL = 0010` = internal RC **2 MHz**,
`SUT = 10`. Confirmed on two units. Together with `U2X1 = 1` (section 5) this
is fully consistent; an apparent contradiction with the baud rates disappeared
once U2X was taken into account.

Lock bits `LB = 0xFC` → LB1=LB2=0 (mode 3: reading and verification disabled),
with BLB01/BLB02/BLB11/BLB12 **unprogrammed**, so SPM from the boot section
into the application section is allowed. **Do not set LB until testing is
finished** — it blocks verification and makes diagnosis much harder.

> **The High fuse is load-bearing.** The application refuses to hand over to
> the bootloader unless fuse H matches exactly. A console whose H fuse has
> drifted answers `BOOT` with `!` and then goes deaf. See section 7.

---

## 5. Which USART, and the clock

This is the difference that killed the first attempt on the Vue: the bootloader
had USART0 hard-coded and simply sat silent. Established from the interrupt
vector tables, then confirmed with a multimeter on the board.

| vector | VP2 3.88 | Vue 4.30 |
|---|---|---|
| 0x48 USART0_RX | `jmp 0x1BA34` | **`FF FF FF FF` — empty** |
| 0x4C USART0_UDRE | empty | empty |
| 0x50 USART0_TX | empty | empty |
| 0x78 USART1_RX | `jmp 0x1BA5C` | `jmp 0x1C128` |

- **VP2** — the baud routine at `0x1B7F6` picks the USART at runtime from bit 1
  of a flag in SRAM `0x0892`: `sbrs r17,1` selects either `out UBRR0L,r16` or
  `sts UBRR1L,r16`, both receiving the same value. A bootloader on USART0 works.
- **Vue** — the routine at `0x1BF78` writes **only `sts 0x0099` (UBRR1L)**. The
  Vue does not use USART0 at all. Apparent `out 0x09` hits in the Vue image
  (`0x0B24`, `0x4460`, `0x447E`) lie inside data tables — the surrounding bytes
  disassemble to `.word 0xfeb9 ????`, which is not code.

Pins (ATmega128, TQFP64): RXD0/TXD0 = PE0/PE1 = pins **2/3**;
RXD1/TXD1 = PD2/PD3 = pins **27/28**.

### U2X — the piece that makes the Vue numbers work

Entry into the Vue baud routine (`0x1BF52`) sets U2X unconditionally:

```
sts  0x0098, r17    ; UBRR1H = 0
lds  r17, 0x009B    ; UCSR1A
ori  r17, 0x02      ; U2X1 = 1
sts  0x009B, r17
```

With `U2X = 1` the divisor is 8 rather than 16, and the ladder
`103/103/51/25/16/12` resolves at a 2 MHz clock into the canonical Davis list:

| branch | UBRR1L | U2X | baud @2 MHz | error |
|---|---|---|---|---|
| idx 0 | 103 | **0** (`andi 0xFD`) | 1200 | +0.16 % |
| idx 1 | 103 | 1 | 2400 | +0.16 % |
| idx 2 | 51 | 1 | 4800 | +0.16 % |
| idx 3 | 25 | 1 | 9600 | +0.16 % |
| idx 4 | 16 | 1 | 14400 | +2.1 % |
| default | 12 | 1 | **19200** | +0.16 % |

The 1200 branch is the only one that clears U2X — at `U2X = 1` it could not
have reused the same constant 103.

The VP2 has the identical six-entry list `95/47/23/11/7/5` on its 1.8432 MHz
crystal with `U2X = 0`; its routine never touches U2X. Hence:

| | clock | U2X | UBRR for 19200 | error |
|---|---|---|---|---|
| VP2 / Envoy | 1.8432 MHz crystal | 0 | **5** | 0.00 % |
| Vue | 2 MHz internal RC | **1** | **12** | 0.16 % |

Values `16` and `12` give no standard baud rate at 1.8432 MHz or at 2 MHz with
`U2X = 0`, which is what first pointed at U2X being involved.

---

## 6. Trimming the internal RC (Vue only)

The Vue **trims its RC oscillator in a loop** against the 32.768 kHz watch
crystal on TOSC. The code at `0x14610` increments or decrements a counter in
SRAM `0x071A`, clamps it to `0x00..0xFF`, and writes it into `OSCCAL`
(`0x006F`):

```
sts  0x071A, r16
lds  r16, 0x071A
sts  0x006F, r16        ; OSCCAL
```

The watch crystal is visible independently: `out OCR0 / in ASSR / sbrc r16,1`
loops (waiting on `OCR0UB`) only make sense with Timer0 in asynchronous mode.
The VP2 never touches `OSCCAL` — it has a real crystal.

**Consequence for a bootloader.** It starts with whatever `OSCCAL` the hardware
loaded at reset. The ATmega128's factory RC calibration is ±3 % under nominal
conditions, and `U2X = 1` (8× oversampling instead of 16×) narrows the UART
error budget to roughly ±2 %. That Davis built a closed trimming loop at all is
strong evidence the default cannot be trusted.

Reading the calibration byte from the signature row is **not possible in
software** on the ATmega128 (there is no `SIGRD` bit in `SPMCSR`), which is
presumably why Davis measures against the crystal instead.

### Confirmed on hardware

The banner printed legibly, but a continuous stream of `'U'` (`0x55`) contained
occasional `0xD5` — that is `0x55` with **only bit d7 wrong**:

```
0x55 = 0101 0101      transmitted
0xD5 = 1101 0101      received
```

That is the textbook signature of a transmitter faster than the receiver: the
sampling points drift through the frame and by the last data bit they have
moved into the stop bit (`1`). The threshold for that is about **+5.9 %**, and
the errors were sporadic, so the RC was sitting right on the edge — roughly
**+6 %, about 2.12 MHz**.

It also explains why reception failed entirely. AVR receiver tolerance at
`U2X = 1` is 94.12 %–105.66 %. Against the bootloader's nominal 20360 baud, a
19200 signal from the PC is **94.3 %** — essentially on the 94.12 % limit.
Transmission barely worked; reception did not.

### The implementation

`clock_trim()` in `davis_boot.c`, enabled by the `CLOCK_TRIM` macro
(Vue = 1, VP2 = 0). Timer0 in asynchronous mode counts 32768 Hz crystal ticks
while Timer1 counts CPU cycles. The target is chosen so that `UBRR = 12` at
`U2X = 1` yields exactly 19200:

```
F_target = 19200 * 8 * (12+1) = 1 996 800 Hz
target   = 32 * 1996800 / 32768 = 1950 cycles   (exact, no remainder)
```

The full `OSCCAL` range 0..255 is scanned and the best match taken. It takes
about 0.5 s. Without a crystal on TOSC the routine returns without touching
`OSCCAL`.

**Trimming runs only after a real reset.** When the application jumps in via
`BOOT` (see section 7) the clock is already trimmed by the application, the
crystal is already running, and Timer0 belongs to the application — the
datasheet warns explicitly that switching `AS0` while the counter runs can
corrupt `TCNT0`, `OCR0` and `TCCR0`.

### Two optimisations that were tried and reverted

Both are recorded because both looked obviously correct and both broke things.

**Caching the result in `.noinit`.** Tempting: remember the value, skip the
scan next time. Wrong. `.noinit` survives a reset but **does not survive
running the application** — the application owns the whole SRAM, so those
addresses sit in the middle of its data and stack. Worse, the leftover bytes
are *deterministic* (same application, same code path), so a validity check
either passes every time or never; this is not a 1-in-65536 risk but a
reproducible failure. Symptom: garbage written into `OSCCAL`, clock detuned,
bootloader deaf.

The same reasoning removed a `BOOT_MAGIC` marker in `.noinit` that had been
inherited from an earlier design. Nothing ever wrote the magic value, so the
test could only ever produce false positives — each one a console that refuses
to boot on its own.

**Replacing the fixed stabilisation wait with a convergence test** ("three
consecutive measurements agreeing to within one cycle"). A crystal drifting
slowly up to its final frequency satisfies that easily — one cycle in 1950 is
0.05 % — while still being percent away, so the trim locked onto a wrong
reference. The fixed ~0.25 s wait is slower but correct.

A binary search over `OSCCAL` was reverted at the same time: `OSCCAL` is not
reliably monotonic, and while a local refinement plus a fallback did work in
simulation, the full scan is cheap enough and has no such failure mode.

---

## 7. How the application hands over — the `BOOT` command

This was the single largest unknown, and it is now fully resolved from the
disassembly of **Vue 4.30** (`0xFDD6`), confirmed independently against
**VP2 3.88** (`0x154D8`).

```
fdd6:  ldi r20,0x04 / r18:r19 = 0x1D56   ; compare input against "BOOT"
fddc:  call 0x10EA8
fde0:  brne  ->  not BOOT, try next command

fde2:  cli                               ; interrupts off
fde4:  ldi r16, 0x09                     ; (1<<BLBSET)|(1<<SPMEN)
fde6:  ldi r30,0x00 / ldi r31,0x00       ; Z = 0  -> fuse Low
fdea:  call 0x10DEC                      ;   sts SPMCSR,r16 ; lpm r17,Z
fdee:  ldi r30, 0x01                     ; Z = 1  -> lock bits
fdf2:  ldi r30, 0x02                     ; Z = 2  -> extended fuse
fdf6:  ldi r30, 0x03                     ; Z = 3  -> FUSE HIGH
fdf8:  sts 0x0068, r16                   ; SPMCSR = 0x09
fdfc:  lpm r16, Z                        ; read the High fuse
fdfe:  cpi r16, 0xD2                     ; ==== THE TEST ====
fe00:  breq -> 0xFE08                    ; match -> hand over
fe02:  ldi r16, 0x21                     ; NO MATCH -> send '!'
fe04:  rcall <send byte>
fe06:  rjmp  <back to the command loop>  ; and do NOT hand over

fe08:  call 0x1112A                      ; ldi r16,0x06 ; send  -> ACK
fe0c:  call 0x12B94
fe10:  call 0x110E0                      ; shut down peripherals
fe14:  rcall 0x10DFE                     ; reset the serial receiver
fe16:  ldi r30,0x00 / ldi r31,0xF8       ; Z = 0xF800 (word address)
fe1a:  ijmp                              ; jump to byte 0x1F000
```

### Cross-check against VP2 — same code, different constants

| | Vue 4.30 | VP2 3.88 |
|---|---|---|
| handler address | `0xFDD6` | `0x154D8` |
| required fuse H | **`0xD2`** | **`0xD4`** |
| `ldi r31,` | `0xF8` → **0x1F000** | `0xFC` → **0x1F800** |

Both fuse constants match the platforms' actual fuse values, and both jump
targets match the start of their boot sections. That corroborates the reading
and, as a by-product, **verifies the fuse H values in section 3 without reading
a single fuse**.

### What follows

1. **The `06` after `BOOT` is sent by the APPLICATION**, not by the bootloader.
   An earlier deduction ("same application, different reply, therefore the
   bootloader answers") was wrong; the difference came from the fuse test not
   passing at all on the console under test.
2. **The `!` after `BOOT` is also sent by the application** and means exactly:
   *the High fuse does not have the required value, I am not handing over*.
   This is why the Davis updater reports a fuse problem — it knows this byte.
3. **The application does not reset**, it uses `ijmp`. So `MCUCSR` on this path
   holds whatever we left in it — zero, because we clear it ourselves. `WDRF`
   will **never** appear on this path.
4. The bootloader **must not** send an ACK at startup; it would duplicate the
   byte the application already sent.

### After a rejection the application goes deaf

The rejection path disables interrupts and never restores them:

```
fde2:  cli                     ; interrupts off, BEFORE the fuse test
fdfe:  cpi r16, 0xD2
fe02:  ldi r16, 0x21           ; send '!'
fe06:  rjmp 0x10dc2            ; back to the command loop, no sei
```

There is no `sei` anywhere on that path. Vue serial reception is
interrupt-driven (the `USART1_RX` vector at `0x78` is populated), so after a
rejection the application **stops receiving anything** until the next reset.

That is the source of the symptom "after `BOOT` a single `!` arrives and the
console goes silent".

### `!` is ambiguous — how to tell the two apart

`!` is both the application's rejection and the bootloader's ordinary prompt.
The **next** command disambiguates:

| after `!`, send `ID` | reply | conclusion |
|---|---|---|
| | `6351B` | the bootloader — the handover worked |
| | `6351` | the application is alive and rejected on the fuse test |
| | **silence** | the application rejected and went deaf (`cli` without `sei`) |

In a `BANNER=1` build the banner settles it too: it appears **only** if the
bootloader actually started.

### The bootloader must recognise the jump

Because the application arrives by a plain `ijmp` with no marker and no reset,
a bootloader that treated this as an ordinary start would see a valid CRC and
bounce straight back — making `BOOT` a no-op. Observed exactly that:

```
BOOT  ->  06          ACK from the application
ID    ->  (silence)   bootloader busy computing the CRC
app banner reappears  the application had been restarted
```

The discriminator is that **no flag in `MCUCSR` means no reset happened**.
Since `main()` is running regardless, something jumped here, and the only thing
that jumps into the boot section is the `BOOT` handler. The application itself
only ever *clears* `MCUCSR` (`0x14A0C`: `in`, `andi 0xF7`, `out`, then `out 0`)
and never sets a flag, so zero is trustworthy.

Full decision table as implemented:

| `MCUCSR` | situation | CRC | decision | CRC computed |
|---|---|---|---|---|
| `00` | jumped from the application | — | **command mode** | skipped |
| `01` | power-on (POR) | OK | start application | yes |
| `01` | power-on (POR) | bad | command mode | yes |
| `02` | external reset (EXT) | OK | start application | yes |
| `02` | external reset (EXT) | bad | command mode | yes |
| `08` | watchdog reset (WDT) | — | command mode | skipped |

Skipping the CRC on the `BOOT` path matters: the updater sends `C` about two
seconds later and the USART receive buffer holds only two bytes, so a 2–3 s CRC
pass at that moment loses the command.

---

## 8. Bootloader protocol

Only the **first letter** of a command is decoded; the rest of the line up to
`\n` is skipped. Confirmed: `BVER\n` and `B\n` give identical replies, and
`ID\n` works the same way with `D` swallowed as part of the line.

```
'\n' / '\r'        ->  '!'                  prompt
'I'   (ID\n)       ->  "6312B"              model + 'B', no CR/LF
'B'   (BVER\n)     ->  06 <lo> <hi>         ACK + 2 version bytes
'C'                ->  06                   invalidate the application
'F'  + 256 bytes   ->  06 ... 06            ACK command, ACK data
'P' <n> '\n'       ->  06                   program page n (address = n × 256)
'A'                ->  (nothing)            jump to 0x0000
```

### Who is answering — confirmed from the captures

Source: `VP2/sessions/VP2_2025-03-19_1647_intro.txt`, a console running the
bootloader alone with no application. It is the only such session in the
archive, and it settles several questions at once.

| command | bootloader | application |
|---|---|---|
| `\n` | `21` (`!`) | `0A 0D` |
| `ID\n` | `36 33 31 32 42` (`6312B`) | `0A 0D 36 33 31 32 0A 0D` (`6312`) |
| `VER\n` | `21` (`!`) — unknown command | `\n\r OK \n\r Sep 11 2017` |
| `BVER\n` | `06 11 03` — **with ACK** | `11 03` — no ACK |
| `B\n` | `06 11 03` | — |

Two things worth highlighting:

- **An unknown command returns `!`.** `VER` is not in the bootloader's set and
  gets the prompt. Hence the `default` branch in the command switch.
- **`BVER` from the bootloader carries an ACK, from the application it does
  not.** An independent way of telling who is on the other end.

### How the updater recognises the bootloader — the `B` suffix

Both flows appear in the captures:

**Console with no application** (VP2 1647):
```
0A     -> 21               '!'
ID\n   -> "6312B"          with 'B' -> this is the bootloader
C\n    -> 06               goes STRAIGHT to erase; BOOT never sent
```

**Console with an application** (Vue 1511):
```
ID\n   -> "\n\r6351\n\r"    no 'B' -> this is the application
BOOT\n -> 06                only now BOOT
C\n    -> 06
```

The `B` suffix in the `ID` reply is therefore **the mechanism by which the
updater knows it is talking to the bootloader**. Do not remove it.

### `BOOT` versus `BVER` — both start with `B`

Since only the first letter is decoded, `BOOT` originally fell into `case 'B'`
and replied `06 14 03` instead of a bare `06`. The two extra bytes stayed in
the stream and desynchronised the updater's next exchange. They are told apart
by the rest of the line, which is already buffered in `arg[]`:

| command | `arg[0]` | reply |
|---|---|---|
| `BOOT\n` | `O` | `06` |
| `BVER\n` | `V` | `06 14 03` |
| `B\n` | `\0` | `06 14 03` |

The compiler folds `arg[0] != 'O' && arg[0] != 'o'` into `andi 0xDF` plus a
single `cpi 0x4F`, so it costs four bytes.

### CR+LF handling

The Davis updater terminates commands with a bare `\n`, but a terminal sends
`\r\n`. Without handling, the second character reached the top of the loop as a
new command and produced a spurious prompt (`ID\r\n` → `6351B!`). The bootloader
remembers the last end-of-line character and swallows the partner of a pair.
Two **identical** characters in a row are still two separate lines and two
prompts.

A side effect worth knowing: before pairing was added, an unknown command
produced `!` — but by accident, from the second half of the CRLF, not because
the code intended it. After the fix, typing something unknown produced dead
silence. The `default` branch now emits the prompt explicitly, which also
matches the stock bootloader's behaviour (`VER` → `!`).

The bootloader also **folds lower case to upper case**. The stock updater sends
upper case so this changes nothing protocol-wise, but when testing by hand it
is easy to type `b` instead of `B` and get silence — a false alarm that cost
time once already.

### Where `BVER` gets its version byte

The `BVER` handler is identical in both images:

```
ldi  r30, 0xFD
ldi  r31, 0xFF          ; Z = 0xFFFD
ldi  r19, 0x01
out  0x3B, r19          ; RAMPZ = 1   ->  address 0x1FFFD
elpm r16, Z             ; ONE byte from the boot section
rcall <send byte>
ldi  r16, 0x03          ; second byte: CONSTANT IN THE APPLICATION
rjmp <send byte, return>
```

- The application reads **exactly one byte from 0x1FFFD**, three bytes from the
  end of flash. The address is identical on both platforms despite the
  different boot section sizes.
- The second reply byte (`0x03`) is a **constant in the application**, not
  something the bootloader supplies. So the observed `11 03` and `14 03` are
  one version byte plus a fixed marker.
- There is **no guard against an empty boot section**. A console without a
  bootloader answers `FF 03`, not "nothing". Both bytes are unprintable, so in
  a terminal or a text log it looks like no reply at all — the difference only
  shows in a hex dump.

The bootloader must therefore place its version byte at 0x1FFFD, which
`-Wl,--section-start=.bootver=0x1FFFD` takes care of.

---

## 9. Firmware package format

The image is exactly the whole application section (0x00000 up to the start of
the boot section minus one), sent in 256-byte pages.

**Footer:** the last two bytes are a **CRC-16/XMODEM stored big-endian**
(poly 0x1021, init 0x0000, MSB first) over everything from 0x00000 to the end
of the image minus two.

Verified against three real images:

| image | size | CRC |
|---|---|---|
| Vue 4.30 | 126 976 | `0xD795` |
| VP2 3.80 | 129 024 | `0xF6CD` |
| VP2 3.88 | 129 024 | `0x9E6B` |

The updater writes every page contiguously from 0 to the last, covering the
whole section — confirmed from both flash sessions (Vue: 496 pages 0–495;
VP2: 504 pages 0–503).

The Vue image also carries the word `AE 01` = 430 (version 4.30) just before
the CRC. The VP2 images have `FF FF` there, so that field is not universal and
should not be relied on.

`tools/check_image.py` verifies all of this for any image in one command. Only
one Vue firmware version has been checked so far, so running it against further
images is genuinely useful.

---

## 10. Building

```bash
make              # silent production build, both platforms
make BANNER=1     # minimal diagnostics
make BANNER=2     # full diagnostics
make size         # flash usage with a hard limit
make check        # sanity checks
```

Reference results with avr-gcc 7.3.0 and avr-libc 2.0.0:

| build | VP2 / Envoy | Vue |
|---|---|---|
| `make` (greeting, no diagnostics) | 1196 B / 2045 B (58 %) | 1496 B / 4093 B (36 %) |
| `make BANNER=1` | 1478 B (72 %) | 1774 B (43 %) |
| `make BANNER=2 GREETING=0` | 1960 B (95 %) | 2696 B (65 %) |
| `make GREETING=0` (fully silent) | 1022 B (49 %) | 1348 B (32 %) |

`make BANNER=2` with the greeting enabled **does not fit** the VP2 boot
section - the linker refuses, reporting that `.data` overlaps `.bootver`,
rather than producing a broken image. Use `GREETING=0` for full diagnostics
on VP2/Envoy. The Vue has room for both but the combination is not built by
default either, for symmetry.

The limits are 2045 B and 4093 B rather than 2048 and 4096 because the version
byte occupies 0x1FFFD. `make size` enforces them and fails the build.

The Vue build is larger because the USART1 registers are memory-mapped
(`sts`/`lds`, 4 bytes) rather than in I/O space (`out`/`in`, 2 bytes).

`make check` verifies four things automatically:

1. the version byte is present at 0x1FFFD;
2. RAMPZ is written around `SPM`/`ELPM` — without it half the pages would land
   at the wrong addresses on a 128 KiB part;
3. **no `LPM` instruction exists in the image** — see section 12;
4. **the VP2 build reaches `UDR0` and not `UDR1`, and the Vue build the
   reverse** — this guards against repeating the mistake that killed the first
   attempt on the Vue.

---

## 11. Flashing

### Case A — console has an application, no bootloader (the main scenario)

The boot section is already erased (`0xFF`), so it can be written without
erasing everything. **`-D` is essential** — it disables the automatic chip
erase; without it avrdude wipes the application.

```bash
avrdude -c <programmer> -p m128 -D -U flash:w:davis_boot_VUE.hex:i
```

Then check fuse H: `BOOTRST` must be programmed (bit 0 = 0). For the Vue,
`H = 0xD2` already satisfies that; if so, **do not touch the fuses at all**.

### Case A2 — overwriting an existing bootloader

`-D` works **only** when the boot section is pristine (`0xFF`). Overwriting a
previous bootloader with `-D` **must** fail, because flash programming can only
change bits `1→0`. Symptom:

```
Warning: flash verification mismatch
  device 0x00 != input 0x11 at addr 0x1f08c (error)
```

The device held `0x00` from the old build and the new one wants `0x11`; without
an erase that is physically impossible. Write **with** the erase (drop `-D`):

```bash
avrdude -c <programmer> -p m128 -U flash:w:davis_boot_VUE.hex:i
```

**That erases the application.** What survives a chip erase:

| | state |
|---|---|
| fuses L/H/E | untouched |
| EEPROM | **preserved** — `EESAVE` is programmed (H=`0xD2`, bit 3 = 0) |
| application | erased — reflash it with the Davis updater |
| lock bits | reset to `0xFF` (unlocked); we do not set them anyway |

The sequence that works and has been tested:

1. chip erase plus bootloader write;
2. the Davis updater flashes the application through the bootloader.

A single page cannot be erased over ISP — the ATmega128 serial programming
instruction set only offers Chip Erase. The alternative is to merge the
application image and the bootloader into one file and write it in one pass.

### Case B — blank part, flashing application and bootloader

```bash
# 1. fuses first (example for Envoy/VP2)
avrdude -c <programmer> -p m128 -U lfuse:w:0xFD:m -U hfuse:w:0xD4:m -U efuse:w:0xFF:m
# 2. application
avrdude -c <programmer> -p m128 -U flash:w:VP2_3.88.bin:r
# 3. bootloader, without erasing
avrdude -c <programmer> -p m128 -D -U flash:w:davis_boot_VP2.hex:i
```

### Verifying afterwards

```bash
avrdude -c <programmer> -p m128 -U flash:r:dump.bin:r
```

Check that the bytes from the start of the boot section are no longer `0xFF`
and that the application area is unchanged.

---

## 12. The trap that cost the most: PROGMEM above 64 KB

**This was a real bug found on hardware.** Worth knowing before touching this
code.

The boot section sits at 0x1F000 (Vue) / 0x1F800 (VP2), i.e. **above 64 KB**.
Pointers on AVR are 16-bit, and `pgm_read_byte()` / `pgm_read_word()` compile to
**`LPM`, which uses only the 16-bit Z register and ignores RAMPZ**. Every such
read from the boot section lands `0x10000` too low — that is, **inside the
application area**.

The CRC table was declared `PROGMEM` and read with `pgm_read_word()`. The linker
placed it at `0x1F08C`; the code read from `0xF08C`. In a disassembly it looks
like this:

```
subi r30, 0x74      ; -0x0F74 = +0xF08C   <-- 0x10000 missing
sbci r31, 0x0F
lpm  r18, Z+
lpm  r19, Z
```

**Why it stayed hidden.** With an empty application section, `app_valid()`
returns early on `pgm_read_word_far(0) == 0xFFFF` and the CRC loop never runs.
The bug only surfaced once firmware was flashed: the bootloader computed `AB12`
instead of the `D795` in the footer and therefore never handed over to a
perfectly good application. The `A` command still worked, because `start_app()`
bypasses the check — which made it look like a bootloader problem rather than a
CRC problem.

**It affected both platforms** — the VP2 boot section at 0x1F800 is also above
64 KB.

### The fix

The CRC table lives in **RAM** (`static const`, no `PROGMEM`). It costs 32 B of
SRAM out of 4096 and is faster than `LPM` anyway.

For reading flash, use **only the `_far` variants**, which set RAMPZ:

| allowed | not allowed |
|---|---|
| `pgm_read_byte_far(a)` | `pgm_read_byte(p)` |
| `pgm_read_word_far(a)` | `pgm_read_word(p)` |

### Automatic guard

`make check` verifies that the finished image contains **no `LPM` instruction
at all** — only `ELPM`. The startup `.data` copy on the ATmega128 also uses
`ELPM`, so a threshold of zero is correct and any `LPM` means a bug of this
class.

---

## 13. Startup greeting

After a **real reset** the bootloader identifies itself on the serial port:

```
Davis serial bootloader for Vantage Vue 6351
built by byq with Claude (Anthropic)
APP START
```

`APP START` is printed only on the automatic hand-over, immediately before
jumping to the application. If the bootloader stays in command mode - no
application, or a failing CRC - the greeting appears alone.

The VP2 build names the models it covers:

```
Davis serial bootloader for Vantage Pro2 6312 / 6152C, Weather Envoy 6316 / 6316C
```

**On the `BOOT` path the bootloader emits nothing at all.** That is not a
detail: the updater is already mid-conversation there, and any unsolicited byte
desynchronises it. The guard is `MCUCSR != 0`, the same discriminator used for
clock trimming (section 7) - a zero `MCUCSR` means the application jumped in.

The `A` command is silent for the same reason: the updater sends it at the end
of an update, and the reference captures show no reply.

One consequence worth knowing: at power-up the greeting goes out before the
application starts. At 19200 the Vue line takes about 45 ms and the VP2 line
about 65 ms. If the updater happens to open the port during that window it will
see those bytes. In practice the CRC check that follows takes 2-3 s, so there is
ample separation - but power-cycling a console with the updater already running
is worth avoiding.

Build with `make GREETING=0` for a completely silent bootloader.

Cost of the greeting: 174 B on VP2, 148 B on Vue.

---

## 13a. Diagnostic banner

**`make` builds silently by default.** The banner has three levels because it
interferes with the protocol: confirmed live, the updater received
`\r\nRX 49 -> 6351B` in reply to `ID\n` instead of a bare `6351B` and lost its
place. `CLOCK_TRIM` is independent of `BANNER` and stays enabled in every
build; without it the Vue UART does not work.

Level 1 is the compromise — short enough that the updater has a chance of
tolerating it, while still explaining why the bootloader did or did not hand
over:

```
BOOTLOADER 6351B
CRC D795=D795 M01 APP
    │    │ │    │   └─ decision: APP = start the application, CMD = command mode
    │    │ │    └─ MCUCSR (01 = POR, 02 = EXT, 08 = WDT, 00 = jumped from the app)
    │    │ └─ CRC stored in the image footer
    │    └─ '=' match, '!' mismatch
    └─ CRC computed from flash contents
```

`CRC NONE` means the reset vector is 0xFFFF — there is no application at all.
`CRC SKIP` means the decision was already made and the CRC was not computed.

Level 2 adds a full header, a raw echo of every received command byte, and a
calibration burst:

```
=== DAVIS BOOT 6351B ===
SEC  1F000  VER 14 03
UART 1  UBRR 000C  U2X 1  BAUD 19200
CLK  1950 / 1950  F=1996 kHz  OSCCAL B7
MCUCSR 01 = POR
CAL  UUUUUUUUUUUUUUUU…          (~2 s of continuous 0x55)
CRC  checking... D795 == D795  OK
-> STARTING APPLICATION
```

### Measuring the real clock with `CAL`

`'U'` = `0x55` = alternating bits. An 8N1 frame on the wire:

```
start d0 d1 d2 d3 d4 d5 d6 d7 stop | start …
  0   1  0  1  0  1  0  1  0   1   |   0
```

A continuous stream of `'U'` therefore produces a **clean square wave on TXD
whose half period equals exactly one bit time**. A multimeter in frequency mode
reads `baud/2`, nominally **9600 Hz** on both platforms. Hence:

```
baud  = 2 × f_measured
F_CPU = baud × 8  × (UBRR+1)    at U2X = 1   →  Vue:  F_CPU = f × 208
F_CPU = baud × 16 × (UBRR+1)    at U2X = 0   →  VP2:  F_CPU = f × 192
```

Example: a meter reading 8800 Hz on a Vue gives `F_CPU = 8800 × 208 = 1.83 MHz`,
i.e. the RC is 8.5 % low. The burst lasts about 2 s because that is what a
typical multimeter needs to settle; the `U` command repeats it without a reset.

### Reading the RX echo

```
RX 42 -> <06> <14> <03>          after typing 'B' + Enter
```

If typing `B` comes back as `RX 42`, the receiver is fine. Anything else means
the byte arrived corrupted, i.e. the clock is still wrong.

### The banner changes port behaviour

It is emitted on **every** start, including an ordinary power-up before control
passes to the application, so both the application and the Davis updater will
see those bytes. Rebuild with plain `make` for normal operation.

---

## 14. Test plan

Order matters — each step assumes the previous one passed.

1. **Banner.** Power up with a terminal on 19200 8N1 and a `BANNER=2` build.
   - clean text → the UART works, continue
   - garbage → the bootloader started but the clock is off; measure `CAL`
   - silence → wrong port, or the bootloader did not start
2. **Prompt.** Send `\n`. Expected: `!`
3. **ID.** `ID\n` → `6312B` (or `6351B`).
4. **BVER.** `B\n` → `06 11 03`. After the application starts, `BVER\n` must
   give `11 03` — that verifies the byte at 0x1FFFD really got written.
5. **Start the application.** `A\n` → the console should come up normally.
6. **Power cycle.** A console with a valid application should come up on its
   own without stopping at `!`. If it stops, the CRC check is failing.
7. **Entry from the application.** `BOOT\n` → `06`, then `ID\n` → `6351B`.
   If `ID` gives `6351` or silence, see section 7.
8. **Full update.** Run the stock updater against a known package and compare
   the exchange with the reference capture.

---

## 15. Open risks

### 15.1 Hardware behaviour during programming — the main risk

A full update takes about six minutes. It is not known whether the console has:

- a power latch that needs holding from software,
- a watchdog enabled by the `WDTON` fuse (the code calls `wdt_reset()` in its
  loops, but whether often enough is unverified),
- requirements around refreshing the LCD.

None of this can be extracted from a capture or a firmware image — it lives in
the schematic or in the original bootloader, which cannot be read out because
of the lock bits. **This is the one place where this code could plausibly
damage a unit mid-update.** Run the first full test on a console you can afford
to lose.

### 15.2 Startup time

The CRC check reads ~127 KB of flash, taking 2–3 s at 2 MHz on every power-up.
If that becomes a problem, write a marker into EEPROM after a successful start
and compute the full CRC only when the marker is absent. `EESAVE` is
programmed, so the marker survives a chip erase.

### 15.3 The footer format is verified on one Vue version

Three images across two platforms follow the same rule, but only one of them is
a Vue. `tools/check_image.py` checks any further image in one command.

### 15.4 There is deliberately no timeout anywhere

`uart_getc()` spins on `RXC` forever. That applies equally to a command, to the
rest of a line, and to the 256 data bytes after `F`. This is a deliberate
choice, not an oversight.

The `F` transfer counts exactly 256 bytes with no framing of any kind, because
that is what the protocol provides. A single byte lost or duplicated in transit
desynchronises the count **permanently**: the following `P <n>\n` is swallowed
as page data, and so is everything after it. The bootloader then waits forever
for the remainder of a page that will never arrive, which from the outside looks
exactly like a hang at a random page number.

Adding a timeout that recovered to the command loop would be worse. The updater
has already moved on, so its next `P <n>` would be parsed as a valid command and
**a page would be programmed from a partially filled buffer** - silently, with
no error. The update would appear to succeed while leaving a corrupted
application. Hanging is ugly but honest: it cannot produce a false success. A
power cycle recovers the console, and the CRC check then refuses to start the
damaged application.

Observed in practice: updates over a TCP-to-serial bridge stalled around page
300 of 496, while the same update over direct RS-232 completed reliably. TCP
itself does not lose data, but the serial side of such a bridge has no flow
control - if its buffer overruns while forwarding, bytes vanish silently. The
fault was on the bridge, not in the bootloader.

The one window where the bootloader is not reading the port is `page_program()`:
page erase plus page write, about 9 ms, during which roughly 17 byte times pass
at 19200 while the USART receive FIFO holds only two. The updater waits for the
ACK, so nothing should arrive then - but a bridge flushing buffered data can
still lose bytes there.

### 15.5 ACK ordering on `F`

The capture shows `06 06` inside a single TCP packet, so it is not visible
whether the first ACK follows the `F\n` command or only arrives after all 256
bytes. As implemented: ACK after the command, second ACK after the data. If an
updater stalls at this point, swap the order.

### 15.6 The `C` command may do more than we do

In the captures the stock bootloader takes 256 ms (Vue, application present) and
1153 ms (VP2, blank flash) to answer `C`. Those differ four-fold, which a fixed
number of page erases cannot explain, so the original evidently does something
more. Our implementation erases only the page holding the footer, which is
enough to stop an interrupted update from leaving an image that looks valid.

---

## 16. What not to do

- **Do not chip-erase a console that has a working original bootloader.** The
  bootloader is not part of the update package and cannot be read out (lock
  bits), so erasing it is irreversible.
- Do not set `LB = 0xFC` before testing is finished.
- Do not use avrdude without `-D` when adding only the bootloader to a console
  that has an application.
- Do not assume the VP2 has a 4 KiB boot section like the Vue — it has 2 KiB.
- **Do not assume the Vue talks over USART0** — it uses USART1 exclusively.
  Run `make check` after any change to the UART code; it checks this
  automatically.
- Do not cache anything in `.noinit` across a run of the application, and do
  not use `pgm_read_byte()` / `pgm_read_word()` anywhere in this project.

---

## 17. Files

| file | description |
|---|---|
| `davis_boot.c` | bootloader source, one file for both platforms |
| `Makefile` | `make`, `make BANNER=1`, `make BANNER=2`, `make size`, `make check` |
| `davis_boot_VP2.hex` | build for the 2 KiB boot section @ 0x1F800, USART0 |
| `davis_boot_VUE.hex` | build for the 4 KiB boot section @ 0x1F000, USART1 |
| `VP2.lss`, `VUE.lss` | disassembly with section headers |
| `tools/check_image.py` | verifies a firmware image against the bootloader's assumptions |

The VP2 application images used for verification were reconstructed from the
`F` frames in the captures and checked against the footer CRC. A Vue image
reconstructed the same way was byte-identical to one supplied independently,
which validates the extraction method.
