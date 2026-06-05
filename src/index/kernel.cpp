#include <stddef.h>
#include <stdint.h>

#include "arch/aarch64/cpu.hpp"
#include "drivers/aleister.hpp"
#include "drivers/artificial_heaven_canvas.hpp"
#include "drivers/electro_master.hpp"
#include "drivers/misaka_mail.hpp"
#include "drivers/random_vector.hpp"
#include "drivers/stiyl_magnus.hpp"
#include "drivers/tsuchimikado.hpp"
#include "drivers/underground.hpp"
#include "drivers/underline.hpp"
#include "index/artificial_heaven.hpp"
#include "index/dark_matter.hpp"
#include "index/grimoire_fs.hpp"
#include "index/idol_theory.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/index_librorum_prohibitorum.hpp"
#include "index/last_order.hpp"
#include "index/lateran.hpp"
#include "index/misaka_network.hpp"
#include "index/necessarius.hpp"
#include "index/teleport.hpp"
#include "index/tree_diagram.hpp"
#include "index/types.hpp"

namespace {

void print_artificial_heaven(const index::ArtificialHeaven &heaven) {
    using namespace index;
    namespace district = imaginary_number_district;

    district::write("  EL                 : ");
    district::dec(arch::current_el());
    district::write("\n  Testament          : ");
    district::write(testament_kind_name(heaven.testament));
    district::write("\n  DTB                : ");
    district::hex(heaven.dtb_addr);
    district::write(" + ");
    district::hex(heaven.dtb_size);
    district::write("\n  ElectroMaster      : ");
    district::write(drivers::electro_master_kind_name(heaven.electro_master.kind));
    district::write(" @ ");
    district::hex(heaven.electro_master.base);
    district::write("\n  Othinus (PSCI)     : ");
    if (heaven.othinus.available) {
        district::write("ready via ");
        district::write(drivers::psci_conduit_name(heaven.othinus.conduit));
    } else {
        district::write("absent");
    }
    const bool gic_v3 = heaven.aleister.version == index::drivers::GicVersion::v3;
    district::write(gic_v3 ? "\n  Aleister (GICv3)   : " : "\n  Aleister (GICv2)   : ");
    if (heaven.aleister.available) {
        district::write("GICD ");
        district::hex(heaven.aleister.gicd);
        if (gic_v3) {
            district::write(" GICR ");
            district::hex(heaven.aleister.gicr);
        } else {
            district::write(" GICC ");
            district::hex(heaven.aleister.gicc);
        }
    } else {
        district::write("absent");
    }
    district::write("\n");

    district::write("  ImaginaryDistrict  : ");
    district::dec(heaven.memory_count);
    district::writeln(" range(s)");
    for (uint32_t i = 0; i < heaven.memory_count; ++i) {
        district::write("    [");
        district::dec(i);
        district::write("] ");
        district::hex(heaven.memory[i].base);
        district::write(" + ");
        district::hex(heaven.memory[i].size);
        district::write("\n");
    }

    district::write("  AIMDiffusionField  : ");
    district::dec(heaven.aim_field_count);
    district::writeln(" range(s)");
    for (uint32_t i = 0; i < heaven.aim_field_count; ++i) {
        district::write("    [");
        district::dec(i);
        district::write("] ");
        district::hex(heaven.aim_diffusion_field[i].base);
        district::write(" + ");
        district::hex(heaven.aim_diffusion_field[i].size);
        district::write("\n");
    }

    if (heaven.canvas.valid) {
        district::write("  ArtificialHeaven   : ");
        district::dec(heaven.canvas.width);
        district::write("x");
        district::dec(heaven.canvas.height);
        district::write(" stride ");
        district::dec(heaven.canvas.stride);
        district::write("\n");
    } else {
        district::writeln("  ArtificialHeaven   : not found");
    }
}

void print_grimoires(const index::IndexLibrorumProhibitorum &grimoires) {
    using namespace index;
    namespace district = imaginary_number_district;

    if (!grimoires.ready) {
        district::writeln("  IndexLibrorum      : sealed");
        return;
    }

    district::write("  IndexLibrorum      : ");
    district::hex(grimoires.start);
    district::write(" .. ");
    district::hex(grimoires.end);
    district::write(" (");
    district::dec(grimoires.available() / kib(1024));
    district::writeln(" MiB)");
}

void print_tree_diagram(const index::TreeDiagram &tree) {
    namespace district = index::imaginary_number_district;

    if (!tree.ready) {
        district::writeln("  TreeDiagram        : offline");
        return;
    }

    district::write("  TreeDiagram        : ");
    district::dec(tree.available_pages());
    district::write(" / ");
    district::dec(tree.total_pages);
    district::writeln(" pages available");
}

} // namespace

