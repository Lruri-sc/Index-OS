#include "index/idol_theory.hpp"

#include "index/imaginary_number_district.hpp"
#include "index/mmio.hpp"

namespace index {

namespace district = index::imaginary_number_district;

namespace {

// QEMU 'virt' machine maps a PL031 RTC at 0x09010000. Its DR register (offset
// 0) returns seconds since the Unix epoch (the firmware initialises it from
// the host clock at startup). Reading DR atomically captures the current
// value; CR is initialised to enabled by QEMU.
constexpr uint64_t kPl031Base = 0x09010000;
constexpr uint64_t kPl031Dr = 0x00; // RTC data register (u32 seconds)

// Anchor: we read the RTC once and remember the CNTPCT value at the moment of
// that read. Subsequent epoch queries add (now_cntpct - anchor_cntpct) / freq.
uint64_t g_anchor_seconds = 0;
uint64_t g_anchor_cntpct = 0;
uint64_t g_cntfrq = 0;
bool g_ready = false;

uint64_t read_cntpct() {
    uint64_t v = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

uint64_t read_cntfrq() {
    uint64_t v = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

} // namespace

void idol_theory_init() {
    g_cntfrq = read_cntfrq();
    if (g_cntfrq == 0) g_cntfrq = 62500000; // fallback for unusual TCG configs
    // Read the PL031. Most QEMU virt setups expose it at this fixed address
    // (we don't yet parse the DTB for it). If the read returns 0 the firmware
    // never set it -- treat as "wall clock unknown" and fall back to 0.
    const uint32_t sec = mmio::read32(kPl031Base + kPl031Dr);
    g_anchor_seconds = sec;
    g_anchor_cntpct = read_cntpct();
    g_ready = (sec != 0);
    if (g_ready) {
        district::write("  IdolTheory (RTC)   : PL031 ");
        district::dec(static_cast<uint64_t>(sec));
        district::write(" s since epoch (");
        // Decode to YYYY-MM-DD roughly. Cheap approximation: 365.25 d/yr from 1970.
        const uint64_t days = sec / 86400;
        const uint64_t year_approx = 1970 + (days * 4) / 1461;
        district::dec(year_approx);
        district::writeln(")");
    } else {
        district::writeln("  IdolTheory (RTC)   : no RTC; CLOCK_REALTIME stays 0");
    }
}

uint64_t idol_theory_epoch_seconds() {
    if (!g_ready) return 0;
    const uint64_t delta = read_cntpct() - g_anchor_cntpct;
    return g_anchor_seconds + (g_cntfrq ? delta / g_cntfrq : 0);
}

void idol_theory_epoch_nanos(uint64_t *out_sec, uint64_t *out_nsec) {
    const uint64_t freq = g_cntfrq ? g_cntfrq : 1;
    const uint64_t delta = read_cntpct() - g_anchor_cntpct;
    const uint64_t sec = g_anchor_seconds + delta / freq;
    const uint64_t nsec = ((delta % freq) * 1000000000ULL) / freq;
    if (out_sec) *out_sec = sec;
    if (out_nsec) *out_nsec = nsec;
}

} // namespace index
