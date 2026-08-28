# WXBootloader

A serial bootloader for Davis weather consoles built around an ATmega128.

It puts a console back under the control of the stock Davis updater. The main
case is a console whose firmware was written with a programmer and which
therefore has **no bootloader at all** — the boot section is blank and the
updater has nothing to talk to.

The protocol, the memory layout and the platform differences were reverse
engineered from captured update sessions and by disassembling the stock
firmware images. `NOTES.md` documents all of it, including the wrong turns.

## Supported consoles

| model | boot section | serial port |
|---|---|---|
| Vantage Vue 6351 | 4 KiB @ 0x1F000 | USART1 (PD2/PD3) |
| Vantage Pro2 6312 | 2 KiB @ 0x1F800 | USART0 (PE0/PE1) |
| Cabled Vantage Pro2 6152C | 2 KiB @ 0x1F800 | USART0 |
| Weather Envoy 6316 | 2 KiB @ 0x1F800 | USART0 |
| Cabled Weather Envoy 6316C | 2 KiB @ 0x1F800 | USART0 |

19200 baud, 8N1.

## Status

Tested on hardware, on a Vantage Vue 6351:

- the console starts its application on its own after power-up
- the stock Davis updater flashes firmware on a blank flash
- the stock Davis updater flashes firmware from the running application, via
  the `BOOT` command

The VP2 / Envoy build compiles and passes the same checks, but has not been
run on hardware.

## Building

Needs `gcc-avr`, `avr-libc` and `binutils-avr`. On Debian or Ubuntu:

```bash
sudo apt install gcc-avr avr-libc binutils-avr
```

Then:

```bash
make                        # both platforms
make check                  # sanity checks - run this after any change
make size                   # flash usage against the boot section limit
```

Build variants:

```bash
make GREETING=0             # completely silent
make BANNER=1               # one diagnostic line explaining the boot decision
make BANNER=2 GREETING=0    # full diagnostics: header, RX echo, clock measurement
```

Reference sizes with avr-gcc 7.3.0:

| build | VP2 / Envoy | Vue |
|---|---|---|
| `make` | 1196 B / 2045 B | 1496 B / 4093 B |
| `make BANNER=2 GREETING=0` | 1960 B / 2045 B | 2696 B / 4093 B |

Prebuilt `davis_boot_VP2.hex` and `davis_boot_VUE.hex` are in the repository.

## Flashing

The boot section on a console without a bootloader is already blank, so it can
be written without erasing anything else. **`-D` is essential** — it disables
avrdude's automatic chip erase, which would otherwise wipe the application:

```bash
avrdude -c <programmer> -p m128 -D -U flash:w:davis_boot_VUE.hex:i
```

Then check that fuse H is right. On a stock console it already is, and in that
case **do not touch the fuses at all**.

| console | fuse H |
|---|---|
| Vantage Vue | `0xD2` |
| Vantage Pro2 / Envoy | `0xD4` |

The value matters more than it looks: the stock application reads the High
fuse itself and refuses to hand over to the bootloader unless it matches
exactly. A console with a drifted H fuse answers `BOOT` with `!` and then stops
responding until it is reset.

## What it does at startup

After a real reset the bootloader identifies itself and then either starts the
application or waits for commands:

```
Davis serial bootloader for Vantage Vue 6351
built by byq with Claude (Anthropic)
APP START
```

When the application hands over via its `BOOT` command it prints **nothing** —
the updater is already mid-conversation at that point and any unsolicited byte
would desynchronise it.

The application is started only if its CRC-16/XMODEM footer checks out, so a
console left with a half-written firmware image stays recoverable.

## Warnings

- **Never chip-erase a console that has a working original bootloader.** The
  original is not part of any update package and cannot be read out, because
  the lock bits block it. Erasing it is irreversible.
- **Run the first full update on a console you can afford to lose.** A complete
  update takes about six minutes. Whether the hardware needs a power latch held
  from software, or the LCD refreshed during that time, is not known — it is
  not visible in any capture or firmware image. This is the one place where
  this code could plausibly damage a unit.
- Do not set the lock bits while testing; they block verification.
- Do not assume the Vue uses USART0 — it uses USART1 exclusively. `make check`
  verifies this automatically.

## Tools

```bash
./tools/check_image.py firmware.bin
```

Checks a Davis firmware image against the three assumptions the bootloader
relies on: the image is exactly the size of the application section, the last
two bytes are a big-endian CRC-16/XMODEM of everything before them, and the
reset vector is not blank.

Verified against three real images (Vue 4.30, VP2 3.80, VP2 3.88), but only one
of those is a Vue — running it against further images is genuinely useful.

## Documentation

`NOTES.md` is the real documentation. Among other things it covers:

- how the application decides whether to hand over, and what it checks first
- why the Vue needs its internal RC trimmed against the watch crystal before
  the UART works at all
- the PROGMEM trap for boot sections above 64 KB, which cost the most time here
- why there is deliberately no receive timeout
- two optimisations that looked obviously correct and were reverted

## Licence

MIT — see `LICENSE`.

Developed by byq13 with Claude (Anthropic). Not affiliated with or endorsed by
Davis Instruments.
