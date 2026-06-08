#include "index/necessarius.hpp"

#include "arch/aarch64/cpu.hpp"
#include "drivers/artificial_heaven_canvas.hpp"
#include "drivers/electro_master.hpp"
#include "drivers/misaka_mail.hpp"
#include "drivers/othinus.hpp"
#include "drivers/underline.hpp"
#include "index/bookshelf.hpp"
#include "index/dark_matter.hpp"
#include "index/esper.hpp"
#include "index/grimoire_fs.hpp"
#include "index/imagine_breaker.hpp"
#include "index/lateran.hpp"
#include "index/usermode.hpp"
#include "index/antenna.hpp"
#include "index/dhcp.hpp"
#include "index/dns.hpp"
#include "index/last_order.hpp"
#include "index/misaka_network.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/teleport.hpp"
#include "index/tree_diagram.hpp"
#include "index/types.hpp"

// Start of the read-only executable .text segment (from linker.ld).
extern "C" char __text_start[];

namespace index {

namespace {

constexpr uint32_t kLineSize = 256;

bool streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// If `line` begins with `prefix`, return the pointer just past it (the
// argument); otherwise nullptr. Used for commands that take an operand.
const char *arg_after(const char *line, const char *prefix) {
    while (*prefix) {
        if (*line == 0 || *line != *prefix) {
            return nullptr;
        }
        ++line;
        ++prefix;
    }
    return line;
}

void prompt() {
    imaginary_number_district::write("Index> ");
}

// Parse "a.b.c.d" into out[4]. Returns true on a well-formed dotted quad.
bool parse_ip(const char *s, uint8_t out[4]) {
    for (uint32_t i = 0; i < 4; ++i) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + uint32_t(*s - '0');
            ++s;
        }
        if (v > 255) {
            return false;
        }
        out[i] = uint8_t(v);
        if (i < 3) {
            if (*s != '.') {
                return false;
            }
            ++s;
        }
    }
    return true;
}

uint64_t parse_u64(const char *s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + static_cast<uint64_t>(*s - '0');
        ++s;
    }
    return v;
}

alignas(16) uint8_t g_sector_buf[drivers::kUnderlineSectorSize];
char g_fat_buf[2048]; // scratch for printing a Lateran (FAT) file

