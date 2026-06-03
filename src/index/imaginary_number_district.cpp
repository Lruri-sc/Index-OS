#include "index/imaginary_number_district.hpp"

#include "drivers/aleister.hpp"
#include "index/anti_skill.hpp"
#include "index/esper.hpp"
#include "index/fortis931.hpp"
#include "index/usermode.hpp"

namespace index::imaginary_number_district {

namespace {

drivers::ElectroMaster g_line;
bool g_ready = false;
uint32_t g_fg_pgrp = 0; // 0 = none (job control off / pre-tcsetpgrp)
bool g_isig = true;     // matches default termios (ISIG on)
char g_vintr = 3;       // Ctrl-C

// Serializes whole output items (a string, a number) so concurrent prints from
// multiple cores cannot interleave mid-token into garbage. Lines from different
// cores may still alternate -- like printk on SMP -- but never shred a token.
AntiSkill g_console_lock;

// The console is currently LOCK-FREE: g_console_locking is never set true.
// Reason: the AntiSkill spinlock (ldaxr/stxr + wfe) hangs under Apple HVF when
// taken in the early-boot window (before enable_irq / SMP bring-up) -- the banner
// printed but the next locked write spun forever. Taking it only AFTER SMP is up
// instead deadlocked the boot core against the secondaries already parked on wfe.
// Boot prints are single-core; EL0 prints go through syscalls. The only cost of
// lock-free is that simultaneous prints from different cores MAY interleave a
// token (printk-on-SMP style) -- never a crash. enable_locking() is retained for
// when the underlying pre-IRQ wfe-under-HVF issue is root-caused. See utm-deploy.
bool g_console_locking = false;

uint64_t console_lock() {
    return g_console_locking ? anti_skill_lock_irqsave(g_console_lock) : 0;
}

void console_unlock(uint64_t flags) {
    if (g_console_locking) {
        anti_skill_unlock_irqrestore(g_console_lock, flags);
    }
}

// kmsg ring: every emitted byte lands here so `dmesg` / /proc/kmsg can replay.
// Lock-piggyback: writers already hold g_console_lock, so the ring is updated
// without a separate lock.
constexpr uint32_t kKmsgSize = 16384;
char g_kmsg[kKmsgSize];
uint32_t g_kmsg_head = 0;   // next write offset
uint32_t g_kmsg_bytes = 0;  // total bytes written (capped at kKmsgSize)

void kmsg_put(char c) {
    g_kmsg[g_kmsg_head] = c;
    g_kmsg_head = (g_kmsg_head + 1) % kKmsgSize;
    if (g_kmsg_bytes < kKmsgSize) ++g_kmsg_bytes;
}

char hex_digit(uint8_t value) {
    return value < 10 ? char('0' + value) : char('a' + value - 10);
}

// Unlocked emit primitives: only ever called while g_console_lock is held, so
// they must NOT take the lock themselves (AntiSkill is not recursive). Every
// byte sent to the UART also lands in the kmsg ring.
void emit_char(char c) {
    drivers::electro_master_putc(g_line, c);
    kmsg_put(c);
}

void emit_str(const char *s) {
    drivers::electro_master_write(g_line, s);
    while (*s) kmsg_put(*s++);
}

void emit_hex(uint64_t value) {
    emit_str("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        emit_char(hex_digit(uint8_t((value >> shift) & 0xf)));
    }
}

void emit_dec(uint64_t value) {
    char buf[21];
    uint32_t pos = 0;
    if (value == 0) {
        emit_char('0');
        return;
    }
    while (value && pos < sizeof(buf)) {
        buf[pos++] = char('0' + (value % 10));
        value /= 10;
    }
    while (pos) {
        emit_char(buf[--pos]);
    }
}

} // namespace

void init(const drivers::ElectroMaster &line) {
    g_line = line;
    g_ready = line.kind != drivers::ElectroMasterKind::none && line.base != 0;
}

bool ready() {
    return g_ready;
}

void putc(char c) {
    if (!g_ready) {
        return;
    }
    const uint64_t flags = console_lock();
    emit_char(c);
    console_unlock(flags);
}

void write(const char *s) {
    if (!g_ready) {
        return;
    }
    const uint64_t flags = console_lock();
    emit_str(s);
    console_unlock(flags);
}

void writeln(const char *s) {
    if (!g_ready) {
        return;
    }
    const uint64_t flags = console_lock();
    emit_str(s);
    emit_char('\n');
    console_unlock(flags);
}

void hex(uint64_t value) {
    if (!g_ready) {
        return;
    }
    const uint64_t flags = console_lock();
    emit_hex(value);
    console_unlock(flags);
}

void dec(uint64_t value) {
    if (!g_ready) {
        return;
    }
    const uint64_t flags = console_lock();
    emit_dec(value);
    console_unlock(flags);
}

namespace {

// Kernel-side console input ring. The PL011 RX IRQ drains the hardware FIFO
// into this software ring; read syscalls consume from the ring. Two reasons
// the ring exists instead of read-directly-from-PL011:
//   1. The IRQ path needs to *inspect* each byte (VINTR / Ctrl-C) and consume
//      it BEFORE handing it to userland, so it can deliver SIGINT to the
//      foreground process group regardless of whether anyone is reading.
//   2. PL011's RX FIFO is only 32 bytes; under heavy paste the IRQ can drain
//      it into a bigger software buffer the userland reader can take its time
//      with.
// Single producer (PL011 IRQ on boot core) + single consumer (EL0 scheduler
// is single-core today), so no lock needed. If/when EL0 goes multi-core, wrap
// the pop in a CAS.
constexpr uint32_t kInputRingSize = 256;
char g_input_ring[kInputRingSize];
uint32_t g_input_head = 0;
uint32_t g_input_tail = 0;
uint32_t g_input_size = 0;

void input_push(char c) {
    if (g_input_size >= kInputRingSize) {
        // Drop oldest -- the alternative is dropping the new byte, but losing
        // the most recent keystroke is more confusing for an interactive user.
        g_input_tail = (g_input_tail + 1) % kInputRingSize;
        --g_input_size;
    }
    g_input_ring[g_input_head] = c;
    g_input_head = (g_input_head + 1) % kInputRingSize;
    ++g_input_size;
}

int input_pop() {
    if (g_input_size == 0) return -1;
    const char c = g_input_ring[g_input_tail];
    g_input_tail = (g_input_tail + 1) % kInputRingSize;
    --g_input_size;
    return static_cast<int>(static_cast<uint8_t>(c));
}

} // namespace

int try_read() {
    if (!g_ready) {
        return -1;
    }
    // Ring first; only fall back to a direct PL011 read if the ring is empty
    // AND no IRQ has fired yet (pre-enable_rx_irq boot path -- read_blocking
    // pre-init).
    const int from_ring = input_pop();
    if (from_ring >= 0) return from_ring;
    return drivers::electro_master_try_read(g_line);
}

bool has_input() {
    if (!g_ready) {
        return false;
    }
    if (g_input_size > 0) return true;
    return drivers::electro_master_has_input(g_line);
}

namespace {

// QEMU 'virt' machine UART0 is SPI 1 -> GIC INTID 32 + 1 = 33. The PL011's
// RX IRQ raises this whenever a character lands in the FIFO (RXIM) or the
// FIFO becomes non-empty long enough to trip the RX timeout (RTIM).
constexpr uint32_t kUartIntId = 33;

void pl011_rx_irq(void *) {
    // Drain the entire RX FIFO into the software ring; each byte goes through
    // a VINTR filter first. When ISIG is enabled and the byte matches VINTR
    // (default 0x03 = Ctrl-C), the byte is CONSUMED -- it does NOT enter the
    // ring -- and SIGINT is delivered to every Esper in the foreground pgrp.
    // That's how real Linux makes Ctrl-C interrupt a backgrounded `sleep 30`:
    // the TTY layer turns the keystroke into a signal long before the shell
    // gets around to reading the byte.
    bool delivered = false;
    for (;;) {
        const int ch = drivers::electro_master_try_read(g_line);
        if (ch < 0) break;
        const char c = static_cast<char>(ch);
        if (g_isig && c == g_vintr && g_fg_pgrp != 0) {
            fortis931_kill_pgrp(g_fg_pgrp, /*SIGINT*/2);
            delivered = true;
            continue; // consume the byte; never let userland read it
        }
        input_push(c);
    }
    drivers::electro_master_clear_rx_irq(g_line);
    linux_ipc_wake(Esper::IpcWaitKind::ConsoleRead, -1);
    (void)delivered;
}

} // namespace

void enable_rx_irq() {
    if (!g_ready) return;
    drivers::electro_master_enable_rx_irq(g_line);
    drivers::aleister_register(kUartIntId, pl011_rx_irq, nullptr);
    drivers::aleister_enable(kUartIntId, 0x80);
}

void set_fg_pgrp(uint32_t pgrp) { g_fg_pgrp = pgrp; }
uint32_t get_fg_pgrp() { return g_fg_pgrp; }
void set_console_isig(bool on, char vintr) { g_isig = on; g_vintr = vintr; }
bool console_isig() { return g_isig; }
char console_vintr() { return g_vintr; }

char read_blocking() {
    while (true) {
        const int ch = try_read();
        if (ch >= 0) {
            return static_cast<char>(ch);
        }
        asm volatile("yield");
    }
}

void enable_locking() { g_console_locking = true; }

void banner() {
    writeln("");
    writeln("Index kernel (arm-Index / UTM port)");
    writeln("  codename : Index Librorum Prohibitorum");
    writeln("  target   : UTM ARM64 virt (QEMU) + DTB");
    writeln("  district : Imaginary Number District");
}

uint32_t kmsg_size() { return g_kmsg_bytes; }

uint32_t kmsg_read(char *out, uint32_t cap) {
    if (out == nullptr || cap == 0 || g_kmsg_bytes == 0) return 0;
    const uint32_t n = (g_kmsg_bytes < cap) ? g_kmsg_bytes : cap;
    // Oldest byte: if the ring hasn't wrapped (bytes < size), it's at index 0.
    // Once wrapped, it's at head (the next slot to overwrite). Either way the
    // logical oldest is (head - bytes) mod size.
    const uint32_t start = (g_kmsg_head + kKmsgSize - g_kmsg_bytes) % kKmsgSize;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = g_kmsg[(start + i) % kKmsgSize];
    }
    return n;
}

} // namespace index::imaginary_number_district
