#pragma once

#include <stdint.h>

namespace index::drivers {

enum class ElectroMasterKind {
    none,
    pl011,
    ns16550,
    samsung,
};

struct ElectroMaster {
    ElectroMasterKind kind = ElectroMasterKind::none;
    uint64_t base = 0;
    uint32_t reg_shift = 0;
};

bool electro_master_ready(const ElectroMaster &line);
bool electro_master_has_input(const ElectroMaster &line);
void electro_master_putc(const ElectroMaster &line, char c);
void electro_master_write(const ElectroMaster &line, const char *s);
int electro_master_try_read(const ElectroMaster &line);
// Enable RX interrupt source on the UART (PL011 RXIM + RTIM). Caller routes
// the SPI through Aleister. ns16550/samsung not implemented (no-op).
void electro_master_enable_rx_irq(const ElectroMaster &line);
// Acknowledge a pending RX interrupt by clearing the device-side status. Call
// from the registered Aleister IRQ handler.
void electro_master_clear_rx_irq(const ElectroMaster &line);
const char *electro_master_kind_name(ElectroMasterKind kind);

} // namespace index::drivers
