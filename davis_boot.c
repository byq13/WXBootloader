/* =====================================================================
 * Serial bootloader for Davis weather consoles based on ATmega128
 *
 * Protocol reconstructed from captured update sessions
 * (Vue 4.30, VP2 3.80, VP2 3.88) and confirmed against a console
 * running the bootloader alone, with no application present:
 *
 *   '\n' / '\r'          ->  '!'                    prompt
 *   'I'   (ID\n)         ->  "6312B"                model + 'B', no CR/LF
 *   'B'   (BVER\n)       ->  06 <lo> <hi>           ACK + 2 version bytes
 *   'C'                  ->  06                     invalidate application
 *   'F'  + 256 bytes     ->  06 ... 06              ACK command, ACK data
 *   'P' <n> '\n'         ->  06                     program page n
 *   'A'                  ->  (nothing)              jump to 0x0000
 *
 * Only the FIRST letter of a command is decoded; the rest of the line up
 * to '\n' is skipped.  Confirmed in the captures: "BVER\n" and "B\n"
 * produce identical replies.  An unknown command answers with the prompt
 * (also confirmed: the application-only command "VER" returns '!').
 *
 * Build (VP2 / Envoy, 2 KiB boot section):
 *   avr-gcc -mmcu=atmega128 -Os -DPLATFORM_VP2 \
 *           -Wl,--section-start=.text=0x1F800 -o davis_boot.elf davis_boot.c
 *   avr-objcopy -O ihex davis_boot.elf davis_boot.hex
 *   avr-size davis_boot.elf          # .text+.data must fit in 2048 B
 *
 * Vue (4 KiB boot section):
 *   -DPLATFORM_VUE -Wl,--section-start=.text=0x1F000
 *
 * Or simply use the Makefile.
 *
 * Fuses:
 *   VP2 / Envoy   H = 0xD4   (BOOTSZ=10 -> 2 KiB, BOOTRST programmed)
 *   Vue           H = 0xD2   (BOOTSZ=01 -> 4 KiB, BOOTRST programmed)
 *
 * These values are not a guess.  The stock application reads the High
 * fuse and refuses to hand over to the bootloader unless it matches
 * exactly - see the note above the "BOOT" entry detection in main().
 *
 * Lock bits must leave the boot section permission to write the
 * application section (BLB01/BLB02 unprogrammed).  Do not set LB=0xFC
 * until testing is complete - it blocks verification.
 * ===================================================================== */

#include <avr/io.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * PLATFORM CONFIGURATION
 * ------------------------------------------------------------------- */

#if defined(PLATFORM_VUE)
  #define BOOT_START   0x1F000UL      /* 4 KiB, BOOTSZ = 01           */
  #define MODEL_ID     "6351B"        /* Vantage Vue                  */
  #define BVER_B0      0x14           /* observed: 14 03              */
  #define UART_PORT    1              /* USART1 - RXD1/TXD1 = PD2/PD3 */
  #define F_CPU_HZ     2000000UL      /* internal RC, CKSEL = 0010    */
  #define UART_U2X     1              /* Vue derives baud with U2X=1  */
  #define CLOCK_TRIM   1              /* RC needs trimming, see below */
  #define GREETING_MODELS "Vantage Vue 6351"
#else                                  /* PLATFORM_VP2 (Envoy too)     */
  #define BOOT_START   0x1F800UL      /* 2 KiB, BOOTSZ = 10           */
  #define MODEL_ID     "6312B"        /* Vantage Pro2                 */
  #define BVER_B0      0x11           /* observed: 11 03              */
  #define UART_PORT    0              /* USART0 - RXD0/TXD0 = PE0/PE1 */
  #define F_CPU_HZ     1843200UL      /* crystal                      */
  #define UART_U2X     0              /* VP2 leaves U2X alone         */
  #define CLOCK_TRIM   0              /* VP2 has a crystal            */
  #define GREETING_MODELS "Vantage Pro2 6312 / 6152C, Weather Envoy 6316 / 6316C"
#endif

#define BVER_B1        0x03           /* constant on both platforms   */

/* ---------------------------------------------------------------------
 * STARTUP GREETING
 *
 * Printed once after a REAL reset, and never when the application jumps
 * in via its BOOT command - on that path the updater is already talking
 * and any unsolicited byte desynchronises it.  The discriminator is the
 * same one used for clock trimming: a zero MCUCSR means no reset
 * happened, so we got here by a jump.  See the note in main().
 *
 * "APP START" is emitted only on the automatic hand-over after a reset.
 * The 'A' command deliberately stays silent, because that is what the
 * updater sends at the end of an update and the reference captures show
 * no reply to it.
 *
 * Build with -DBOOT_GREETING=0 (or "make GREETING=0") for a completely
 * silent build.  That is also how BANNER=2 is built for VP2/Envoy: the
 * 2 KiB boot section cannot hold both the greeting and full diagnostics,
 * and the linker refuses rather than producing a broken image.
 * ------------------------------------------------------------------- */
#ifndef BOOT_GREETING
  #define BOOT_GREETING  1
#endif

#define GREETING       "\r\nDavis serial bootloader for " GREETING_MODELS \
                       "\r\nbuilt by byq with Claude (Anthropic)\r\n"

