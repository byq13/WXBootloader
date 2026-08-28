# Davis / ATmega128 bootloader - builds both platforms
#
#   make            -> davis_boot_VP2.hex and davis_boot_VUE.hex
#   make BANNER=1   -> minimal: "BOOTLOADER 6351B" + one CRC/flags line
#   make BANNER=2   -> full diagnostics: banner, RX echo, CAL burst
#                      on VP2/Envoy this needs GREETING=0, see below
#   make GREETING=0 -> no startup greeting at all
#   make size       -> flash usage plus a hard boot-section limit
#   make check      -> sanity checks, see the comments on each below

MCU      = atmega128
BANNER   ?= 0
GREETING ?= 1
CFLAGS   = -mmcu=$(MCU) -Os -Wall -std=gnu99
CFLAGS  += -DBOOT_BANNER=$(BANNER) -DBOOT_GREETING=$(GREETING)
BOOTVER = -Wl,--section-start=.bootver=0x1FFFD

VP2_SEC = 0x1F800
VUE_SEC = 0x1F000

all: davis_boot_VP2.hex davis_boot_VUE.hex

boot_VP2.elf: davis_boot.c
	avr-gcc $(CFLAGS) -DPLATFORM_VP2 -Wl,--section-start=.text=$(VP2_SEC) $(BOOTVER) -o $@ $<

boot_VUE.elf: davis_boot.c
	avr-gcc $(CFLAGS) -DPLATFORM_VUE -Wl,--section-start=.text=$(VUE_SEC) $(BOOTVER) -o $@ $<

davis_boot_%.hex: boot_%.elf
	avr-objcopy -O ihex -R .eeprom $< $@
	avr-objdump -h -S $< > $*.lss

# Hard limit. The VP2 boot section is 2048 B, but the version byte sits at
# 0x1FFFD, so 2045 B are left for code. Vue: 4096 B / 4093 B.
size: boot_VP2.elf boot_VUE.elf
	@avr-size boot_VP2.elf boot_VUE.elf
	@for p in VP2:2045 VUE:4093; do \
	    e=boot_$${p%%:*}.elf; lim=$${p##*:}; \
	    n=$$(avr-size -A $$e | awk '/^\.text|^\.data/ {s+=$$2} END {print s+0}'); \
	    if [ $$n -gt $$lim ]; then \
	        echo "$$e: $$n B > limit $$lim B - DOES NOT FIT"; exit 1; \
	    else \
	        echo "$$e: $$n B / $$lim B  ($$((100*n/lim)) %)"; \
	    fi; \
	 done

check: boot_VP2.elf boot_VUE.elf
	@echo "--- version byte at 0x1FFFD (read by the application's BVER) ---"
	@for e in boot_VP2.elf boot_VUE.elf; do \
	    printf '%-14s ' $$e; \
	    avr-objdump -t $$e | grep bootver || echo "MISSING - app will return 0xFF"; \
	 done
	@echo "--- RAMPZ writes (I/O 0x3b or memory 0x005B) ---"
	@for e in boot_VP2.elf boot_VUE.elf; do \
	    printf '%-14s %s writes\n' $$e \
	      "$$(avr-objdump -d $$e | grep -cE '0x3b, r|0x005B, r')"; \
	 done
	@echo "--- spm / elpm ---"
	@for e in boot_VP2.elf boot_VUE.elf; do \
	    printf '%-14s %s\n' $$e "$$(avr-objdump -d $$e | grep -cE '\b(spm|elpm)\b')"; \
	 done
# The boot section lives above 64 KB, and LPM uses only the 16-bit Z
# register - it ignores RAMPZ. Any LPM therefore reads 0x10000 too low,
# landing in the application area. This caught a real bug: the CRC table
# was in PROGMEM and was being read out of the application's flash.
	@echo "--- LPM: boot section is above 64 KB, only elpm is allowed ---"
	@for e in boot_VP2.elf boot_VUE.elf; do \
	    n=$$(avr-objdump -d $$e | grep -cE '\blpm\b' || true); \
	    if [ "$$n" != "0" ]; then \
	        echo "$$e: $$n LPM instructions - reads land 0x10000 too low!"; exit 1; \
	    else echo "$$e: OK - no LPM"; fi; \
	 done
# USART check. Vue uses USART1 ONLY (the USART0_* vectors in image 4.30
# are empty); VP2 talks over USART0. Getting this wrong leaves the
# bootloader listening on a pin nothing is connected to - it just stays
# silent, which is a confusing failure to debug.
#   USART0: UDR0 = I/O 0x0C     USART1: UDR1 = memory 0x009C
	@echo "--- USART: VP2 must use USART0, Vue USART1 ---"
	@if avr-objdump -d boot_VP2.elf | grep -qE '(out|in).*0x0c'; then \
	    echo "boot_VP2.elf  OK  - USART0 (UDR0 = I/O 0x0C)"; \
	 else echo "boot_VP2.elf  FAIL - no access to UDR0!"; exit 1; fi
	@if avr-objdump -d boot_VP2.elf | grep -qE '0x009[Cc]'; then \
	    echo "boot_VP2.elf  FAIL - reaches for UDR1!"; exit 1; fi
	@if avr-objdump -d boot_VUE.elf | grep -qE '0x009[Cc]'; then \
	    echo "boot_VUE.elf  OK  - USART1 (UDR1 = 0x009C)"; \
	 else echo "boot_VUE.elf  FAIL - no access to UDR1!"; exit 1; fi
	@if avr-objdump -d boot_VUE.elf | grep -qE '(out|in).*0x0c'; then \
	    echo "boot_VUE.elf  FAIL - reaches for UDR0, Vue has no USART0!"; exit 1; fi

clean:
	rm -f *.elf

.PHONY: all size check clean
