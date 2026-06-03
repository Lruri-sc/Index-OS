#pragma once

#include <stdint.h>

namespace index {

// IdolTheory: Aleister Crowley's framework for reshaping reality, here
// repurposed as the kernel's wall-clock time module. LastOrder is the
// monotonic tick counter (CNTPCT-derived, restarts every boot); IdolTheory
// reads the PL031 RTC at boot to learn the real-world epoch and answers
// clock_gettime(CLOCK_REALTIME) + fills inode timestamps so files persist
// real-looking mtimes across reboots.
void idol_theory_init();

// Seconds since Unix epoch (1970-01-01 UTC). Returns 0 until idol_theory_init
// has discovered the RTC. After init it advances with CNTPCT so the value
// stays current even though we only read the RTC once.
uint64_t idol_theory_epoch_seconds();

// Same as above but with nanosecond resolution.
void idol_theory_epoch_nanos(uint64_t *out_sec, uint64_t *out_nsec);

} // namespace index