/* uart_flush() and the TXC handling in uart_putc() are needed whenever
 * anything is printed before jumping to the application - otherwise
 * start_app() disables the transmitter mid-byte and truncates the last
 * line.                                                                */
#if BOOT_GREETING || BOOT_BANNER >= 1
  #define NEED_TXC     1
#else
  #define NEED_TXC     0
#endif

/* ---------------------------------------------------------------------
 * VERSION BYTE READ BY THE APPLICATION
 *
 * Established by disassembling the Vue 4.30 and VP2 3.88 images - the
 * code is identical in both:
 *
 *     ldi  r30, 0xFD
 *     ldi  r31, 0xFF          ; Z = 0xFFFD
 *     ldi  r19, 0x01
 *     out  0x3B, r19          ; RAMPZ = 1  ->  address 0x1FFFD
 *     elpm r16, Z             ; one byte from the boot section
 *     rcall <send byte>
 *     ldi  r16, 0x03          ; second byte: CONSTANT IN THE APPLICATION
 *     rjmp <send byte, return>
 *
 * So the application's BVER command reads EXACTLY ONE byte from
 * 0x1FFFD.  Without that byte the application returns 0xFF; its code has
 * no guard against an empty boot section.  Note the address is the same
 * on both platforms despite the different boot section sizes.
 *
 * Linking:  -Wl,--section-start=.bootver=0x1FFFD
 * ------------------------------------------------------------------- */
const uint8_t bootver __attribute__((section(".bootver"), used)) = BVER_B0;

/* ---------------------------------------------------------------------
 * SERIAL PORT AND BAUD RATE
 *
 * Established from the interrupt vector tables and from disassembling
 * the baud-rate routines.  The two platforms differ, and getting this
 * wrong makes the bootloader sit silently on a pin nothing is connected
 * to - which is exactly what happened on the first attempt with Vue.
 *
 * VP2 (routine at 0x1B7F6 in image 3.88) picks the USART at runtime
 * from a flag in SRAM 0x0892:  "sbrs r17,1" selects either "out UBRR0L"
 * or "sts UBRR1L", both with the same value.  Vectors USART0_RX (0x48)
 * and USART1_RX (0x78) are both populated.
 *
 * Vue (routine at 0x1BF78 in image 4.30) writes UBRR1L ONLY.  Vectors
 * USART0_RX / USART0_UDRE / USART0_TX are empty (0xFF); only USART1_RX
 * is populated.  Vue does not use USART0 at all.
 *
 * U2X matters.  Vue sets U2X1 = 1 on entry to the baud routine:
 *
 *     sts  0x0098, r17     ; UBRR1H = 0
 *     lds  r17, 0x009B     ; UCSR1A
 *     ori  r17, 0x02       ; U2X1 = 1
 *     sts  0x009B, r17
 *
 * With U2X = 1 the divisor is 8, not 16, and the whole UBRR ladder
 * resolves at a 2 MHz clock - consistent with fuse L = 0xE2
 * (CKSEL = 0010, internal RC 2 MHz):
 *
 *     103 + U2X=0 -> 1200    (the only branch that clears U2X: andi 0xFD)
 *     103         -> 2400
 *      51         -> 4800
 *      25         -> 9600
 *      16         -> 14400  (+2.1 %)
 *      12         -> 19200  (+0.16 %, the default branch)
 *
 * VP2 has the same six-entry list (95/47/23/11/7/5) on its 1.8432 MHz
 * crystal with U2X = 0; its routine never touches U2X.
 *
 * Rounding to nearest, not down:
 *   VP2  U2X=0  (1843200 + 8*19200) / (16*19200) - 1 =  5   (error 0.00 %)
 *   Vue  U2X=1  (2000000 + 4*19200) / ( 8*19200) - 1 = 12   (error 0.16 %)
 * ------------------------------------------------------------------- */

#define BAUD_RATE      19200UL
#define BAUD_TXT       "19200"

#if UART_U2X
  #define UBRR_VAL     ((F_CPU_HZ + 4UL * BAUD_RATE) / (8UL * BAUD_RATE) - 1UL)
#else
  #define UBRR_VAL     ((F_CPU_HZ + 8UL * BAUD_RATE) / (16UL * BAUD_RATE) - 1UL)
#endif

#if UART_PORT == 1
  #define UBRRH_REG    UBRR1H
  #define UBRRL_REG    UBRR1L
  #define UCSRA_REG    UCSR1A
  #define UCSRB_REG    UCSR1B
  #define UCSRC_REG    UCSR1C
  #define UDR_REG      UDR1
#else
  #define UBRRH_REG    UBRR0H
  #define UBRRL_REG    UBRR0L
  #define UCSRA_REG    UCSR0A
  #define UCSRB_REG    UCSR0B
  #define UCSRC_REG    UCSR0C
  #define UDR_REG      UDR0
#endif

/* Bit positions are identical in both USARTs (ATmega128 datasheet,
 * UCSRnA / UCSRnB / UCSRnC tables).  Numbers are used instead of the
 * avr-libc names because the USART1 names carry a '1' suffix and mixing
 * them up compiles cleanly while producing a silent port.            */