extern "C" void kmain(uint64_t dtb_addr, uint64_t image_start, uint64_t image_end) {
    index::ArtificialHeaven heaven = index::construct_artificial_heaven(dtb_addr);
    index::imaginary_number_district::init(heaven.electro_master);
    index::imaginary_number_district::banner();

    print_artificial_heaven(heaven);

    index::Teleport teleport = index::teleport_enable(heaven);
    index::imaginary_number_district::write("  Teleport (MMU)     : ");
    if (teleport.enabled) {
        index::imaginary_number_district::write("on, ");
        index::imaginary_number_district::dec(teleport.va_bits);
        index::imaginary_number_district::write("-bit VA, ");
        index::imaginary_number_district::dec(teleport.blocks);
        index::imaginary_number_district::write(" GiB mapped (PA ");
        index::imaginary_number_district::dec(teleport.pa_bits);
        index::imaginary_number_district::write("-bit)");
        if (teleport.wx_protected) {
            index::imaginary_number_district::write(", kernel W^X");
        }
        index::imaginary_number_district::write("\n");
    } else {
        index::imaginary_number_district::writeln("off");
    }

    index::IndexLibrorumProhibitorum grimoires;
    grimoires.init(heaven, image_end);
    print_grimoires(grimoires);

    static index::TreeDiagram tree;
    tree.init(heaven, image_start, image_end, grimoires.start, grimoires.end);
    index::tree_diagram_set_global(&tree);
    print_tree_diagram(tree);

    void *first_page = grimoires.allocate(index::kib(4), index::kib(4));
    index::imaginary_number_district::write("  first page         : ");
    index::imaginary_number_district::hex(reinterpret_cast<uint64_t>(first_page));
    index::imaginary_number_district::write("\n");

    index::drivers::othinus_install(heaven.othinus);
    index::drivers::aleister_init(heaven.aleister);
    // PL011 RX interrupt -- so a parked Esper wakes within microseconds of a
    // keystroke instead of waiting up to one timer tick (~10 ms). Must run
    // after aleister_init (we route the SPI through it).
    index::imaginary_number_district::enable_rx_irq();
    static index::LastOrder clock;
    index::last_order_init(clock, 100);
    index::idol_theory_init(); // PL031 RTC; LastOrder must be ticking already
    index::misaka_network_init();
    // Wake the other cores (IRQs still masked here, so the boot core is not
    // preempted mid-bring-up); each secondary then runs its own idle and timer.
    index::misaka_network_bring_up_secondaries(heaven, heaven.othinus);
    index::drivers::tsuchimikado_start(); // QGA / vdagent agent Sister
    index::arch::enable_irq();

    index::imaginary_number_district::write("  LastOrder (timer)  : ");
    if (clock.armed) {
        index::imaginary_number_district::dec(clock.hz);
        index::imaginary_number_district::write(" Hz, CNTFRQ ");
        index::imaginary_number_district::dec(clock.frequency);
        index::imaginary_number_district::write(" Hz\n");
    } else {
        index::imaginary_number_district::writeln("offline");
    }

    index::imaginary_number_district::write("  MisakaNetwork      : ");
    index::imaginary_number_district::dec(index::misaka_network_count());
    index::imaginary_number_district::writeln(" sister(s), preemptive round-robin");

    index::imaginary_number_district::write("  MisakaNetwork SMP  : ");
    index::imaginary_number_district::dec(index::misaka_network_online_cpus());
    index::imaginary_number_district::write(" / ");
    index::imaginary_number_district::dec(heaven.cpu_count);
    index::imaginary_number_district::writeln(" core(s) online");

    index::dark_matter_init();
    index::imaginary_number_district::write("  DarkMatter (heap)  : ");
    index::imaginary_number_district::dec(index::dark_matter_stats().arena_size / index::kib(1));
    index::imaginary_number_district::writeln(" KiB arena, kmalloc/kfree ready");

    index::drivers::underground_init(heaven.pcie_ecam); // PCIe host bridge for the PCI disk path
    index::drivers::Underline disk = index::drivers::underline_probe();
    index::imaginary_number_district::write("  Underline (disk)   : ");
    if (disk.present) {
        index::imaginary_number_district::dec(disk.capacity_sectors);
        index::imaginary_number_district::writeln(" sector(s), virtio-blk ready");
    } else {
        index::imaginary_number_district::writeln("no block device attached");
    }

    const bool fs_mounted = index::lateran_mount();
    index::imaginary_number_district::write("  Lateran (");
    index::imaginary_number_district::write(index::lateran_format());
    index::imaginary_number_district::write(")     : ");
    if (fs_mounted) {
        index::LateranEntry entries[16];
        index::imaginary_number_district::dec(index::lateran_list(entries, 16));
        index::imaginary_number_district::writeln(" entr(ies) on the disk");
    } else {
        index::imaginary_number_district::writeln("no recognised volume");
    }

    // Mount real in-memory tmpfs (Testament) over the standard volatile dirs,
    // shadowing the baked-in ext2 placeholders -- Linux init does this via fstab.
    // RAM-backed, so /tmp + /dev/shm are fast + truly volatile (gone on reboot).
    index::lateran_tmpfs_mount("/tmp");
    index::lateran_tmpfs_mount("/dev/shm");

    index::drivers::MisakaMail net = index::drivers::misaka_mail_probe();
    index::imaginary_number_district::write("  MisakaMail (net)   : ");
    if (net.present) {
        const char *hex = "0123456789abcdef";
        for (uint32_t i = 0; i < 6; ++i) {
            if (i) {
                index::imaginary_number_district::putc(':');
            }
            index::imaginary_number_district::putc(hex[net.mac[i] >> 4]);
            index::imaginary_number_district::putc(hex[net.mac[i] & 0xf]);
        }
        index::imaginary_number_district::write(" IP ");
        for (uint32_t i = 0; i < 4; ++i) {
            if (i) {
                index::imaginary_number_district::putc('.');
            }
            index::imaginary_number_district::dec(net.ip[i]);
        }
        index::imaginary_number_district::writeln("");
    } else {
        index::imaginary_number_district::writeln("no NIC found");
    }

    index::drivers::RandomVector rng = index::drivers::random_vector_probe();
    index::imaginary_number_district::write("  RandomVector (rng) : ");
    if (rng.present) {
        index::imaginary_number_district::writeln("virtio-rng ready");
    } else {
        index::imaginary_number_district::writeln("absent (PRNG fallback)");
    }

    // Tsuchimikado: virtio-serial agent stub. Acks PORT_READY + PORT_OPEN so
    // the host-side QGA / SPICE vdagent see a "connected" guest and stop
    // polling. The single largest contributor to QEMU host CPU under UTM is
    // these channels retrying when no daemon is present.
    index::drivers::Tsuchimikado tsuchi = index::drivers::tsuchimikado_probe();
    index::imaginary_number_district::write("  Tsuchimikado(serial): ");
    if (tsuchi.present) {
        index::imaginary_number_district::write("virtio-serial ack'd, ");
        index::imaginary_number_district::dec(tsuchi.num_ports);
        index::imaginary_number_district::writeln(" port(s)");
    } else {
        index::imaginary_number_district::writeln("absent");
    }

    // StiylMagnus: virtio-9p (9P2000.L) client. If QEMU exports a host
    // directory via `-fsdev local,...,mount_tag=hostshare`, the guest sees it
    // live under /host -- edit on the host, see it in Index without rebuilding
    // ext2.img. Necessarius's Stiyl Magnus shuttles fire and word between the
    // Church and the outside world; this driver does the same for files.
    index::drivers::StiylMagnus stiyl = index::drivers::stiyl_magnus_probe();
    index::imaginary_number_district::write("  StiylMagnus(9p)   : ");
    if (stiyl.present) {
        index::imaginary_number_district::write("9P2000.L bound, msize ");
        index::imaginary_number_district::dec(stiyl.msize);
        index::imaginary_number_district::writeln(" B, /host attached");
    } else {
        index::imaginary_number_district::writeln("absent");
    }

    // Quiesce every PCI device we don't drive (xHCI, HD audio, ...). UTM ships
    // a USB controller stack + audio for SPICE redirect; if their emulators
    // are left running they keep polling host-side at the device's intrinsic
    // rate even with no guest driver attached, which under HVF burns ~100% of
    // one host core per controller. Mainstream OSes get this for free via
    // their driver model (Linux halts xhci, sets HDA to D3 if unused). Must
    // run AFTER virtio probes so we don't accidentally disable them.
    index::drivers::underground_quiesce_unused();
    index::imaginary_number_district::writeln("  Underground quiesce: unused PCI devices halted");

    index::imaginary_number_district::write("  GrimoireFS         : ");
    index::imaginary_number_district::dec(index::grimoire_fs_count());
    index::imaginary_number_district::writeln(" grimoire(s) on the Bookshelf");

    index::drivers::draw_index_glyph(heaven.canvas);
    index::imaginary_number_district::writeln("Index is alive; entering Necessarius.");

    // Teleport to the higher half: from here the shell, scheduler, and IRQ
    // handlers run at kernel VAs (TTBR1). Raise the exception vectors to their
    // high alias, then call the shell through its high VA. The identity alias
    // (TTBR0) stays live, so device MMIO and physical thread stacks still work.
    if (teleport.enabled) {
        uint64_t vbar = 0;
        asm volatile("mrs %0, vbar_el1" : "=r"(vbar));
        vbar = index::teleport_high_alias(vbar);
        asm volatile("msr vbar_el1, %0; isb" ::"r"(vbar));

        using ShellFn = void (*)(const index::ArtificialHeaven &,
                                 index::IndexLibrorumProhibitorum &,
                                 index::TreeDiagram &);
        ShellFn shell = &index::enter_necessarius;
        shell = reinterpret_cast<ShellFn>(
            index::teleport_high_alias(reinterpret_cast<uint64_t>(shell)));
        shell(heaven, grimoires, tree); // jumps to the high half; never returns
    }

    index::enter_necessarius(heaven, grimoires, tree);
}

