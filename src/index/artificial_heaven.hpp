#pragma once

#include <stdint.h>

#include "drivers/aleister.hpp"
#include "drivers/artificial_heaven_canvas.hpp"
#include "drivers/electro_master.hpp"
#include "drivers/othinus.hpp"

namespace index {

constexpr uint32_t kMaxMemoryRanges = 16;
constexpr uint32_t kMaxAIMDiffusionRanges = 64;
constexpr uint32_t kMaxCpus = 8;

struct ImaginaryNumberRange {
    uint64_t base = 0;
    uint64_t size = 0;
};

enum class TestamentKind {
    unknown,
    apple_silicon,
    qemu_virt,
};

struct ArtificialHeaven {
    TestamentKind testament = TestamentKind::unknown;
    uint64_t dtb_addr = 0;
    uint32_t dtb_size = 0;
    drivers::ElectroMaster electro_master;
    drivers::ArtificialHeavenCanvas canvas;
    drivers::Othinus othinus;
    drivers::Aleister aleister;
    ImaginaryNumberRange memory[kMaxMemoryRanges];
    ImaginaryNumberRange aim_diffusion_field[kMaxAIMDiffusionRanges];
    uint64_t cpus[kMaxCpus] = {}; // MPIDR affinity of each CPU found under /cpus
    uint64_t pcie_ecam = 0;       // PCIe ECAM config-space base (0 if no host bridge)
    uint32_t memory_count = 0;
    uint32_t aim_field_count = 0;
    uint32_t cpu_count = 0;
};

ArtificialHeaven construct_artificial_heaven(uint64_t dtb_addr);
const char *testament_kind_name(TestamentKind kind);

} // namespace index