#define BIT_RXC        7              /* UCSRnA */
#define BIT_UDRE       5              /* UCSRnA */
#define BIT_U2X        1              /* UCSRnA */
#define BIT_TXC        6              /* UCSRnA */
#define BIT_RXEN       4              /* UCSRnB */
#define BIT_TXEN       3              /* UCSRnB */
#define BIT_UCSZ1      2              /* UCSRnC */
#define BIT_UCSZ0      1              /* UCSRnC */

/* ---------------------------------------------------------------------
 * WHEN THE BOOTLOADER KEEPS CONTROL
 *
 * BOOTRST is programmed, so EVERY reset lands here, including an
 * ordinary power-up.  The bootloader therefore has to decide for itself
 * whether to hand over to the application.  It stays in command mode
 * when any of these holds:
 *
 *   1. arrived by a jump rather than a reset  -> the application's
 *      "BOOT" command; see the note in main()
 *   2. the reset came from the watchdog (WDRF)
 *   3. the application fails its CRC check
 *
 * Condition 1 is the one that makes the "BOOT" command work at all, and
 * condition 3 means a console without a valid application is always
 * recoverable.
 *
 * An earlier design also looked for a magic value in a .noinit SRAM
 * byte.  That was REMOVED: the application owns the whole SRAM, so
 * after it has run that byte holds its data, and nothing ever wrote the
 * magic value there - the test could only ever produce false positives,
 * each one a console that refuses to boot on its own.  If a
 * "application requests the bootloader" signal is ever needed, EEPROM
 * is the only carrier that survives (EESAVE is programmed).
 * ------------------------------------------------------------------- */

#define PAGE_SIZE      SPM_PAGESIZE            /* 256 on ATmega128      */
#define APP_PAGES      (BOOT_START / PAGE_SIZE)/* 496 (Vue) / 504 (VP2) */
#define CRC_ADDR       (BOOT_START - 2)        /* footer, big-endian    */

#define ACK            0x06
#define PROMPT         '!'

static uint8_t page_buf[PAGE_SIZE];

#if CLOCK_TRIM

/* =====================================================================
 * TRIMMING THE INTERNAL RC AGAINST THE 32768 Hz CRYSTAL
 *
 * Vue has no crystal clocking the CPU - it runs on the internal RC
 * oscillator, which after reset can sit several percent off nominal.
 * With U2X = 1 the receiver samples 8x instead of 16x, so the AVR
 * receiver tolerance narrows to 94.12 % .. 105.66 %; a few percent is
 * enough to kill the link.
 *
 * Observed on real hardware: in a continuous stream of 'U' (0x55) the
 * receiver occasionally saw 0xD5 - that is 0x55 with ONLY bit d7 wrong.
 * That is the signature of a transmitter running roughly +6 % fast: the
 * sampling point for the last data bit has drifted into the stop bit.
 *
 * The Davis firmware solves this with a loop that trims OSCCAL against
 * the 32768 Hz watch crystal on TOSC (visible at 0x14610 in image 4.30).
 * Reading the factory calibration byte from the signature row is NOT
 * possible in software on ATmega128 - there is no SIGRD bit in SPMCSR -
 * so measuring against the crystal is the only route.
 *
 * Method: Timer0 in asynchronous mode counts crystal ticks while Timer1
 * counts CPU cycles.  Cycles per TRIM_TICKS crystal ticks gives the
 * clock:
 *
 *     F_CPU = count * 32768 / TRIM_TICKS = count * 1024   (TICKS = 32)
 *
 * The target is chosen so that UBRR = 12 with U2X = 1 yields EXACTLY
 * 19200 baud:
 *
 *     F_target = 19200 * 8 * (12+1) = 1 996 800 Hz
 *     target   = 32 * 1996800 / 32768 = 1950 cycles  (exact, no remainder)
 *
 * Measurement resolution is 1/1950 = 0.05 %, well below one OSCCAL step.
 * The whole 0..255 range is scanned and the best match taken - OSCCAL is
 * not reliably monotonic, so a binary search can land on a wrong value.
 * A binary search was tried and reverted for exactly that reason.
 * ===================================================================== */

#define TRIM_TICKS     32             /* crystal ticks per measurement  */
#define TRIM_TARGET    1950           /* CPU cycles = 1 996 800 Hz      */

/* Last measurement, for the diagnostic banner.  0 = crystal not running. */
static uint16_t trim_count;
static uint8_t  trim_done;

/* ---------------------------------------------------------------------
 * WHY THE RESULT IS NOT CACHED IN .noinit
 *
 * It is tempting to remember the OSCCAL value in .noinit and skip the
 * scan on the next entry into the bootloader.  That is WRONG and was
 * reverted after testing on hardware.
 *
 * .noinit survives a reset, but it does NOT survive running the
 * application.  The application is a separate program that owns the
 * whole SRAM - our variables sit in the middle of its data and stack.
 * After returning from it they hold its leftovers, not our values.
 *
 * Worse, those bytes are DETERMINISTIC (same application, same code
 * path), so a validity check either passes every time or never - this
 * is not a 1-in-65536 risk but a reproducible failure.  Symptom: the
 * bootloader writes garbage into OSCCAL, detunes the clock and stops
 * responding to commands entirely.
 * ------------------------------------------------------------------- */

