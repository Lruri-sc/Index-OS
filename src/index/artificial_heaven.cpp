#include "index/artificial_heaven.hpp"

#include "index/fdt.hpp"
#include "index/types.hpp"

namespace index {

namespace {

struct NodeState {
    static constexpr uint32_t kMaxNodeRegs = 8;

    struct RegTuple {
        uint64_t base = 0;
        uint64_t size = 0;
    };

    uint32_t address_cells = 2;
    uint32_t size_cells = 1;
    RegTuple regs[kMaxNodeRegs];
    uint32_t reg_count = 0;
    uint32_t reg_shift = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    drivers::ElectroMasterKind electro_master_kind = drivers::ElectroMasterKind::none;
    drivers::PsciConduit psci_conduit = drivers::PsciConduit::none;
    bool is_reserved_container = false;
    bool in_reserved_memory = false;
    bool is_framebuffer = false;
    bool is_memory = false;
    bool is_psci = false;
    bool is_gic = false;
    bool is_gicv3 = false;
    bool is_cpu = false;
    bool is_pcie = false;
};

bool prop_name(const fdt::Property &prop, const char *name) {
    return prop.name && fdt::streq(prop.name, name);
}

drivers::ElectroMasterKind electro_master_kind_from_compatible(const fdt::Property &prop) {
    using fdt::compatible_has;

    if (compatible_has(prop, "arm,pl011") ||
        compatible_has(prop, "m1n1,uart") ||
        compatible_has(prop, "apple,m1n1-uart")) {
        return drivers::ElectroMasterKind::pl011;
    }

    if (compatible_has(prop, "ns16550a") ||
        compatible_has(prop, "ns16550") ||
        compatible_has(prop, "snps,dw-apb-uart")) {
        return drivers::ElectroMasterKind::ns16550;
    }

    if (compatible_has(prop, "apple,s5l-uart") ||
        compatible_has(prop, "samsung,s3c6400-uart") ||
        compatible_has(prop, "samsung,exynos4210-uart")) {
        return drivers::ElectroMasterKind::samsung;
    }

    return drivers::ElectroMasterKind::none;
}

bool is_gicv2_compatible(const fdt::Property &prop) {
    return fdt::compatible_has(prop, "arm,cortex-a15-gic") ||
           fdt::compatible_has(prop, "arm,cortex-a9-gic") ||
           fdt::compatible_has(prop, "arm,gic-400");
}

bool is_gicv3_compatible(const fdt::Property &prop) {
    return fdt::compatible_has(prop, "arm,gic-v3");
}

bool is_psci_compatible(const fdt::Property &prop) {
    return fdt::compatible_has(prop, "arm,psci") ||
           fdt::compatible_has(prop, "arm,psci-0.2") ||
           fdt::compatible_has(prop, "arm,psci-1.0");
}

bool is_pcie_compatible(const fdt::Property &prop) {
    return fdt::compatible_has(prop, "pci-host-ecam-generic") ||
           fdt::compatible_has(prop, "pci-host-cam-generic");
}

drivers::PsciConduit psci_conduit_from_method(const fdt::Property &prop) {
    if (fdt::bytes_eq_text(prop, "smc")) {
        return drivers::PsciConduit::smc;
    }
    if (fdt::bytes_eq_text(prop, "hvc")) {
        return drivers::PsciConduit::hvc;
    }
    return drivers::PsciConduit::none;
}

TestamentKind testament_from_compatible(const fdt::Property &prop) {
    if (fdt::compatible_has(prop, "linux,dummy-virt") ||
        fdt::compatible_has(prop, "qemu,virt")) {
        return TestamentKind::qemu_virt;
    }

    if (fdt::compatible_has_prefix(prop, "apple,")) {
        return TestamentKind::apple_silicon;
    }

    return TestamentKind::unknown;
}

void add_memory(ArtificialHeaven &heaven, uint64_t base, uint64_t size) {
    if (size == 0 || heaven.memory_count >= kMaxMemoryRanges) {
        return;
    }
    heaven.memory[heaven.memory_count++] = ImaginaryNumberRange{base, size};
}

void add_aim_diffusion_field(ArtificialHeaven &heaven, uint64_t base, uint64_t size) {
    if (size == 0 || heaven.aim_field_count >= kMaxAIMDiffusionRanges) {
        return;
    }
    heaven.aim_diffusion_field[heaven.aim_field_count++] = ImaginaryNumberRange{base, size};
}

void add_cpu(ArtificialHeaven &heaven, uint64_t mpidr) {
    if (heaven.cpu_count >= kMaxCpus) {
        return;
    }
    heaven.cpus[heaven.cpu_count++] = mpidr;
}

void add_reg(NodeState &node, uint64_t base, uint64_t size) {
    if (node.reg_count >= NodeState::kMaxNodeRegs) {
        return;
    }
    node.regs[node.reg_count++] = NodeState::RegTuple{base, size};
}

void finish_node(const NodeState &node, ArtificialHeaven &heaven) {
    const uint64_t reg_base = node.reg_count > 0 ? node.regs[0].base : 0;

    if (heaven.electro_master.kind == drivers::ElectroMasterKind::none &&
        node.electro_master_kind != drivers::ElectroMasterKind::none && reg_base != 0) {
        heaven.electro_master.kind = node.electro_master_kind;
        heaven.electro_master.base = reg_base;
        heaven.electro_master.reg_shift = node.reg_shift;
    }

    if (!heaven.othinus.available && node.is_psci) {
        heaven.othinus.conduit = node.psci_conduit != drivers::PsciConduit::none
                                     ? node.psci_conduit
                                     : drivers::PsciConduit::hvc;
        heaven.othinus.available = true;
    }

    if (!heaven.aleister.available && node.is_gic && node.reg_count >= 2) {
        // reg[0] = distributor (GICD) for both versions. reg[1] = the GICv2 CPU
        // interface (GICC) OR the GICv3 redistributor region (GICR).
        heaven.aleister.gicd = node.regs[0].base;
        if (node.is_gicv3) {
            heaven.aleister.version = drivers::GicVersion::v3;
            heaven.aleister.gicr = node.regs[1].base;
        } else {
            heaven.aleister.version = drivers::GicVersion::v2;
            heaven.aleister.gicc = node.regs[1].base;
        }
        heaven.aleister.available = node.regs[0].base != 0 && node.regs[1].base != 0;
    }

    if (!heaven.canvas.valid && node.is_framebuffer && reg_base != 0 &&
        node.width != 0 && node.height != 0 && node.stride != 0) {
        heaven.canvas.base = reg_base;
        heaven.canvas.width = node.width;
        heaven.canvas.height = node.height;
        heaven.canvas.stride = node.stride;
        heaven.canvas.valid = true;
    }

    if (node.is_cpu && node.reg_count > 0) {
        add_cpu(heaven, node.regs[0].base); // a cpu@N node's reg is its MPIDR
    }

    if (heaven.pcie_ecam == 0 && node.is_pcie && node.reg_count > 0) {
        heaven.pcie_ecam = node.regs[0].base; // the pcie node's reg is the ECAM base
    }

    if (node.is_memory) {
        for (uint32_t i = 0; i < node.reg_count; ++i) {
            add_memory(heaven, node.regs[i].base, node.regs[i].size);
        }
    }

    if (node.in_reserved_memory) {
        for (uint32_t i = 0; i < node.reg_count; ++i) {
            add_aim_diffusion_field(heaven, node.regs[i].base, node.regs[i].size);
        }
    }
}

void apply_testament_fallbacks(ArtificialHeaven &heaven) {
    if (heaven.testament == TestamentKind::qemu_virt &&
        heaven.electro_master.kind == drivers::ElectroMasterKind::none) {
        heaven.electro_master.kind = drivers::ElectroMasterKind::pl011;
        heaven.electro_master.base = 0x09000000;
        heaven.electro_master.reg_shift = 0;
    }

    if (heaven.testament == TestamentKind::qemu_virt && heaven.memory_count == 0) {
        add_memory(heaven, 0x40000000, mib(1024));
    }

    // UTM / QEMU virt always exposes PSCI; default to the EL1 `hvc` conduit
    // when the device tree did not spell it out.
    if (heaven.testament == TestamentKind::qemu_virt && !heaven.othinus.available) {
        heaven.othinus.conduit = drivers::PsciConduit::hvc;
        heaven.othinus.available = true;
    }

    // UTM / QEMU virt GICv2 lives at fixed addresses; fall back to them.
    if (heaven.testament == TestamentKind::qemu_virt && !heaven.aleister.available) {
        heaven.aleister.gicd = 0x08000000;
        heaven.aleister.gicc = 0x08010000;
        heaven.aleister.available = true;
    }

    // UTM / QEMU virt PCIe ECAM is the modern high window; fall back to it.
    if (heaven.testament == TestamentKind::qemu_virt && heaven.pcie_ecam == 0) {
        heaven.pcie_ecam = 0x4010000000ULL;
    }
}

ArtificialHeaven qemu_virt_fallback(uint64_t dtb_addr) {
    ArtificialHeaven heaven;
    heaven.testament = TestamentKind::qemu_virt;
    heaven.dtb_addr = dtb_addr;
    heaven.dtb_size = fdt::total_size(dtb_addr);
    apply_testament_fallbacks(heaven);
    return heaven;
}

} // namespace

ArtificialHeaven construct_artificial_heaven(uint64_t dtb_addr) {
    using namespace fdt;

    ArtificialHeaven heaven;
    heaven.dtb_addr = dtb_addr;
    heaven.dtb_size = total_size(dtb_addr);

    if (dtb_addr == 0) {
        return qemu_virt_fallback(dtb_addr);
    }

    const auto *hdr = reinterpret_cast<const Header *>(dtb_addr);
    if (be32(&hdr->magic) != kMagic) {
        return qemu_virt_fallback(dtb_addr);
    }

    const auto *base = reinterpret_cast<const uint8_t *>(dtb_addr);
    const auto *struct_p = base + be32(&hdr->off_dt_struct);
    const auto *strings = base + be32(&hdr->off_dt_strings);
    const auto *struct_end = struct_p + be32(&hdr->size_dt_struct);
    const auto *reserve_p = base + be32(&hdr->off_mem_rsvmap);

    while (true) {
        const uint64_t address = be64(reserve_p);
        const uint64_t size = be64(reserve_p + 8);
        reserve_p += 16;
        if (address == 0 && size == 0) {
            break;
        }
        add_aim_diffusion_field(heaven, address, size);
    }

    NodeState stack[32];
    uint32_t depth = 0;

    while (struct_p + 4 <= struct_end) {
        const uint32_t token = be32(struct_p);
        struct_p += 4;

        if (token == kBeginNode) {
            const char *name = reinterpret_cast<const char *>(struct_p);
            while (struct_p < struct_end && *struct_p) {
                ++struct_p;
            }
            if (struct_p < struct_end) {
                ++struct_p;
            }
            struct_p = align4(struct_p);

            if (depth < 32) {
                NodeState node;
                if (depth > 0) {
                    node.address_cells = stack[depth - 1].address_cells;
                    node.size_cells = stack[depth - 1].size_cells;
                    node.in_reserved_memory = stack[depth - 1].in_reserved_memory ||
                                              stack[depth - 1].is_reserved_container;
                }
                node.is_reserved_container = depth == 1 && streq(name, "reserved-memory");
                node.is_memory = starts_with(name, "memory@");
                node.is_psci = streq(name, "psci");
                node.is_cpu = starts_with(name, "cpu@"); // a CPU under /cpus
                stack[depth++] = node;
            }
            continue;
        }

        if (token == kEndNode) {
            if (depth > 0) {
                finish_node(stack[depth - 1], heaven);
                --depth;
            }
            continue;
        }

        if (token == kProp) {
            if (struct_p + 8 > struct_end || depth == 0) {
                break;
            }

            const uint32_t len = be32(struct_p);
            const uint32_t nameoff = be32(struct_p + 4);
            struct_p += 8;

            Property prop;
            prop.name = reinterpret_cast<const char *>(strings + nameoff);
            prop.data = struct_p;
            prop.len = len;
            struct_p = align4(struct_p + len);

            NodeState &node = stack[depth - 1];
            const uint32_t parent_address_cells = depth > 1 ? stack[depth - 2].address_cells : 2;
            const uint32_t parent_size_cells = depth > 1 ? stack[depth - 2].size_cells : 1;

            if (prop_name(prop, "#address-cells")) {
                node.address_cells = prop_u32(prop, node.address_cells);
            } else if (prop_name(prop, "#size-cells")) {
                node.size_cells = prop_u32(prop, node.size_cells);
            } else if (prop_name(prop, "compatible")) {
                if (depth == 1) {
                    heaven.testament = testament_from_compatible(prop);
                }
                node.electro_master_kind = electro_master_kind_from_compatible(prop);
                node.is_framebuffer = compatible_has(prop, "simple-framebuffer");
                node.is_psci = node.is_psci || is_psci_compatible(prop);
                if (is_gicv3_compatible(prop)) {
                    node.is_gic = true;
                    node.is_gicv3 = true;
                } else if (is_gicv2_compatible(prop)) {
                    node.is_gic = true;
                }
                node.is_pcie = node.is_pcie || is_pcie_compatible(prop);
            } else if (prop_name(prop, "method")) {
                node.psci_conduit = psci_conduit_from_method(prop);
            } else if (prop_name(prop, "device_type") && bytes_eq_text(prop, "memory")) {
                node.is_memory = true;
            } else if (prop_name(prop, "reg")) {
                const uint32_t address_bytes = parent_address_cells * 4;
                const uint32_t size_bytes = parent_size_cells * 4;
                const uint32_t tuple_bytes = address_bytes + size_bytes;
                uint32_t offset = 0;
                while (tuple_bytes != 0 && offset + tuple_bytes <= prop.len) {
                    const uint64_t reg_base = read_cells(prop.data + offset, parent_address_cells);
                    const uint64_t reg_size = read_cells(prop.data + offset + address_bytes,
                                                         parent_size_cells);
                    add_reg(node, reg_base, reg_size);
                    offset += tuple_bytes;
                }
            } else if (prop_name(prop, "reg-shift")) {
                node.reg_shift = prop_u32(prop, 0);
            } else if (prop_name(prop, "width")) {
                node.width = prop_u32(prop, 0);
            } else if (prop_name(prop, "height")) {
                node.height = prop_u32(prop, 0);
            } else if (prop_name(prop, "stride")) {
                node.stride = prop_u32(prop, 0);
            }
            continue;
        }

        if (token == kNop) {
            continue;
        }

        if (token == kEnd) {
            break;
        }

        break;
    }

    apply_testament_fallbacks(heaven);
    return heaven;
}

const char *testament_kind_name(TestamentKind kind) {
    switch (kind) {
    case TestamentKind::apple_silicon:
        return "Apple Silicon";
    case TestamentKind::qemu_virt:
        return "UTM / QEMU virt";
    case TestamentKind::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace index
