#include "drivers/electro_master.hpp"

#include "index/mmio.hpp"

namespace index::drivers {

namespace {

uint32_t mmio_reg(uint32_t offset, uint32_t shift) {
    return offset << shift;
}

} // namespace

bool electro_master_ready(const ElectroMaster &line) {
    using index::mmio::read32;

    switch (line.kind) {
    case ElectroMasterKind::pl011:
        return (read32(line.base + 0x18) & (1u << 5)) == 0;
    case ElectroMasterKind::ns16550:
        return (read32(line.base + mmio_reg(5, line.reg_shift)) & (1u << 5)) != 0;
    case ElectroMasterKind::samsung:
        return (read32(line.base + 0x10) & (1u << 1)) != 0;
    case ElectroMasterKind::none:
        return false;
    }
    return false;
}

bool electro_master_has_input(const ElectroMaster &line) {
    using index::mmio::read32;

    switch (line.kind) {
    case ElectroMasterKind::pl011:
        return (read32(line.base + 0x18) & (1u << 4)) == 0;
    case ElectroMasterKind::ns16550:
        return (read32(line.base + mmio_reg(5, line.reg_shift)) & 1u) != 0;
    case ElectroMasterKind::samsung:
        return (read32(line.base + 0x10) & 1u) != 0;
    case ElectroMasterKind::none:
        return false;
    }
    return false;
}

void electro_master_putc(const ElectroMaster &line, char c) {
    using index::mmio::write32;

    if (line.kind == ElectroMasterKind::none || line.base == 0) {
        return;
    }

    // Serial terminals (and UTM's built-in terminal) need a carriage return to
    // get back to column 0; emit CR before every LF so a bare '\n' from anywhere
    // (kernel or EL0 user program) renders as a proper newline, not a staircase.
    if (c == '\n') {
        electro_master_putc(line, '\r');
    }

    for (uint32_t spin = 0; spin < 1000000 && !electro_master_ready(line); ++spin) {
        asm volatile("yield");
    }

    switch (line.kind) {
    case ElectroMasterKind::pl011:
        write32(line.base + 0x00, static_cast<uint32_t>(c));
        break;
    case ElectroMasterKind::ns16550:
        write32(line.base + mmio_reg(0, line.reg_shift), static_cast<uint32_t>(c));
        break;
    case ElectroMasterKind::samsung:
        write32(line.base + 0x20, static_cast<uint32_t>(c));
        break;
    case ElectroMasterKind::none:
        break;
    }
}

void electro_master_write(const ElectroMaster &line, const char *s) {
    while (*s) {
        electro_master_putc(line, *s++); // putc adds CR before any LF
    }
}

int electro_master_try_read(const ElectroMaster &line) {
    using index::mmio::read32;

    if (line.kind == ElectroMasterKind::none || line.base == 0 ||
        !electro_master_has_input(line)) {
        return -1;
    }

    switch (line.kind) {
    case ElectroMasterKind::pl011:
        return static_cast<int>(read32(line.base + 0x00) & 0xff);
    case ElectroMasterKind::ns16550:
        return static_cast<int>(read32(line.base + mmio_reg(0, line.reg_shift)) & 0xff);
    case ElectroMasterKind::samsung:
        return static_cast<int>(read32(line.base + 0x24) & 0xff);
    case ElectroMasterKind::none:
        return -1;
    }
    return -1;
}

void electro_master_enable_rx_irq(const ElectroMaster &line) {
    using index::mmio::read32;
    using index::mmio::write32;
    if (line.kind != ElectroMasterKind::pl011 || line.base == 0) return;
    // PL011 register offsets (per ARM PrimeCell UART r1p5):
    //   0x38 IMSC: bit 4 RXIM (RX FIFO at level), bit 6 RTIM (RX timeout).
    //   0x44 ICR : write-1-to-clear all interrupt latches.
    //   0x34 IFLS: RX FIFO trigger level (bits 5:3). Set to 1/8 (000) so a
    //              single byte triggers RXIM immediately.
    write32(line.base + 0x34, 0); // IFLS = RX 1/8, TX 1/8
    write32(line.base + 0x44, 0xFFFF); // ICR: clear any latched events
    write32(line.base + 0x38, (1u << 4) | (1u << 6)); // IMSC = RXIM | RTIM
}

void electro_master_clear_rx_irq(const ElectroMaster &line) {
    using index::mmio::write32;
    if (line.kind != ElectroMasterKind::pl011 || line.base == 0) return;
    // Clear RX + RX-timeout sources. Other bits left as the hardware reports
    // them; the kernel only listens for RX on this line today.
    write32(line.base + 0x44, (1u << 4) | (1u << 6));
}

const char *electro_master_kind_name(ElectroMasterKind kind) {
    switch (kind) {
    case ElectroMasterKind::pl011:
        return "pl011";
    case ElectroMasterKind::ns16550:
        return "ns16550";
    case ElectroMasterKind::samsung:
        return "samsung-s5l";
    case ElectroMasterKind::none:
        return "none";
    }
    return "unknown";
}

} // namespace index::drivers