/* CPU cycles per TRIM_TICKS crystal ticks.  0 if the crystal is stopped. */
static uint16_t trim_measure(void)
{
    uint8_t  t0;
    uint16_t guard;

    /* Sync to a crystal edge.  One tick is 30.5 us, i.e. a few dozen CPU
     * cycles, so a 16-bit guard has enormous margin; it exists only to
     * detect that there is no crystal at all.                          */
    t0 = TCNT0;
    for (guard = 0; TCNT0 == t0; guard++) {
        wdt_reset();
        if (guard == 0xFFFF)
            return 0;
    }

    TCNT1 = 0;
    t0 = TCNT0;

    /* The guard is REQUIRED here too.  Without it, a crystal that
     * started, ticked once and then stalled during warm-up would hang
     * the bootloader forever - this loop runs before uart_init(), so the
     * console would answer nothing at all.  TRIM_TICKS ticks is about
     * 1 ms, so a full 16-bit count has enormous margin.                */
    for (guard = 0; (uint8_t)(TCNT0 - t0) < TRIM_TICKS; guard++) {
        wdt_reset();
        if (guard == 0xFFFF)
            return 0;
    }

    return TCNT1;
}

/* Deviation of a measurement from the target, in CPU cycles. */
static uint16_t trim_err(uint16_t m)
{
    return (m > TRIM_TARGET) ? (m - TRIM_TARGET) : (TRIM_TARGET - m);
}

static void clock_trim(void)
{
    uint8_t  cal, best_cal, entry_cal;
    uint16_t m, err, best_err, n;
    uint32_t guard32;

    entry_cal = OSCCAL;                    /* restored on any abort     */

    /* Timer0 from the 32768 Hz crystal on TOSC, no prescaler.          */
    ASSR  = (1 << AS0);
    TCCR0 = (1 << CS00);

    /* The *UB flags only clear once the write has been synchronised into
     * the 32 kHz clock domain - that REQUIRES a ticking crystal.  This
     * loop runs before uart_init(), so without a guard it hangs the
     * bootloader before it can say anything.
     *
     * The limit is DELIBERATELY generous (~1 s at 2 MHz).  It is not
     * there to bound latency - a crystal that was running moments ago is
     * still ringing mechanically and restarts immediately.  It is there
     * so that a stopped crystal can never produce a hang.              */
    for (guard32 = 0; ASSR & ((1 << TCN0UB) | (1 << OCR0UB) | (1 << TCR0UB));
         guard32++) {
        wdt_reset();
        if (guard32 == 300000UL)
            goto abort;
    }

    /* Timer1 from the CPU clock, no prescaler - a plain cycle counter. */
    TCCR1A = 0;
    TCCR1B = (1 << CS10);

    /* No crystal at all: leave OSCCAL alone and return.  The bootloader
     * will run on whatever calibration it has.                         */
    if (trim_measure() == 0)
        goto abort;

    /* The watch crystal starts slowly - wait a fixed ~0.25 s.
     *
     * Replacing this with a convergence test ("three consecutive
     * measurements agreeing to within one cycle") was tried and
     * REVERTED.  A crystal drifting slowly up to its final frequency
     * satisfies that condition easily - one cycle in 1950 is 0.05 % -
     * while still being percent away, so the trim locked onto a wrong
     * reference.                                                        */
    for (n = 0; n < 8192 / TRIM_TICKS; n++) {
        if (trim_measure() == 0)
            goto abort;
    }

    /* Full 0..255 scan.  Slower than a binary search but immune to
     * OSCCAL non-monotonicity, and proven on hardware.                 */
    best_err = 0xFFFF;
    best_cal = entry_cal;

    for (cal = 0; ; cal++) {
        OSCCAL = cal;
        m = trim_measure();
        if (m == 0)
            goto abort;
        err = trim_err(m);
        if (err < best_err) {
            best_err = err;
            best_cal = cal;
        }
        if (cal == 0xFF)
            break;
    }

    OSCCAL     = best_cal;
    trim_count = trim_measure();

    /* Final gate: if the result is still far from target, the factory
     * calibration is a better bet than whatever we found.  2 % of target
     * is 39 cycles, the edge of usable UART timing at U2X = 1.         */
    if (trim_count == 0 || trim_err(trim_count) > 39) {
        OSCCAL = entry_cal;
        goto abort;
    }

    trim_done = 1;

abort:
    if (!trim_done)
        OSCCAL = entry_cal;

    /* Leave the timers idle for the application to configure. */
    TCCR0  = 0;
    TCCR1B = 0;
    ASSR   = 0;
}

#endif /* CLOCK_TRIM */

/* =====================================================================
 * UART  (USART0 on VP2/Envoy, USART1 on Vue - see UART_PORT)
 * ===================================================================== */

static void uart_init(void)
{
    UBRRH_REG = (uint8_t)(UBRR_VAL >> 8);
    UBRRL_REG = (uint8_t)(UBRR_VAL);
    UCSRA_REG = (UART_U2X << BIT_U2X);               /* see UART_U2X    */
    UCSRC_REG = (1 << BIT_UCSZ1) | (1 << BIT_UCSZ0); /* 8N1             */
    UCSRB_REG = (1 << BIT_RXEN) | (1 << BIT_TXEN);   /* polled, no IRQ  */
}

static uint8_t uart_getc(void)
{
    while (!(UCSRA_REG & (1 << BIT_RXC)))
        wdt_reset();                           /* in case WDTON is set  */
    return UDR_REG;
}