extern "C" char __text_start[];
extern "C" char __text_end[];

// Set on the first kernel exception so the 100 Hz [WD] dump in network_tick
// goes quiet -- the backtrace below is then the LAST thing on the console
// (easy to find/copy instead of being buried under recurring [WD] blocks).
extern "C" volatile bool g_panicked = false;

extern "C" void exception_report(uint64_t kind, uint64_t esr, uint64_t elr,
                                  uint64_t far, uint64_t spsr, uint64_t lr,
                                  uint64_t fp, uint64_t sp) {
    using namespace index;
    namespace district = imaginary_number_district;
    g_panicked = true; // hush the periodic [WD] dump so this trace stands alone

    district::write("\n====[EXCEPTION]==== vector ");
    district::dec(kind);
    district::write(" cpu ");
    district::dec(arch::this_cpu_id());
    district::write("\n  ESR  "); district::hex(esr);
    district::write("\n  ELR  "); district::hex(elr);
    district::write("\n  FAR  "); district::hex(far);
    district::write("\n  SPSR "); district::hex(spsr);
    district::write("\n  LR   "); district::hex(lr);
    district::write("\n  FP   "); district::hex(fp);
    district::write("\n  SP   "); district::hex(sp);

    const uint64_t tlo = reinterpret_cast<uint64_t>(__text_start);
    const uint64_t thi = reinterpret_cast<uint64_t>(__text_end);
    district::write("\n  ktext ["); district::hex(tlo);
    district::write(","); district::hex(thi); district::write(")");

    // Kernel stacks live in the high-half (TTBR1) mapping of RAM
    // [0x40000000,0x80000000). Bound EVERY dereference to that window so the
    // unwinder can't itself fault on a corrupted fp/sp (that would recurse).
    // A crashing secondary core can be on a LOW (identity) stack -- SP/FP land
    // in physical RAM [0x40000000,0x80000000), not the high-half. Accept both by
    // checking VA[39:0] is in RAM, and ALWAYS dereference through the high-half
    // alias (TTBR1 maps RAM there regardless of which TTBR0 is live), so the
    // unwinder can't fault on a low pointer / wrong user TTBR0.
    auto inram = [](uint64_t a) {
        const uint64_t lo = a & 0x000000FFFFFFFFFFULL;
        return (a & 7u) == 0 && lo >= 0x40000000ULL && lo + 16 <= 0x80000000ULL;
    };
    auto hh = [](uint64_t a) {        // high-half alias of a RAM address, for reads
        return 0xFFFFFF8000000000ULL | (a & 0x000000FFFFFFFFFFULL);
    };
    auto rd = [&](uint64_t a) { return *reinterpret_cast<const uint64_t *>(hh(a)); };
    auto ktext = [&](uint64_t a) {     // match kernel text by VA[38:0] (identity==high)
        const uint64_t m = 0x0000007FFFFFFFFFULL;
        return (a & m) >= (tlo & m) && (a & m) < (thi & m);
    };

    // FP-chain unwind (arm64 unwind_frame: [fp]=prev fp, [fp+8]=return addr).
    district::write("\n  [fp-chain backtrace]");
    uint64_t f = fp;
    for (int i = 0; i < 24 && inram(f); ++i) {
        const uint64_t ret = rd(f + 8);
        const uint64_t nxt = rd(f);
        district::write("\n    ret="); district::hex(ret);
        if (ktext(ret)) district::write(" <ktext>");
        if (nxt <= f) break; // FP must grow toward higher addresses
        f = nxt;
    }
    // Raw stack dump @SP (find return addresses even if fp was smashed).
    district::write("\n  [raw stack @SP]");
    for (uint64_t n = 0; n < 32 && inram(sp + n * 8); ++n) {
        const uint64_t w = rd(sp + n * 8);
        district::write("\n    +"); district::dec(n * 8); district::write(": ");
        district::hex(w);
        if (ktext(w)) district::write(" <ktext>");
    }
    district::write("\n====[/EXCEPTION]====\n");

    arch::halt();
}

extern "C" void *__dso_handle = nullptr;

extern "C" void __cxa_pure_virtual() {
    index::arch::halt();
}

extern "C" void *memset(void *dest, int value, size_t count) {
    auto *p = static_cast<uint8_t *>(dest);
    for (size_t i = 0; i < count; ++i) {
        p[i] = static_cast<uint8_t>(value);
    }
    return dest;
}

extern "C" void *memcpy(void *dest, const void *src, size_t count) {
    auto *d = static_cast<uint8_t *>(dest);
    const auto *s = static_cast<const uint8_t *>(src);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

extern "C" void *memmove(void *dest, const void *src, size_t count) {
    auto *d = static_cast<uint8_t *>(dest);
    const auto *s = static_cast<const uint8_t *>(src);
    if (d < s) {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}