void dump_sector(uint64_t sector) {
    using namespace imaginary_number_district;

    const drivers::Underline &disk = drivers::underline_status();
    if (!disk.present) {
        writeln("Underline: no block device attached.");
        return;
    }
    if (sector >= disk.capacity_sectors) {
        writeln("sector out of range.");
        return;
    }
    if (!drivers::underline_read(disk, sector, g_sector_buf)) {
        writeln("Underline: read failed.");
        return;
    }

    write("sector ");
    dec(sector);
    write(" first 64 bytes:\n");
    for (uint32_t i = 0; i < 64; ++i) {
        const uint8_t c = g_sector_buf[i];
        putc((c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '.');
    }
    write("\n");
}

void print_help() {
    using namespace imaginary_number_district;

    writeln("Necessarius commands:");
    writeln("  help    show this list");
    writeln("  status  show EL, ElectroMaster, ArtificialHeaven canvas");
    writeln("  mem     show Imaginary Number District memory ranges");
    writeln("  dtb     show device tree location");
    writeln("  heaven  redraw ArtificialHeaven canvas");
    writeln("  alloc   allocate one 4 KiB page from Index Librorum Prohibitorum");
    writeln("  tree    show TreeDiagram page ranges");
    writeln("  page    allocate one physical page from TreeDiagram");
    writeln("  uptime  show LastOrder timer beats and elapsed seconds");
    writeln("  sisters show MisakaNetwork threads and their counters");
    writeln("  smp     show online cores and the Sister each is running");
    writeln("  spawn   spawn a busy worker Sister into MisakaNetwork");
    writeln("  sleeper spawn a Sister that sleeps ~1s between counter bumps");
    writeln("  prodcons spawn a producer/consumer pair sharing an Imprimatur");
    writeln("  levels  spawn Lv4 + Lv2 workers to show priority scheduling");
    writeln("  judgement spawn two Sisters contending on a Judgement mutex");
    writeln("  invert  priority-inversion demo fixed by Level Upper (alias pi)");
    writeln("  radio   sender/receiver over a RadioNoise mailbox (alias mailbox)");
    writeln("  heap    exercise the DarkMatter heap (alloc/free/reuse/coalesce)");
    writeln("  user    drop to EL0 and run a user program via syscalls");
    writeln("  userfault run an EL0 program that faults; kernel survives");
    writeln("  komoe   an EL0 shell: fork/exec/wait + read/write/open/close");
    writeln("  net     MisakaMail networking: 'net', 'net ping [ip]', 'net serve', 'net ip a.b.c.d'");
    writeln("  exec <n>  load a PIE ELF from the Lateran disk and run it at EL0");
    writeln("  coexec <a> <b>  run several ELF programs concurrently (cooperative)");
    writeln("  ps      list Espers (user processes): pid, state, exit code");
    writeln("  ls      list files (GrimoireFS [ro] + Bookshelf [rw] + Lateran [fat])");
    writeln("  cat <n> print file <n> (Bookshelf / Lateran disk / GrimoireFS)");
    writeln("  write <n> <text>  create/overwrite Bookshelf file <n>");
    writeln("  rm <n>  delete Bookshelf file <n>");
    writeln("  disk [s]  read sector s (default 0) from the Underline disk");
    writeln("  diskwrite write+readback a scratch sector (tests virtio-blk write)");
    writeln("  load <n> <s>  read disk sector s into Bookshelf file <n>");
    writeln("  wxtest  write to .text to prove W^X (faults + halts)");
    writeln("  halt    enter idle loop");
    writeln("  poweroff power the UTM VM off via Othinus (PSCI SYSTEM_OFF)");
    writeln("  reboot   reset the UTM VM via Othinus (PSCI SYSTEM_RESET)");
    writeln("  imagine trip Imagine Breaker");
}

void print_status(const ArtificialHeaven &heaven) {
    using namespace imaginary_number_district;

    write("EL: ");
    dec(arch::current_el());
    write("\nTestament: ");
    write(testament_kind_name(heaven.testament));
    write("\nElectroMaster: ");
    write(drivers::electro_master_kind_name(heaven.electro_master.kind));
    write(" @ ");
    hex(heaven.electro_master.base);
    write("\nOthinus (PSCI): ");
    if (heaven.othinus.available) {
        write("ready via ");
        write(drivers::psci_conduit_name(heaven.othinus.conduit));
    } else {
        write("absent");
    }
    write("\nTeleport (MMU): ");
    if (teleport_status().enabled) {
        write("on, ");
        dec(teleport_status().va_bits);
        write("-bit VA, ");
        dec(teleport_status().blocks);
        write(" GiB mapped");
        if (teleport_status().wx_protected) {
            write(", kernel W^X");
        }
    } else {
        write("off");
    }
    write("\nArtificialHeaven canvas: ");
    if (heaven.canvas.valid) {
        dec(heaven.canvas.width);
        write("x");
        dec(heaven.canvas.height);
        write(" stride ");
        dec(heaven.canvas.stride);
        write("\n");
    } else {
        writeln("not found");
    }
}

void print_memory(const ArtificialHeaven &heaven, const IndexLibrorumProhibitorum &grimoires) {
    using namespace imaginary_number_district;

    write("ImaginaryNumberDistrict ranges: ");
    dec(heaven.memory_count);
    write("\n");
    for (uint32_t i = 0; i < heaven.memory_count; ++i) {
        write("  [");
        dec(i);
        write("] ");
        hex(heaven.memory[i].base);
        write(" + ");
        hex(heaven.memory[i].size);
        write("\n");
    }

    write("AIMDiffusionField ranges: ");
    dec(heaven.aim_field_count);
    write("\n");
    for (uint32_t i = 0; i < heaven.aim_field_count; ++i) {
        write("  [");
        dec(i);
        write("] ");
        hex(heaven.aim_diffusion_field[i].base);
        write(" + ");
        hex(heaven.aim_diffusion_field[i].size);
        write("\n");
    }

    write("IndexLibrorumProhibitorum: ");
    if (!grimoires.ready) {
        writeln("sealed");
        return;
    }

    hex(grimoires.start);
    write(" .. ");
    hex(grimoires.end);
    write(" used ");
    dec(grimoires.used());
    write(" bytes, available ");
    dec(grimoires.available());
    writeln(" bytes");
}

void print_tree_diagram(const TreeDiagram &tree) {
    using namespace imaginary_number_district;

    write("TreeDiagram: ");
    if (!tree.ready) {
        writeln("offline");
        return;
    }

    dec(tree.available_pages());
    write(" / ");
    dec(tree.total_pages);
    write(" pages available\n");

    for (uint32_t i = 0; i < tree.range_count; ++i) {
        const TreeDiagramRange &range = tree.ranges[i];
        write("  [");
        dec(i);
        write("] ");
        hex(range.start);
        write(" .. ");
        hex(range.end);
        write(" next ");
        hex(range.current);
        write("\n");
    }
}

void print_dtb(const ArtificialHeaven &heaven) {
    using namespace imaginary_number_district;

    write("DTB: ");
    hex(heaven.dtb_addr);
    write(" + ");
    hex(heaven.dtb_size);
    write("\n");
}

void execute(const char *line, const ArtificialHeaven &heaven,
             IndexLibrorumProhibitorum &grimoires, TreeDiagram &tree) {
    using namespace imaginary_number_district;

    if (line[0] == 0) {
        return;
    }
    if (const char *name = arg_after(line, "cat ")) {
        if (const BookshelfFile *f = bookshelf_find(name)) {
            write(f->data);
            write("\n");
        } else if (const int64_t n = lateran_read_file(name, g_fat_buf, sizeof(g_fat_buf) - 1);
                   n >= 0) {
            g_fat_buf[n] = 0;
            write(g_fat_buf);
        } else if (const Grimoire *g = grimoire_fs_find(name)) {
            write(g->text);
        } else {
            write("No such file: ");
            writeln(name);
        }
        return;
    }
    if (const char *rest = arg_after(line, "write ")) {
        char namebuf[32];
        uint32_t n = 0;
        while (rest[n] != 0 && rest[n] != ' ' && n + 1 < sizeof(namebuf)) {
            namebuf[n] = rest[n];
            ++n;
        }
        namebuf[n] = 0;
        const char *text = rest + n;
        while (*text == ' ') {
            ++text;
        }
        if (namebuf[0] == 0) {
            writeln("usage: write <name> <text>");
        } else if (bookshelf_write(namebuf, text)) {
            write("wrote ");
            writeln(namebuf);
        } else {
            writeln("Bookshelf full or out of heap.");
        }
        return;
    }
    if (const char *name = arg_after(line, "rm ")) {
        if (bookshelf_remove(name)) {
            write("removed ");
            writeln(name);
        } else {
            write("no such file: ");
            writeln(name);
        }
        return;
    }
    if (const char *arg = arg_after(line, "disk ")) {
        dump_sector(parse_u64(arg));
        return;
    }
    if (const char *name = arg_after(line, "exec ")) {
        run_elf(name);
        return;
    }
    if (const char *rest = arg_after(line, "linuxrun ")) {
        // linuxrun <path> [args...] : spawn a Linux ELF with a real argv.
        // Tokenise on whitespace but keep "..."/'...' quoted strings together
        // so sh-style payloads like `sh -c "echo a; echo b"` reach the new
        // process intact. Quotes are stripped from the resulting argv entries.
        static char buf[kLineSize];
        uint32_t bn = 0;
        while (rest[bn] != 0 && bn + 1 < sizeof(buf)) { buf[bn] = rest[bn]; ++bn; }
        buf[bn] = 0;
        const char *argv[kExecArgvCap] = {};
        uint32_t argc = 0;
        char *p = buf;
        while (*p == ' ') ++p;
        while (*p != 0 && argc < kExecArgvCap) {
            char quote = 0;
            if (*p == '"' || *p == '\'') { quote = *p; ++p; }
            argv[argc++] = p;
            if (quote != 0) {
                while (*p != 0 && *p != quote) ++p;
                if (*p == quote) *p++ = 0;
            } else {
                while (*p != 0 && *p != ' ') ++p;
                if (*p == ' ') *p++ = 0;
            }
            while (*p == ' ') ++p;
        }
        if (argc == 0) {
            writeln("usage: linuxrun <prog> [args...]");
            return;
        }
        // argv[0] is also the path to load. busybox-style binaries get their
        // applet from argv[0], so passing the same path keeps "/bin/busybox"
        // running as "busybox".
        run_elf_argv(argv[0], argv, argc, nullptr, 0);
        return;
    }
    if (const char *rest = arg_after(line, "net")) {
        while (*rest == ' ') {
            ++rest;
        }
        if (*rest == 0) {
            drivers::misaka_mail_report();
        } else if (const char *ip = arg_after(rest, "ping")) {
            while (*ip == ' ') {
                ++ip;
            }
            uint8_t addr[4];
            if (*ip == 0) {
                drivers::misaka_mail_ping(nullptr); // default: gateway
            } else if (parse_ip(ip, addr)) {
                drivers::misaka_mail_ping(addr);
            } else {
                writeln("usage: net ping [a.b.c.d]");
            }
        } else if (streq(rest, "serve")) {
            drivers::misaka_mail_serve();
        } else if (streq(rest, "dhcp")) {
            // net dhcp: lease an IP from the DHCP server (SLIRP serves at
            // 10.0.2.2). Updates our IP and the DNS resolver if offered.
            dhcp_acquire();
        } else if (const char *host = arg_after(rest, "resolve ")) {
            // net resolve <name>: UDP DNS A-record lookup via the current
            // resolver (default 10.0.2.3).
            while (*host == ' ') ++host;
            if (*host == 0) { writeln("usage: net resolve <hostname>"); return; }
            uint8_t ip[4];
            if (dns_resolve(host, ip)) {
                write("net resolve: ");
                write(host);
                write(" -> ");
                dec(ip[0]); putc('.');
                dec(ip[1]); putc('.');
                dec(ip[2]); putc('.');
                dec(ip[3]); writeln("");
            } else {
                write("net resolve: no answer for ");
                writeln(host);
            }
        } else if (streq(rest, "dns")) {
            dns_report();
        } else if (const char *a = arg_after(rest, "ip ")) {
            uint8_t addr[4];
            if (parse_ip(a, addr)) {
                drivers::misaka_mail_set_ip(addr[0], addr[1], addr[2], addr[3]);
                drivers::misaka_mail_report();
            } else {
                writeln("usage: net ip a.b.c.d");
            }
        } else if (const char *u = arg_after(rest, "udp ")) {
            // net udp a.b.c.d port "payload": open an Antenna, sendto.
            while (*u == ' ') ++u;
            uint8_t addr[4];
            if (!parse_ip(u, addr)) { writeln("usage: net udp a.b.c.d port text"); return; }
            while (*u && *u != ' ') ++u;
            while (*u == ' ') ++u;
            const uint64_t port = parse_u64(u);
            if (port == 0 || port > 65535) { writeln("usage: net udp a.b.c.d port text"); return; }
            while (*u && *u != ' ') ++u;
            while (*u == ' ') ++u;
            const char *payload = u;
            uint32_t plen = 0;
            while (payload[plen] != 0) ++plen;
            const int sock = antenna_socket_udp();
            if (sock < 0) { writeln("net udp: no socket slot"); return; }
            antenna_bind(sock, 0);
            const int64_t n = antenna_sendto(sock, addr, static_cast<uint16_t>(port),
                                             reinterpret_cast<const uint8_t *>(payload),
                                             plen);
            antenna_close(sock);
            if (n < 0) {
                writeln("net udp: send failed");
            } else {
                write("net udp: sent ");
                imaginary_number_district::dec(static_cast<uint64_t>(n));
                writeln(" bytes.");
            }
        } else if (const char *p = arg_after(rest, "udprecv ")) {
            // net udprecv port: bind a socket, pump RX, print one datagram.
            const uint64_t port = parse_u64(p);
            if (port == 0 || port > 65535) { writeln("usage: net udprecv port"); return; }
            const int sock = antenna_socket_udp();
            if (sock < 0) { writeln("net udprecv: no slot"); return; }
            if (!antenna_bind(sock, static_cast<uint16_t>(port))) {
                writeln("net udprecv: bind failed");
                antenna_close(sock);
                return;
            }
            write("net udprecv: listening on :"); writeln(p);
            uint8_t buf[1500];
            uint8_t src_ip[4];
            uint16_t src_port = 0;
            const int64_t n = antenna_recvfrom(sock, buf, sizeof(buf), src_ip,
                                               &src_port, 1000); // ~10s @ 100Hz
            if (n <= 0) {
                writeln("net udprecv: no datagram (timeout).");
            } else {
                write("net udprecv: ");
                imaginary_number_district::dec(static_cast<uint64_t>(n));
                write(" bytes from ");
                imaginary_number_district::dec(src_ip[0]); write(".");
                imaginary_number_district::dec(src_ip[1]); write(".");
                imaginary_number_district::dec(src_ip[2]); write(".");
                imaginary_number_district::dec(src_ip[3]); write(":");
                imaginary_number_district::dec(src_port);
                write(" : ");
                for (int64_t i = 0; i < n; ++i) imaginary_number_district::putc(static_cast<char>(buf[i]));
                writeln("");
            }
            antenna_close(sock);
        } else if (streq(rest, "sockets") || streq(rest, "ants")) {
            antenna_report();
        } else if (const char *lp = arg_after(rest, "tcplisten ")) {
            // net tcplisten port: bind a TCP socket, listen, accept ONE
            // connection, echo whatever it sends back, close.
            const uint64_t port = parse_u64(lp);
            if (port == 0 || port > 65535) { writeln("usage: net tcplisten port"); return; }
            const int sock = antenna_socket_tcp();
            if (sock < 0) { writeln("net tcplisten: no slot"); return; }
            if (!antenna_bind(sock, static_cast<uint16_t>(port)) ||
                !antenna_tcp_listen(sock, 4)) {
                writeln("net tcplisten: bind/listen failed");
                antenna_close(sock);
                return;
            }
            write("net tcplisten: listening on :"); writeln(lp);
            uint8_t peer_ip[4] = {};
            uint16_t peer_port = 0;
            const int child = antenna_tcp_accept(sock, peer_ip, &peer_port, 1500);
            if (child < 0) {
                writeln("net tcplisten: accept timed out.");
                antenna_close(sock);
                return;
            }
            write("net tcplisten: accepted from ");
            imaginary_number_district::dec(peer_ip[0]); write(".");
            imaginary_number_district::dec(peer_ip[1]); write(".");
            imaginary_number_district::dec(peer_ip[2]); write(".");
            imaginary_number_district::dec(peer_ip[3]); write(":");
            imaginary_number_district::dec(peer_port);
            writeln("");
            uint8_t rbuf[1500];
            const int64_t r = antenna_tcp_recv(child, rbuf, sizeof(rbuf), 500);
            if (r > 0) {
                write("net tcplisten: got ");
                imaginary_number_district::dec(static_cast<uint64_t>(r));
                write(" bytes: ");
                for (int64_t i = 0; i < r; ++i)
                    imaginary_number_district::putc(static_cast<char>(rbuf[i]));
                writeln("");
                static const char prefix[] = "echo:";
                antenna_tcp_send(child, reinterpret_cast<const uint8_t *>(prefix), 5);
                antenna_tcp_send(child, rbuf, static_cast<uint32_t>(r));
            }
            antenna_close(child);
            antenna_close(sock);
        } else if (const char *t = arg_after(rest, "tcp ")) {
            // net tcp a.b.c.d port text: connect, send, read once, close.
            while (*t == ' ') ++t;
            uint8_t addr[4];
            if (!parse_ip(t, addr)) { writeln("usage: net tcp a.b.c.d port text"); return; }
            while (*t && *t != ' ') ++t;
            while (*t == ' ') ++t;
            const uint64_t port = parse_u64(t);
            if (port == 0 || port > 65535) { writeln("usage: net tcp a.b.c.d port text"); return; }
            while (*t && *t != ' ') ++t;
            while (*t == ' ') ++t;
            const char *payload = t;
            uint32_t plen = 0;
            while (payload[plen] != 0) ++plen;
            const int sock = antenna_socket_tcp();
            if (sock < 0) { writeln("net tcp: no slot"); return; }
            if (!antenna_tcp_connect(sock, addr, static_cast<uint16_t>(port))) {
                writeln("net tcp: connect failed (timeout/RST)");
                antenna_close(sock);
                return;
            }
            writeln("net tcp: connected.");
            if (plen > 0) {
                const int64_t w = antenna_tcp_send(sock, reinterpret_cast<const uint8_t *>(payload), plen);
                if (w < 0) { writeln("net tcp: send failed"); antenna_close(sock); return; }
                write("net tcp: sent ");
                imaginary_number_district::dec(static_cast<uint64_t>(w));
                writeln(" bytes.");
            }
            uint8_t rbuf[1500];
            const int64_t r = antenna_tcp_recv(sock, rbuf, sizeof(rbuf) - 1, 300);
            if (r > 0) {
                rbuf[r] = 0;
                write("net tcp: received ");
                imaginary_number_district::dec(static_cast<uint64_t>(r));
                write(" bytes: ");
                for (int64_t i = 0; i < r; ++i) imaginary_number_district::putc(static_cast<char>(rbuf[i]));
                writeln("");
            } else {
                writeln("net tcp: no reply.");
            }
            antenna_close(sock);
        } else {
            writeln("usage: net | net ping [ip] | net serve | net ip a.b.c.d");
            writeln("       net udp a.b.c.d port text | net udprecv port | net sockets");
            writeln("       net tcp a.b.c.d port text | net tcplisten port");
        }
        return;
    }
    if (const char *names = arg_after(line, "coexec ")) {
        run_coexec(names);
        return;
    }
    if (const char *rest = arg_after(line, "load ")) {
        // load <name> <sector>: read a disk sector into a Bookshelf text file,
        // bridging the block device (Underline) to the writable FS (Bookshelf).
        char namebuf[32];
        uint32_t n = 0;
        while (rest[n] != 0 && rest[n] != ' ' && n + 1 < sizeof(namebuf)) {
            namebuf[n] = rest[n];
            ++n;
        }
        namebuf[n] = 0;
        const char *secstr = rest + n;
        while (*secstr == ' ') {
            ++secstr;
        }
        const drivers::Underline &disk = drivers::underline_status();
        if (namebuf[0] == 0) {
            writeln("usage: load <name> <sector>");
        } else if (!disk.present) {
            writeln("Underline: no block device attached.");
        } else if (!drivers::underline_read(disk, parse_u64(secstr), g_sector_buf)) {
            writeln("Underline: read failed.");
        } else {
            g_sector_buf[drivers::kUnderlineSectorSize - 1] = 0; // treat as text
            if (bookshelf_write(namebuf, reinterpret_cast<const char *>(g_sector_buf))) {
                write("loaded sector into ");
                writeln(namebuf);
            } else {
                writeln("Bookshelf full or out of heap.");
            }
        }
        return;
    }
    if (streq(line, "help")) {
        print_help();
    } else if (streq(line, "status")) {
        print_status(heaven);
    } else if (streq(line, "mem")) {
        print_memory(heaven, grimoires);
    } else if (streq(line, "dtb")) {
        print_dtb(heaven);
    } else if (streq(line, "heaven")) {
        drivers::draw_index_glyph(heaven.canvas);
        writeln("ArtificialHeaven canvas redrawn.");
    } else if (streq(line, "alloc")) {
        void *page = grimoires.allocate(kib(4), kib(4));
        write("IndexLibrorumProhibitorum page: ");
        hex(reinterpret_cast<uint64_t>(page));
        write("\n");
    } else if (streq(line, "tree")) {
        print_tree_diagram(tree);
    } else if (streq(line, "page")) {
        void *page = tree.allocate_page();
        write("TreeDiagram page: ");
        hex(reinterpret_cast<uint64_t>(page));
        write("\n");
    } else if (streq(line, "uptime") || streq(line, "ticks")) {
        const uint64_t beats = last_order_ticks();
        const uint32_t hz = last_order_hz();
        write("LastOrder beats: ");
        dec(beats);
        if (hz != 0) {
            write(" (");
            dec(beats / hz);
            write(" s at ");
            dec(hz);
            write(" Hz)");
        } else {
            write(" (timer offline)");
        }
        write("\n");
    } else if (streq(line, "sisters")) {
        misaka_network_report();
    } else if (streq(line, "smp")) {
        misaka_network_smp_report();
    } else if (streq(line, "spawn")) {
        if (misaka_network_spawn_demo()) {
            writeln("Spawned a worker Sister into MisakaNetwork.");
        } else {
            writeln("MisakaNetwork is full.");
        }
    } else if (streq(line, "sleeper")) {
        if (misaka_network_spawn_sleeper()) {
            writeln("Spawned a sleeper Sister into MisakaNetwork.");
        } else {
            writeln("MisakaNetwork is full.");
        }
    } else if (streq(line, "prodcons")) {
        if (misaka_network_spawn_prodcons()) {
            writeln("Spawned producer + consumer sharing an Imprimatur.");
        } else {
            writeln("Already running, or MisakaNetwork is full.");
        }
    } else if (streq(line, "levels")) {
        if (misaka_network_spawn_levels()) {
            writeln("Spawned lv4-high + lv2-low; watch lv4 outrun lv2.");
        } else {
            writeln("Already running, or MisakaNetwork is full.");
        }
    } else if (streq(line, "judgement") || streq(line, "judge")) {
        if (misaka_network_spawn_judgement()) {
            writeln("Spawned judge-A + judge-B contending on a Judgement lock.");
        } else {
            writeln("Already running, or MisakaNetwork is full.");
        }
    } else if (streq(line, "invert") || streq(line, "pi")) {
        if (misaka_network_spawn_priority_inversion()) {
            writeln("Spawned pi-low/pi-mid/pi-high; Level Upper resolves inversion.");
        } else {
            writeln("Already running, or not enough free slots.");
        }
    } else if (streq(line, "radio") || streq(line, "mailbox")) {
        if (misaka_network_spawn_radio()) {
            writeln("Spawned rn-send/rn-recv over a RadioNoise mailbox.");
        } else {
            writeln("Already running, or MisakaNetwork is full.");
        }
    } else if (streq(line, "disk")) {
        dump_sector(0);
    } else if (const char *fw = arg_after(line, "fwrite ")) {
        // fwrite <name> <text>: create/overwrite a real file on the FAT disk.
        char fname[16];
        uint32_t fn = 0;
        while (*fw == ' ') ++fw;
        while (*fw && *fw != ' ' && fn + 1 < sizeof(fname)) fname[fn++] = *fw++;
        fname[fn] = 0;
        while (*fw == ' ') ++fw;
        uint32_t tlen = 0;
        while (fw[tlen]) ++tlen;
        const int64_t w = lateran_write_file(fname, fw, tlen);
        if (w < 0) {
            writeln("fwrite: failed (disk/root full or not mounted)");
        } else {
            write("fwrite: wrote ");
            dec(static_cast<uint64_t>(w));
            write(" bytes to ");
            writeln(fname);
        }
    } else if (const char *fr = arg_after(line, "frm ")) {
        while (*fr == ' ') ++fr;
        writeln(lateran_unlink(fr) ? "frm: removed" : "frm: no such file");
    } else if (streq(line, "diskwrite")) {
        // Write a known pattern to a high scratch sector, read it back, verify.
        // Proves the virtio-blk write path (Underline) works end to end.
        const drivers::Underline &disk = drivers::underline_status();
        if (!disk.present || disk.capacity_sectors < 2) {
            writeln("diskwrite: no disk.");
        } else {
            const uint64_t scratch = disk.capacity_sectors - 1;
            for (uint32_t i = 0; i < 512; ++i) g_sector_buf[i] = static_cast<uint8_t>(i ^ 0x5a);
            bool ok = drivers::underline_write(disk, scratch, g_sector_buf);
            for (uint32_t i = 0; i < 512; ++i) g_sector_buf[i] = 0;
            ok = ok && drivers::underline_read(disk, scratch, g_sector_buf);
            bool match = ok;
            for (uint32_t i = 0; i < 512 && match; ++i) {
                if (g_sector_buf[i] != static_cast<uint8_t>(i ^ 0x5a)) match = false;
            }
            write("diskwrite: sector ");
            dec(scratch);
            writeln(match ? " write+readback OK (persisted)" : " FAILED");
        }
    } else if (streq(line, "ps")) {
        esper_report();
    } else if (streq(line, "user")) {
        run_user();
    } else if (streq(line, "userfault")) {
        run_user_fault();
    } else if (streq(line, "komoe")) {
        run_elf("KOMOE.ELF"); // EL0 shell: fork/exec/wait/read/write/open/close
    } else if (streq(line, "heap")) {
        void *a = dark_matter_alloc(1000);
        void *b = dark_matter_alloc(2000);
        void *c = dark_matter_alloc(500);
        write("alloc a="); hex(reinterpret_cast<uint64_t>(a));
        write(" b="); hex(reinterpret_cast<uint64_t>(b));
        write(" c="); hex(reinterpret_cast<uint64_t>(c));
        DarkMatterStats s1 = dark_matter_stats();
        write("\n  used="); dec(s1.used);
        write(" blocks="); dec(s1.blocks);
        write(" free_blocks="); dec(s1.free_blocks);
        dark_matter_free(b);
        DarkMatterStats s2 = dark_matter_stats();
        write("\nfreed b -> used="); dec(s2.used);
        write(" free_blocks="); dec(s2.free_blocks);
        void *d = dark_matter_alloc(1500);
        write("\nalloc d="); hex(reinterpret_cast<uint64_t>(d));
        writeln(d == b ? " (reused b's slot)" : " (new slot)");
        dark_matter_free(a);
        dark_matter_free(c);
        dark_matter_free(d);
        DarkMatterStats s3 = dark_matter_stats();
        write("freed all -> used="); dec(s3.used);
        write(" free_blocks="); dec(s3.free_blocks);
        writeln(s3.free_blocks == 1 ? " (fully coalesced)" : "");
    } else if (streq(line, "ls")) {
        for (uint32_t i = 0; i < grimoire_fs_count(); ++i) {
            write("  ");
            write(grimoire_fs_at(i)->name);
            writeln(" [ro]");
        }
        for (uint32_t i = 0; i < bookshelf_capacity(); ++i) {
            const BookshelfFile *f = bookshelf_at(i);
            if (f->used) {
                write("  ");
                write(f->name);
                write(" [rw] ");
                dec(f->size);
                writeln(" bytes");
            }
        }
        LateranEntry fat[32];
        const uint32_t fatn = lateran_list(fat, 32);
        for (uint32_t i = 0; i < fatn; ++i) {
            write("  ");
            write(fat[i].name);
            write(fat[i].is_dir ? "/ [disk dir]" : " [disk] ");
            if (!fat[i].is_dir) dec(fat[i].size);
            writeln(fat[i].is_dir ? "" : " bytes");
        }
    } else if (streq(line, "cat")) {
        writeln("usage: cat <name>");
    } else if (streq(line, "halt")) {
        writeln("Entering idle loop.");
        arch::halt();
    } else if (streq(line, "dmesg")) {
        // Snapshot the whole ring once into a stack buffer before printing so
        // the act of printing (which itself feeds the ring) can't grow what
        // we're trying to read. 16 KiB on the kernel stack is fine.
        static char chunk[16384];
        const uint32_t n = imaginary_number_district::kmsg_read(chunk, sizeof(chunk));
        for (uint32_t i = 0; i < n; ++i) putc(chunk[i]);
    } else if (streq(line, "poweroff") || streq(line, "off")) {
        if (heaven.othinus.available) {
            writeln("Othinus ends the world. Goodbye.");
            drivers::othinus_system_off(heaven.othinus);
        }
        writeln("Othinus is absent; cannot power off. Entering idle loop.");
        arch::halt();
    } else if (streq(line, "reboot") || streq(line, "reset")) {
        if (heaven.othinus.available) {
            writeln("Othinus remakes the world. Resetting.");
            drivers::othinus_system_reset(heaven.othinus);
        }
        writeln("Othinus is absent; cannot reset. Entering idle loop.");
        arch::halt();
    } else if (streq(line, "wxtest")) {
        writeln("Writing to .text; W^X should trip a permission fault now...");
        auto *code = reinterpret_cast<volatile uint32_t *>(__text_start);
        *code = 0;
        writeln("No fault occurred -- W^X is NOT enforced!");
    } else if (streq(line, "imagine")) {
        imagine_breaker("manual Necessarius invocation");
    } else {
        write("Unknown spell: ");
        writeln(line);
        writeln("Type 'help'.");
    }
}

} // namespace