static void uart_putc(uint8_t c)
{
    while (!(UCSRA_REG & (1 << BIT_UDRE)))
        wdt_reset();
#if NEED_TXC
    /* Writing a one to TXC clears the flag while preserving U2X, so
     * uart_flush() waits for the ACTUAL end of transmission rather than
     * a stale flag from an earlier byte.  Needed only by the diagnostic
     * banner - the production path is left exactly as it was.          */
    UCSRA_REG = (UART_U2X << BIT_U2X) | (1 << BIT_TXC);
#endif
    UDR_REG = c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc((uint8_t)*s++);
}

#if NEED_TXC

/* Waits until the last byte has left the shift register.  Call ONLY
 * right after transmitting something - otherwise TXC is clear and this
 * loop never exits.                                                    */
static void uart_flush(void)
{
    while (!(UCSRA_REG & (1 << BIT_TXC)))
        wdt_reset();
}

#endif /* NEED_TXC */

#if BOOT_BANNER >= 1

static void put_nib(uint8_t n)
{
    n &= 0x0F;
    uart_putc(n < 10 ? (uint8_t)('0' + n) : (uint8_t)('A' + n - 10));
}

static void put_hex8(uint8_t v)
{
    put_nib(v >> 4);
    put_nib(v);
}

static void put_hex16(uint16_t v)
{
    put_hex8((uint8_t)(v >> 8));
    put_hex8((uint8_t)v);
}

#if CLOCK_TRIM && BOOT_BANNER >= 2
static void put_dec(uint16_t v)
{
    uint8_t  buf[5];
    uint8_t  n = 0;

    do {
        buf[n++] = (uint8_t)('0' + (v % 10));
        v /= 10;
    } while (v);

    while (n)
        uart_putc(buf[--n]);
}
#endif

#if BOOT_BANNER >= 2

/* ---------------------------------------------------------------------
 * MEASURING THE ACTUAL CLOCK
 *
 * 'U' = 0x55 = alternating bits.  An 8N1 frame on the wire looks like:
 *
 *   start d0 d1 d2 d3 d4 d5 d6 d7 stop | start ...
 *     0   1  0  1  0  1  0  1  0   1   |   0
 *
 * so a continuous stream of 'U' produces a clean square wave on TXD
 * whose half period equals EXACTLY one bit time.  A multimeter in
 * frequency mode therefore reads baud/2, giving:
 *
 *     baud  = 2 * f_measured
 *     F_CPU = baud * 8  * (UBRR+1)     at U2X = 1   (Vue:  f * 208)
 *     F_CPU = baud * 16 * (UBRR+1)     at U2X = 0   (VP2:  f * 192)
 *
 * Nominally the meter should read 9600 Hz on both platforms.
 * CAL_CHARS is sized for about 2 s of continuous signal, which is what
 * a typical multimeter needs for the reading to settle.
 * ------------------------------------------------------------------- */

#define CAL_CHARS  ((uint16_t)(2UL * BAUD_RATE / 10UL))   /* ~2 s */

static void cal_burst(void)
{
    uint16_t n;

    for (n = 0; n < CAL_CHARS; n++)
        uart_putc('U');
}

#endif /* BOOT_BANNER >= 2 */

#endif /* BOOT_BANNER >= 1 */

/* =====================================================================
 * CRC-16/XMODEM  (poly 0x1021, init 0x0000, MSB first)
 *
 * Nibble-wise variant: a 32 B table instead of 512 B, about four times
 * faster than the bitwise version.  Verified against three real firmware
 * images - Vue 4.30 0xD795, VP2 3.80 0xF6CD, VP2 3.88 0x9E6B.
 * ===================================================================== */

/* THE TABLE LIVES IN RAM, NOT IN PROGMEM - and that is deliberate.
 *
 * The boot section sits above 64 KB (0x1F000 / 0x1F800).  Pointers on
 * AVR are 16-bit, and pgm_read_word() compiles to LPM, which uses ONLY
 * the 16-bit Z register and ignores RAMPZ.  A table placed at 0x1F08C
 * was therefore read from 0xF08C - that is, from the APPLICATION area.
 *
 * The bug was invisible while the application section was empty:
 * app_valid() returned early on the empty reset vector (0xFFFF) and the
 * CRC loop never ran.  It only surfaced once firmware was flashed.
 * Symptom: computed CRC AB12 instead of the D795 stored in the footer,
 * so the bootloader never handed over to a perfectly good application.
 *
 * In a disassembly it shows up as "subi r30,0x74 / sbci r31,0x0F"
 * (i.e. +0xF08C instead of +0x1F08C) just before the lpm.  "make check"
 * guards against it: the finished image must contain NO LPM instruction
 * at all, only ELPM.
 *
 * Keeping the table in RAM costs 32 B of SRAM out of 4096 and is
 * incidentally faster than LPM.
 *
 * The general rule for this file: read flash only through the _far
 * variants (pgm_read_byte_far / pgm_read_word_far), which set RAMPZ.  */
static const uint16_t crc_tab[16] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF
};

static uint16_t crc_update(uint16_t crc, uint8_t b)
{
    crc = (crc << 4) ^ crc_tab[((crc >> 12) ^ (b >> 4)) & 0x0F];
    crc = (crc << 4) ^ crc_tab[((crc >> 12) ^ (b & 0x0F)) & 0x0F];
    return crc;
}

