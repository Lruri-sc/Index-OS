#include "index/last_order.hpp"

#include "arch/aarch64/cpu.hpp"
#include "drivers/aleister.hpp"

namespace index {

namespace {

// Virtual timer = PPI 11 = GIC interrupt ID 27 on the virt SoC. The virtual
// timer (not the physical CNTP) is the one reachable from EL1 whether the
// kernel runs on bare QEMU (TCG) or under a hypervisor like HVF, which owns
// EL2 and traps EL1 physical-timer access.
constexpr uint32_t kTimerIntId = 27;

LastOrder *g_clock = nullptr;
volatile uint64_t g_ticks = 0;

uint64_t read_cntfrq() {
    uint64_t value = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

void write_tval(uint64_t value) {
    asm volatile("msr cntv_tval_el0, %0" ::"r"(value) : "memory");
}

void write_ctl(uint64_t value) {
    asm volatile("msr cntv_ctl_el0, %0" ::"r"(value) : "memory");
    asm volatile("isb");
}

void on_beat(void *) {
    // Every core reprograms its own (banked) timer so it keeps beating, but only
    // the boot core advances the shared heartbeat -- otherwise N cores would
    // inflate the tick count N-fold and break sleep deadlines.
    if (arch::this_cpu_id() == 0) {
        g_ticks = g_ticks + 1;
    }
    if (g_clock != nullptr) {
        write_tval(g_clock->interval); // schedule the next beat
    }
}

} // namespace

void last_order_init(LastOrder &clock, uint32_t hz) {
    clock.frequency = read_cntfrq();
    clock.hz = hz;
    clock.interval = hz != 0 ? clock.frequency / hz : clock.frequency;
    if (clock.interval == 0) {
        clock.interval = 1;
    }

    g_clock = &clock;
    g_ticks = 0;

    drivers::aleister_register(kTimerIntId, on_beat, nullptr);
    drivers::aleister_enable(kTimerIntId, 0x00);

    write_tval(clock.interval);
    write_ctl(1); // ENABLE=1, IMASK=0
    clock.armed = true;
}

void last_order_arm_secondary() {
    if (g_clock == nullptr) {
        return; // boot core never armed the heartbeat
    }
    drivers::aleister_enable(kTimerIntId, 0x00); // enable this core's banked PPI 27
    write_tval(g_clock->interval);
    write_ctl(1); // ENABLE=1, IMASK=0
}

uint64_t last_order_ticks() {
    return g_ticks;
}

uint32_t last_order_hz() {
    return g_clock != nullptr ? g_clock->hz : 0;
}

uint64_t last_order_interval_cycles() {
    return g_clock != nullptr ? g_clock->interval : 0;
}

void last_order_set_oneshot_cycles(uint64_t cycles) {
    if (g_clock == nullptr) return;
    if (cycles == 0) cycles = 1;
    // CNTV_TVAL_EL0 is the signed delta to the next compare; writing it sets
    // CNTV_CVAL = CNTPCT + value. ENABLE stays asserted, so the timer fires
    // when CNTPCT crosses the new compare. The next on_beat will reprogram
    // the regular interval, giving us only one extended sleep.
    write_tval(cycles);
}

} // namespace index
