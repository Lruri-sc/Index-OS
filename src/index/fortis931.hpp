#pragma once

#include <stdint.h>

namespace index {

// Fortis931: Stiyl Magnus's remote-incineration spell. By naming his target
// Stiyl can burn it from a distance -- no contact, no warning. Here it is the
// signals layer: name an Esper by pid and end it from afar. MVP only does
// default termination (no handler registration, no sigaction, no async
// console Ctrl-C yet), so every supported signal has the same effect: tear
// the target down, release its pipes, and -- if its parent is sitting in
// wait() -- hand the exit code to that parent on its next schedule. SIGKILL
// and SIGTERM behave identically here; the distinction will matter when
// handler registration arrives.
constexpr int kSigInt = 2;
constexpr int kSigKill = 9;
constexpr int kSigTerm = 15;
constexpr int kSigChld = 17;

// Returns true if `target_slot` was a live Esper that we killed, false if it
// was already dead/free or out of range. `sig` is recorded in the target's
// exit_code as 128 + sig (the conventional "killed by signal N" code).
bool fortis931_kill(int target_slot, int sig);

// Send `sig` to every Esper whose pgrp == `pgrp`. Returns the count of
// successful deliveries. Used by the PL011 RX IRQ path to forward VINTR
// (Ctrl-C) / VQUIT / VSUSP to the foreground process group of the console.
// Special-cases waiting state: the target gets its saved X0 pre-set to
// -EINTR, then state -> ready, so the in-flight wait/nanosleep/read syscall
// returns -EINTR after the signal handler eventually rt_sigreturns (or
// immediately, if SIG_DFL terminates).
int fortis931_kill_pgrp(uint32_t pgrp, int sig);

} // namespace index