#if BOOT_BANNER >= 1
/* 0 = empty reset vector, 1 = CRC matches, 2 = mismatch, 3 = not computed */
static uint8_t  app_state;
static uint16_t crc_calc, crc_stored;
#endif

/* The application is valid when the CRC of 0x00000..CRC_ADDR-1 matches
 * the two trailing bytes stored BIG-ENDIAN.  Fast reject: an empty reset
 * vector means there is no application at all.                         */
static uint8_t app_valid(void)
{
    uint32_t a;
    uint16_t crc = 0, stored;

    if (pgm_read_word_far(0) == 0xFFFF) {
#if BOOT_BANNER >= 1
        app_state = 0;
#endif
        return 0;
    }

    for (a = 0; a < CRC_ADDR; a++) {
        crc = crc_update(crc, pgm_read_byte_far(a));
        if ((a & 0x03FF) == 0)
            wdt_reset();
    }

    stored = ((uint16_t)pgm_read_byte_far(CRC_ADDR) << 8)
           |  (uint16_t)pgm_read_byte_far(CRC_ADDR + 1);

#if BOOT_BANNER >= 1
    crc_calc   = crc;
    crc_stored = stored;
    app_state  = (crc == stored) ? 1 : 2;
#endif

    return (crc == stored);
}

/* =====================================================================
 * Flash programming
 * ===================================================================== */

static void page_program(uint16_t page)
{
    uint32_t addr = (uint32_t)page * PAGE_SIZE;
    uint16_t i;

    boot_page_erase(addr);
    boot_spm_busy_wait();

    for (i = 0; i < PAGE_SIZE; i += 2)
        boot_page_fill(addr + i,
                       (uint16_t)page_buf[i] | ((uint16_t)page_buf[i + 1] << 8));

    boot_page_write(addr);
    boot_spm_busy_wait();

    boot_rww_enable();
}

/* The 'C' command.  Erasing happens per page during programming, so all
 * this needs to do is invalidate the footer, ensuring an interrupted
 * update cannot leave an image that looks valid.
 *
 * Note for anyone comparing against the original: in the captures the
 * stock bootloader takes 256 ms (Vue, application present) and 1153 ms
 * (VP2, blank flash) to answer 'C'.  Those two differ four-fold, which a
 * fixed number of page erases cannot explain, so the original evidently
 * does something more here.  What exactly is not known.               */
static void app_invalidate(void)
{
    boot_page_erase(BOOT_START - PAGE_SIZE);
    boot_spm_busy_wait();
    boot_rww_enable();
}

/* =====================================================================
 * Starting the application
 * ===================================================================== */

static void start_app(void)
{
    UCSRB_REG = 0;                             /* silence the UART      */
    boot_rww_enable();
    MCUCR = (1 << IVCE);
    MCUCR = 0;                                 /* vectors in the app    */
    RAMPZ = 0;
    __asm__ __volatile__ ("jmp 0" ::);         /* a jump, not a reset - */
                                               /* a reset would come    */
                                               /* straight back here    */
}

/* =====================================================================
 * Command loop
 * ===================================================================== */