[[noreturn]] void enter_necessarius(const ArtificialHeaven &heaven,
                                    IndexLibrorumProhibitorum &grimoires,
                                    TreeDiagram &tree) {
    using namespace imaginary_number_district;

    // Now in the high half with a stable VBAR + a live scheduler: safe to arm the
    // virtio-MMIO completion IRQ (arming it before the teleport hung -- the SPI was
    // taken against a transient vector mid-VBAR-switch).
    drivers::underline_enable_irq();

    // PID 1: try /sbin/init from the rootfs.  If it's there, hand the box
    // over to userspace -- run_elf only returns when every Esper has exited
    // (i.e. init crashed or chose to exit).  Falling through enters the
    // in-kernel shell as a safety net so a broken init never bricks the box.
    if (lateran_mounted()) {
        char probe[16];
        if (lateran_read_file("/sbin/init", probe, sizeof(probe)) > 0) {
            writeln("Index is alive; spawning /sbin/init as PID 1.");
            run_elf("/sbin/init");
            writeln("init returned; falling back to Necessarius.");
        }
    }

    writeln("Necessarius shell ready. Type 'help'.");

    char line[kLineSize];
    uint32_t len = 0;
    prompt();

    while (true) {
        // Poll for input, but sleep between tries so lower-Level Sisters get
        // the core. The shell is the highest Level, so it always wakes promptly
        // to service a keystroke yet never busy-starves the rest of the network.
        int raw = try_read();
        while (raw < 0) {
            misaka_network_sleep(1);
            raw = try_read();
        }
        const char ch = static_cast<char>(raw);

        if (ch == '\r' || ch == '\n') {
            write("\n");
            line[len] = 0;
            execute(line, heaven, grimoires, tree);
            len = 0;
            prompt();
            continue;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (len > 0) {
                --len;
                write("\b \b");
            }
            continue;
        }

        if (ch < 0x20 || ch > 0x7e) {
            continue;
        }

        if (len + 1 < kLineSize) {
            line[len++] = ch;
            putc(ch);
        }
    }
}

} // namespace index
