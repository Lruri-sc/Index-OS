#pragma once

#include <stdint.h>

#include "drivers/electro_master.hpp"

namespace index::imaginary_number_district {

void init(const drivers::ElectroMaster &line);
bool ready();
void putc(char c);
void write(const char *s);
void writeln(const char *s);
void hex(uint64_t value);
void dec(uint64_t value);
int try_read();
// Non-destructive: peek whether a byte is in the RX FIFO (PL011 FR bit 4).
// Used by the timer tick to wake an Esper parked on ConsoleRead without
// consuming the byte -- the parked Esper drains it via try_read on resume.
bool has_input();
char read_blocking();
void banner();

// Register the UART SPI as an Aleister IRQ source and enable RX-interrupt
// generation in the UART itself. After this, each character arriving in the
// PL011 RX FIFO raises the SPI; the kernel-side handler calls
// `linux_ipc_wake(ConsoleRead, -1)` so a parked Esper resumes within
// microseconds (vs ~10 ms with 100 Hz polling).
void enable_rx_irq();

// Foreground process group of the console (single-TTY simplification --
// real Linux carries this per controlling tty). tcsetpgrp(stdin) goes here,
// the PL011 RX IRQ reads it to decide who gets SIGINT on Ctrl-C.
void set_fg_pgrp(uint32_t pgrp);
uint32_t get_fg_pgrp();

// Termios bits the IRQ path needs to consult cheaply: is ISIG enabled? The
// VINTR character (default Ctrl-C = 0x03). Updated whenever userland's
// TCSETS lands.
void set_console_isig(bool on, char vintr);
bool console_isig();
char console_vintr();

// Allow the console to take its SMP spinlock. Called by kmain AFTER the MMU is
// on (exclusive load/store needs the MMU); before that the console is lock-free
// and single-core. See imaginary_number_district.cpp.
void enable_locking();

// kmsg ring buffer. Every byte written through this namespace also lands in
// a 16 KiB ring so `dmesg` (Necessarius) and `/proc/kmsg` can replay the boot
// log + later kernel prints. New bytes overwrite oldest when the ring fills.
uint32_t kmsg_size();                 // current bytes in the ring (<= 16 KiB)
uint32_t kmsg_read(char *out, uint32_t cap); // copy ring contents from oldest

} // namespace index::imaginary_number_district