int main(void)
{
    uint8_t  mcucsr, cmd, c, stay, i;
    uint8_t  arg[8];
    uint8_t  argn;
    uint16_t page;

    /* Last end-of-line character received, 0 = none.  Used to collapse a
     * CR+LF (or LF+CR) pair into ONE line ending.  The Davis updater
     * sends a bare '\n' so it is unaffected, but a terminal sends a pair
     * and without this every command produced a spurious extra prompt. */
    uint8_t  last_eol = 0;

    mcucsr = MCUCSR;
    MCUCSR = 0;
    wdt_disable();
    cli();

#if CLOCK_TRIM
    /* -----------------------------------------------------------------
     * TRIM ONLY AFTER A REAL RESET
     *
     * No flag set in MCUCSR  ==  there was no reset.  Since we are in
     * main() anyway, the application JUMPED here via its BOOT command.
     *
     * All four premises are proven from the Vue 4.30 disassembly, not
     * assumed:
     *
     *   1. The application hands over with "ijmp" (0xFE1A), not with a
     *      reset - so the reset flags stay as they were.
     *   2. The application only CLEARS MCUCSR (0x14A0C: in, andi 0xF7,
     *      out; then out 0) and never sets a flag - so zero is a
     *      trustworthy marker for this path.
     *   3. The application trims OSCCAL itself against the same crystal
     *      (0x14618), so the clock is already correct.
     *   4. The application uses Timer0 asynchronously as its real-time
     *      clock.  The ATmega128 datasheet warns explicitly that
     *      switching AS0 while the counter is running may corrupt
     *      TCNT0, OCR0 and TCCR0 - which is exactly what trimming did.
     *
     * Symptom without this condition: after BOOT the application sent
     * its ACK, the port then emitted garbage, and the bootloader failed
     * to answer three successive 'C' commands.
     *
     * HISTORICAL NOTE: this condition was once added and then reverted
     * because a test looked like a regression.  The actual blocker at
     * the time was a wrong High fuse - the application was not handing
     * over at all.  Do not revert it again without evidence that it is
     * the cause.
     * --------------------------------------------------------------- */
    if (mcucsr & 0x1F)
        clock_trim();
#endif

    uart_init();

#if BOOT_GREETING
    /* Real reset only - silence on the BOOT path, see the macro above. */
    if (mcucsr & 0x1F)
        uart_puts(GREETING);
#endif

#if BOOT_BANNER == 1
    /* Level 1 - the minimum the updater has a chance of tolerating: one
     * short line at startup, one more after the CRC check.             */
    uart_puts("\r\nBOOTLOADER " MODEL_ID "\r\n");
#endif

#if BOOT_BANNER >= 2
    /* The banner goes out BEFORE the CRC check - checking 126 KB takes
     * 2-3 s at 2 MHz and the port is silent throughout.  We want
     * immediate evidence that the bootloader started and can transmit. */
    uart_puts("\r\n=== DAVIS BOOT " MODEL_ID " ===\r\n");

    uart_puts("SEC  ");
    put_nib((uint8_t)(BOOT_START >> 16));
    put_hex16((uint16_t)BOOT_START);

    uart_puts("  VER ");
    put_hex8(BVER_B0);
    uart_putc(' ');
    put_hex8(BVER_B1);

    uart_puts("\r\nUART ");
    uart_putc((uint8_t)('0' + UART_PORT));
    uart_puts("  UBRR ");
    put_hex16((uint16_t)UBRR_VAL);
    uart_puts("  U2X ");
    uart_putc((uint8_t)('0' + UART_U2X));
    uart_puts("  BAUD " BAUD_TXT "\r\n");

#if CLOCK_TRIM
    uart_puts("CLK  ");
    if (!trim_done) {
        uart_puts("NO 32k CRYSTAL - OSCCAL untouched");
    } else {
        put_dec(trim_count);
        uart_puts(" / " );
        put_dec(TRIM_TARGET);
        uart_puts("  F=");
        /* F_CPU = count * 32768 / 32 = count * 1024 Hz.
         * In kHz: count * 1024 / 1000 = count * 128 / 125.             */
        put_dec((uint16_t)(((uint32_t)trim_count * 128UL) / 125UL));
        uart_puts(" kHz  OSCCAL ");
        put_hex8(OSCCAL);
    }
    uart_puts("\r\n");
#endif

    uart_puts("MCUCSR ");
    put_hex8(mcucsr);
    uart_puts(" =");
    if (mcucsr & 0x01) uart_puts(" POR");
    if (mcucsr & 0x02) uart_puts(" EXT");
    if (mcucsr & 0x04) uart_puts(" BOR");
    if (mcucsr & 0x08) uart_puts(" WDT");
    if (mcucsr & 0x10) uart_puts(" JTAG");
    if ((mcucsr & 0x1F) == 0) uart_puts(" none (jumped from app)");

    uart_puts("\r\nCAL  ");
    cal_burst();

    uart_puts("\r\nCRC  checking...");
#endif /* BOOT_BANNER >= 2 */

    stay = 0;

    /* -----------------------------------------------------------------
     * ENTRY FROM THE APPLICATION VIA THE "BOOT" COMMAND
     *
     * The application hands over with a plain "ijmp" (0xFE1A) - no
     * reset, and no marker of any kind left behind.  If the bootloader
     * treated that as an ordinary start it would see a valid CRC and
     * bounce straight back, making the BOOT command a no-op.  Observed
     * exactly that on hardware:
     *
     *     BOOT  ->  06          ACK from the application
     *     ID    ->  (silence)   bootloader busy computing the CRC
     *     app banner reappears  application had been restarted
     *
     * The discriminator: no flag in MCUCSR means NO RESET HAPPENED.
     * Since we are in main() regardless, something jumped here - and the
     * only thing that jumps into the boot section is the BOOT handler.
     * The application itself only ever clears MCUCSR (0x14A0C), never
     * sets it, so zero is trustworthy.
     * --------------------------------------------------------------- */
    if (!(mcucsr & 0x1F))         stay = 1;

    if (mcucsr & (1 << WDRF))     stay = 1;

    /* The CRC is computed ONLY when its result can still change the
     * decision.  Once we know we are staying in command mode, checking
     * 126 KB of flash wastes 2-3 s at the worst possible moment: after
     * BOOT the updater sends 'C' within about 2 s, and the USART receive
     * buffer holds only two bytes.  Without this shortcut the bootloader
     * answered a second late or dropped the command entirely.          */
    if (!stay && !app_valid())    stay = 1;
#if BOOT_BANNER >= 1
    else if (stay)
        app_state = 3;                     /* 3 = CRC not computed     */
#endif

#if BOOT_BANNER == 1
    /* One line carrying everything needed to explain the decision:
     *   CRC <computed><=|!><stored>  M<MCUCSR>  APP|CMD
     * It distinguishes a CRC mismatch from every other reason to stay. */
    uart_puts("CRC ");
    if (app_state == 3) {
        uart_puts("SKIP");
    } else if (app_state == 0) {
        uart_puts("NONE");
    } else {
        put_hex16(crc_calc);
        uart_putc(app_state == 1 ? '=' : '!');
        put_hex16(crc_stored);
    }
    uart_puts(" M");
    put_hex8(mcucsr);
    uart_puts(stay ? " CMD\r\n" : " APP\r\n");
    if (!stay)
        uart_flush();
#endif

#if BOOT_BANNER >= 2
    if (app_state == 3) {
        uart_puts(" SKIPPED (staying in command mode regardless)\r\n");
    } else if (app_state == 0) {
        uart_puts(" NO APPLICATION (reset vector = FFFF)\r\n");
    } else {
        uart_putc(' ');
        put_hex16(crc_calc);
        uart_puts(app_state == 1 ? " == " : " != ");
        put_hex16(crc_stored);
        uart_puts(app_state == 1 ? "  OK\r\n" : "  BAD\r\n");
    }

    if (!stay) {
        uart_puts("-> STARTING APPLICATION\r\n");
        uart_flush();
    } else {
        uart_puts("-> COMMAND MODE (I / B / C / F / P / A)\r\n");
    }
#endif

    if (!stay) {
#if BOOT_GREETING
        /* Reaching this point already implies a real reset: a zero
         * MCUCSR forces stay = 1 above, so the BOOT path never gets
         * here.  The 'A' command calls start_app() directly and stays
         * silent, which is what the updater expects.                  */
        uart_puts("APP START\r\n");
        uart_flush();
#endif
        start_app();
    }

    /* -----------------------------------------------------------------
     * DO NOT SEND AN ACK HERE AFTER ENTRY FROM THE APPLICATION
     *
     * Established from the Vue 4.30 (0xFDD6) and VP2 3.88 (0x154D8)
     * disassemblies: the 0x06 that follows a BOOT command is sent by the
     * APPLICATION, not by the bootloader.  It does so just before the
     * jump.  The bootloader must stay silent at this point, or the
     * updater sees a duplicated byte.
     *
     * Sending an ACK on WDRF was tried here and removed - a wrong
     * premise, since the application never resets, it jumps.
     * --------------------------------------------------------------- */

    for (;;) {
        cmd = uart_getc();

        /* The stock updater sends upper case, but when testing by hand
         * from a terminal it is easy to type lower case and get silence,
         * because the switch below would not recognise it.             */
        if (cmd >= 'a' && cmd <= 'z')
            cmd -= 'a' - 'A';

#if BOOT_BANNER >= 2
        /* Raw value of the received byte.  If typing 'B' comes back as
         * 42, the receiver is fine; anything else means a clock error. */
        if (cmd != '\n' && cmd != '\r') {
            uart_puts("\r\nRX ");
            put_hex8(cmd);
            uart_puts(" -> ");
        }
#endif

        if (cmd == '\n' || cmd == '\r') {
            /* Second half of a CR+LF / LF+CR pair - swallow it silently.
             * Two IDENTICAL characters in a row are two separate lines. */
            if (last_eol && last_eol != cmd) {
                last_eol = 0;
                continue;
            }
            last_eol = cmd;
            uart_putc(PROMPT);
            continue;
        }

        last_eol = 0;

        /* The rest of the line is ignored, except that 'P' needs it for
         * the page number and 'B' needs it to tell BOOT from BVER.     */
        argn = 0;
        for (;;) {
            c = uart_getc();
            if (c == '\n' || c == '\r') {
                last_eol = c;      /* so the pair's partner is swallowed */
                break;
            }
            if (argn < sizeof(arg) - 1)
                arg[argn++] = c;
        }
        arg[argn] = 0;

        switch (cmd) {

        case 'I':                              /* ID                    */
            /* The 'B' suffix is how the updater recognises that it is
             * talking to the bootloader rather than the application,
             * which answers a bare "6351".  Do not remove it.          */
            uart_puts(MODEL_ID);
            break;

        case 'B':                              /* BVER or BOOT          */
            /* Both start with 'B' and only the first letter is decoded,
             * so they have to be told apart by the rest of the line.
             * BOOT arriving at an already-running bootloader means only
             * "yes, I am the bootloader": a bare ACK.  Appending the
             * version bytes left two stray bytes in the updater's
             * stream.                                                  */
            uart_putc(ACK);
            if (arg[0] != 'O' && arg[0] != 'o') {
                uart_putc(BVER_B0);
                uart_putc(BVER_B1);
            }
            break;

        case 'C':                              /* invalidate the app    */
            app_invalidate();
            uart_putc(ACK);
            break;

        case 'F':                              /* 256 bytes into RAM    */
            uart_putc(ACK);
            for (i = 0; ; i++) {
                page_buf[i] = uart_getc();
                if (i == PAGE_SIZE - 1)
                    break;
            }
            uart_putc(ACK);
            break;

        case 'P':                              /* program a page        */
            page = 0;
            for (i = 0; i < argn; i++)
                if (arg[i] >= '0' && arg[i] <= '9')
                    page = page * 10 + (uint16_t)(arg[i] - '0');

            if (page < APP_PAGES)              /* never overwrite self  */
                page_program(page);

            uart_putc(ACK);
            break;

        case 'A':                              /* run the application   */
            start_app();
            break;

#if BOOT_BANNER >= 2
        case 'U':                              /* repeat the cal burst  */
            cal_burst();
            uart_puts("\r\n");
            break;
#endif

        default:
            /* Unknown command - answer with the prompt so it is visible
             * that the bootloader is alive.  Confirmed against the stock
             * bootloader, which replies '!' to the application-only
             * command "VER".                                           */
            uart_putc(PROMPT);
            break;
        }
    }
}
