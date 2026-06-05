BUILD_DIR := build
TARGET := Index

LLVM_PREFIX ?=
LLD_PREFIX ?= $(LLVM_PREFIX)
CLANG ?= $(LLVM_PREFIX)clang
CXX ?= $(LLVM_PREFIX)clang++
OBJCOPY ?= $(LLVM_PREFIX)llvm-objcopy
STRIP ?= $(LLVM_PREFIX)llvm-strip
QEMU ?= qemu-system-aarch64
# zig (if installed) bundles musl + clang and can cross-compile real Linux
# AArch64 binaries to test the Linux-ABI compatibility layer. Optional: the
# kernel and all Index/hand-written tests build without it.
ZIG ?= zig

# UTM's QEMU backend runs the ARM64 `virt` machine. These defaults mirror that
# environment so `build/Image` boots identically under the UTM app (QEMU
# backend) and under a plain QEMU invocation. Do NOT use UTM's Apple
# Virtualization backend: it has no PL011 serial. gic-version=2 pins the GICv2
# controller Aleister drives (current QEMU defaults to v2 here anyway).
QEMU_MACHINE ?= virt,gic-version=2
QEMU_MEMORY ?= 1024M
QEMU_SERIAL ?= mon:stdio
QEMU_EXTRA ?=
# Number of cores. virt exposes them in the DTB and PSCI brings them online;
# MisakaNetwork schedules Sisters across all of them. SMP=1 is the single-core
# path (no secondaries to wake), useful for the classic single-core demos.
SMP ?= 4

# run-utm path: QEMU backend with HVF acceleration (what UTM's QEMU backend
# does on Apple Silicon when "Use Apple Hypervisor" is on). run-qemu path: the
# same QEMU backend with portable TCG emulation.
UTM_ACCEL ?= hvf
UTM_CPU ?= host
TCG_CPU ?= cortex-a72

ifeq ($(origin CXX), default)
CXX := $(LLVM_PREFIX)clang++
endif

ifeq ($(origin LD), default)
ifneq ($(wildcard $(LLD_PREFIX)ld.lld),)
LD := $(LLD_PREFIX)ld.lld
else ifneq ($(wildcard /opt/homebrew/opt/lld/bin/ld.lld),)
LD := /opt/homebrew/opt/lld/bin/ld.lld
else
LD := $(LLD_PREFIX)ld.lld
endif
endif

ARCH_FLAGS := --target=aarch64-none-elf -march=armv8-a -mgeneral-regs-only -mstrict-align
COMMON_FLAGS := $(ARCH_FLAGS) -ffreestanding -fno-stack-protector -fno-pic -fno-jump-tables -Wall -Wextra
DEPFLAGS := -MMD -MP
CXXFLAGS := $(COMMON_FLAGS) $(DEPFLAGS) -std=c++20 -O2 -g -fno-exceptions -fno-rtti -fno-use-cxa-atexit -nostdinc++ -Isrc
ASFLAGS := $(COMMON_FLAGS) $(DEPFLAGS) -x assembler-with-cpp
LDFLAGS := -T linker.ld -nostdlib -static --gc-sections -z max-page-size=0x1000

BOOT_SRC := src/arch/aarch64/boot.S
CPP_SRCS := $(shell find src -name '*.cpp' | sort)
ASM_SRCS := $(filter-out $(BOOT_SRC),$(shell find src -name '*.S' | sort))

BOOT_OBJ := $(BUILD_DIR)/$(BOOT_SRC:.S=.o)
CPP_OBJS := $(CPP_SRCS:%.cpp=$(BUILD_DIR)/%.o)
ASM_OBJS := $(ASM_SRCS:%.S=$(BUILD_DIR)/%.o)
OBJS := $(BOOT_OBJ) $(ASM_OBJS) $(CPP_OBJS)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean check-tools check-qemu run-utm run-qemu run-qemu-disk run-qemu-fat run-qemu-fat-pci run-qemu-net utm-args qemu-args dist utm-instructions

all: $(BUILD_DIR)/Image

check-tools:
	@command -v $(CLANG) >/dev/null || { echo "missing clang: set LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/ or install LLVM"; exit 1; }
	@command -v $(CXX) >/dev/null || { echo "missing clang++: set LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/ or install LLVM"; exit 1; }
	@command -v $(LD) >/dev/null || { echo "missing ld.lld: brew install llvm lld"; exit 1; }
	@command -v $(OBJCOPY) >/dev/null || { echo "missing llvm-objcopy: brew install llvm"; exit 1; }

check-qemu:
	@command -v $(QEMU) >/dev/null || { echo "missing qemu-system-aarch64: brew install qemu"; exit 1; }

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CLANG) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: linker.ld $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/Image: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# QEMU backend with HVF acceleration, matching UTM's QEMU backend on Apple Silicon.
run-utm: check-qemu $(BUILD_DIR)/Image
	$(QEMU) -M $(QEMU_MACHINE) -accel $(UTM_ACCEL) -cpu $(UTM_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) -kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# QEMU backend with portable TCG emulation (works off Apple Silicon too).
run-qemu: check-qemu $(BUILD_DIR)/Image
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) -kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# Like run-qemu but attaches a virtio-blk disk so the Underline driver and the
# `disk` command have something to read. DISK defaults to build/disk.img.
DISK ?= $(BUILD_DIR)/disk.img
$(DISK):
	@mkdir -p $(@D)
	printf 'INDEX-UNDERLINE-SECTOR0! virtio-blk works.' > $@
	dd if=/dev/zero bs=1024 count=1024 >> $@ 2>/dev/null
	@echo "created $@ (1 MiB test disk)"

run-qemu-disk: check-qemu $(BUILD_DIR)/Image $(DISK)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) \
		-drive file=$(DISK),if=none,id=hd0,format=raw -device virtio-blk-device,drive=hd0 \
		-kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# Standalone EL0 user programs, built as position-independent ELFs and placed on
# the FAT disk so the kernel can load and run them (`exec INIT.ELF`, `coexec`).
# Every user ELF links AcademyCity (the shared mini-libc) so user code can use
# ac_printf etc. without each binary re-implementing them.
$(BUILD_DIR)/academy_city.uo: userprog/academy_city.cpp userprog/academy_city.h userprog/usys.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fpie -c $< -o $@

$(BUILD_DIR)/%.elf: userprog/%.cpp $(BUILD_DIR)/academy_city.uo | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fpie -c $< -o $(BUILD_DIR)/$*.uo
	$(LD) -pie -e _start --gc-sections -z max-page-size=0x1000 -o $@ $(BUILD_DIR)/$*.uo $(BUILD_DIR)/academy_city.uo

# Hand-written Linux-ABI test ELF: assembled from .S, linked as PIE without
# academy_city (so no .note.Index marker), then the ELF header's EI_OSABI byte
# (offset 7) is patched to 3 = ELFOSABI_LINUX. The kernel's sniff_abi() sees
# OSABI=3 and routes the loaded Esper to linux_abi.cpp's syscall table. The
# `printf | dd` is a portable way to overwrite one byte without rewriting
# the file.
$(BUILD_DIR)/hellolinux.elf: userprog/hellolinux.S | $(BUILD_DIR)
	$(CLANG) $(ASFLAGS) -c $< -o $(BUILD_DIR)/hellolinux.o
	$(LD) -pie -e _start --gc-sections -z max-page-size=0x1000 -o $@ $(BUILD_DIR)/hellolinux.o
	printf '\x03' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none

# The same Linux-ABI program but linked as a NON-PIE ET_EXEC at a fixed high
# VA (0x400000). This exercises the demand-pager's free-address-space path:
# sniff_abi catches it via e_entry >= 0x400000, the loader honours the
# absolute vaddrs (load_base = 0), and the page-fault handler installs L2/L3
# tables for a region the kernel never pre-mapped.
$(BUILD_DIR)/hellohi.elf: userprog/hellolinux.S | $(BUILD_DIR)
	$(CLANG) $(ASFLAGS) -c $< -o $(BUILD_DIR)/hellohi.o
	$(LD) -e _start --gc-sections -z max-page-size=0x1000 -Ttext=0x400000 -o $@ $(BUILD_DIR)/hellohi.o
	printf '\x03' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none

# auxvdump: walks the SysV startup stack the kernel builds (argc/argv/envp/auxv)
# and prints what it found. Validates Phase C. PIE, Linux-ABI (OSABI patched).
$(BUILD_DIR)/auxvdump.elf: userprog/auxvdump.S | $(BUILD_DIR)
	$(CLANG) $(ASFLAGS) -c $< -o $(BUILD_DIR)/auxvdump.o
	$(LD) -pie -e _start --gc-sections -z max-page-size=0x1000 -o $@ $(BUILD_DIR)/auxvdump.o
	printf '\x03' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none

# A real Linux binary built against musl with zig's bundled toolchain, static
# and stripped (so it fits the kernel's heap/loader budget). This is the
# end-to-end proof that an unmodified Linux/aarch64 executable runs. Requires
# `zig` on PATH; `make check-zig` reports if it's missing.
$(BUILD_DIR)/hellomusl.elf: userprog/hellomusl.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# Same program, dynamically linked. Its PT_INTERP names /lib/ld-musl-aarch64.so.1,
# which the kernel maps to LD-MUSL.SO on the disk. The interpreter (the musl
# dynamic linker, a real aarch64 .so vendored under userprog/) self-relocates
# and runs the program. This is the end-to-end dynamic-linking proof.
$(BUILD_DIR)/hellodyn.elf: userprog/hellomusl.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# Dynamic C++ smoke test -- exercises libstdc++.so.6 + libgcc_s.so.1 link
# (iostreams, STL, RTTI, cross-DSO exception unwind).
$(BUILD_DIR)/hellocxx.elf: userprog/hellocxx.cpp | $(BUILD_DIR)
	$(ZIG) c++ -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# dlopen / dlsym / dlclose runtime probe across all three installed .so's.
$(BUILD_DIR)/dlopentest.elf: userprog/dlopentest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# Full C++17 / C++20 audit: static init, iostream, fstream, STL, RTTI,
# exceptions (including nested), atomic, std::thread, mutex+cv, futures,
# chrono, thread_local, regex, filesystem, variant/optional, lambda+function.
# Each test prints "PASS <tag>" or "FAIL <tag>" so the harness can grep.
$(BUILD_DIR)/cxx_audit.elf: userprog/cxx_audit.cpp | $(BUILD_DIR)
	$(ZIG) c++ -std=c++2c -target aarch64-linux-musl -dynamic -Os \
	    -fcoroutines -fexperimental-library -o $@ $<
	$(STRIP) $@

# Same audit but linked against the rootfs's libstdc++.so.6 (GNU/GCC) instead
# of zig's embedded libc++. Isolates whether the mutex+thread hang is a
# libc++ ↔ musl pthread teardown bug.
$(BUILD_DIR)/cxx_audit_gnu.elf: userprog/cxx_audit.cpp | $(BUILD_DIR)
	$(ZIG) c++ -std=c++23 -target aarch64-linux-musl -dynamic -Os \
	    -stdlib=libstdc++ -o $@ $<
	$(STRIP) $@

# Minimal C pthread mutex test (static-linked musl), bypasses libc++.
$(BUILD_DIR)/mutextest.elf: userprog/mutextest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -O0 -g -o $@ $< -lpthread
	$(STRIP) $@

# Same test, dynamic-linked against the rootfs's /lib/ld-musl-aarch64.so.1
# (Alpine 3.18 musl 1.2.4). -Os to drop unused stat wrappers (zig 0.16's
# musl headers reference `statx` which Alpine 1.2.4 doesn't export).
$(BUILD_DIR)/mutextest_dyn.elf: userprog/mutextest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $< -lpthread
	$(STRIP) $@

# Mixed: std::thread (libc++) + pthread_mutex (musl raw). Isolates which
# wrapper is the bug.
$(BUILD_DIR)/mutextest_cxx.elf: userprog/mutextest_cxx.cpp | $(BUILD_DIR)
	$(ZIG) c++ -target aarch64-linux-musl -dynamic -Os -o $@ $< -lpthread
	$(STRIP) $@

# Single-threaded std::mutex lock/unlock: should never block.
$(BUILD_DIR)/stdmutex.elf: userprog/stdmutex.cpp | $(BUILD_DIR)
	$(ZIG) c++ -target aarch64-linux-musl -dynamic -Os -o $@ $< -lpthread
	$(STRIP) $@

# A musl program that opens + reads a file (exercises Linux openat/read/fstat).
$(BUILD_DIR)/catfile.elf: userprog/catfile.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that installs a signal handler and raises a signal
# (exercises rt_sigaction + signal-frame delivery + rt_sigreturn).
$(BUILD_DIR)/sigtest.elf: userprog/sigtest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# timerfd + signalfd + epoll functional test (static musl). Prints TIMERFD_OK /
# SIGFD_OK so a boot harness can grep for them.
$(BUILD_DIR)/epolltest.elf: userprog/epolltest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that watches /tmp via inotify and verifies CREATE/MODIFY/DELETE.
$(BUILD_DIR)/inotifytest.elf: userprog/inotifytest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that traces a child via ptrace (TRACEME/GETREGSET/PEEK/CONT).
$(BUILD_DIR)/ptracetest.elf: userprog/ptracetest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that exercises PTRACE_SYSCALL syscall-stops.
$(BUILD_DIR)/ptsyscalltest.elf: userprog/ptsyscalltest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that exercises tmpfs edge cases (rename cycle + huge truncate).
$(BUILD_DIR)/tmpfsedge.elf: userprog/tmpfsedge.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that exercises the mmap length-overflow guard.
$(BUILD_DIR)/mmapedge.elf: userprog/mmapedge.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that verifies ptrace PSTATE sanitization blocks EL1 escalation.
$(BUILD_DIR)/ptracesec.elf: userprog/ptracesec.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that verifies a dying tracer's stopped tracee is resumed.
$(BUILD_DIR)/ptraceorphan.elf: userprog/ptraceorphan.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# A musl program that fork()s and waitpid()s (exercises clone + wait4).
$(BUILD_DIR)/forktest.elf: userprog/forktest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that creates + joins a pthread (exercises clone(CLONE_VM) +
# futex + TLS). Static-linked to keep the thread path self-contained.
$(BUILD_DIR)/threadtest.elf: userprog/threadtest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that mmaps a file MAP_PRIVATE and reads through the mapping.
$(BUILD_DIR)/mmaptest.elf: userprog/mmaptest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that fork+CoW+exit's in a loop (exercises page reclamation).
$(BUILD_DIR)/forkloop.elf: userprog/forkloop.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that toggles raw/cooked mode via termios + handles Ctrl-C.
$(BUILD_DIR)/ttytest.elf: userprog/ttytest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that creates + writes a file (exercises the FAT write path).
$(BUILD_DIR)/writefile.elf: userprog/writefile.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that mkdir's a subdir, writes a file in it, and readdir's it.
$(BUILD_DIR)/dirtest.elf: userprog/dirtest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl program that prints its argc + argv. Sanity check for the new SysV
# initial-stack path that linuxrun + execve drive.
$(BUILD_DIR)/argvecho.elf: userprog/argvecho.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl UDP echo program. Validates the Antenna socket layer (Phase Net-1)
# end to end: socket/bind/recvfrom/sendto. Pair with run-qemu-ext2-net so
# host UDP 5556 reaches guest UDP 5556 through SLIRP's hostfwd.
$(BUILD_DIR)/udpecho.elf: userprog/udpecho.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl TCP client. Validates the Antenna TCP path (Phase Net-2): socket
# (SOCK_STREAM)+connect+write+read+close. Pair with a host listener (Python
# or nc) on 127.0.0.1 -- SLIRP's 10.0.2.2 gateway address routes to host
# loopback, so `tcpclient 10.0.2.2 PORT ...` opens a real TCP connection.
$(BUILD_DIR)/tcpclient.elf: userprog/tcpclient.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# A musl TCP server. Validates the Antenna passive-TCP path (Phase Net-3):
# socket+bind+listen+accept+read+write+close. Pair with run-qemu-ext2-net so
# host TCP 5557 reaches the guest listener through SLIRP's hostfwd.
$(BUILD_DIR)/tcpserver.elf: userprog/tcpserver.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -dynamic -Os -o $@ $<
	$(STRIP) $@

# SCM_RIGHTS fd passing over an AF_UNIX socketpair (OpenSSH privsep
# prerequisite): parent forks, sends a pipe write-fd to the child via
# sendmsg(SCM_RIGHTS), child writes through it, parent reads it back.
# Static so it exercises sendmsg/recvmsg/fork without the dynamic loader.
$(BUILD_DIR)/scmtest.elf: userprog/scmtest.c | $(BUILD_DIR)
	$(ZIG) cc -target aarch64-linux-musl -static -Os -o $@ $<
	$(STRIP) $@

# The vendored musl dynamic linker, copied to the disk as LD-MUSL.SO (already
# stripped when vendored under userprog/).
$(BUILD_DIR)/ld-musl.so: userprog/ld-musl-aarch64.so.1 | $(BUILD_DIR)
	cp $< $@

check-zig:
	@command -v $(ZIG) >/dev/null || { echo "missing zig (needed only for the musl Linux-ABI test): brew install zig"; exit 1; }

# Like run-qemu but attaches a FAT16 disk (built by tools/mkfatfs.py) so the
# Lateran filesystem has real files to read, including the user ELFs.
FATDISK ?= $(BUILD_DIR)/fat.img
$(FATDISK): tools/mkfatfs.py $(BUILD_DIR)/init.elf $(BUILD_DIR)/spin.elf \
            $(BUILD_DIR)/komoe.elf $(BUILD_DIR)/forkdemo.elf $(BUILD_DIR)/wc.elf \
            $(BUILD_DIR)/looper.elf $(BUILD_DIR)/hellolinux.elf $(BUILD_DIR)/hellohi.elf \
            $(BUILD_DIR)/auxvdump.elf $(BUILD_DIR)/hellomusl.elf \
            $(BUILD_DIR)/hellodyn.elf $(BUILD_DIR)/ld-musl.so \
            $(BUILD_DIR)/catfile.elf $(BUILD_DIR)/sigtest.elf \
            $(BUILD_DIR)/forktest.elf $(BUILD_DIR)/threadtest.elf \
            $(BUILD_DIR)/mmaptest.elf $(BUILD_DIR)/forkloop.elf \
            $(BUILD_DIR)/ttytest.elf $(BUILD_DIR)/writefile.elf \
            $(BUILD_DIR)/dirtest.elf | $(BUILD_DIR)
	python3 tools/mkfatfs.py $@ INIT.ELF=$(BUILD_DIR)/init.elf SPIN.ELF=$(BUILD_DIR)/spin.elf \
		KOMOE.ELF=$(BUILD_DIR)/komoe.elf FORKDEMO.ELF=$(BUILD_DIR)/forkdemo.elf \
		WC.ELF=$(BUILD_DIR)/wc.elf LOOPER.ELF=$(BUILD_DIR)/looper.elf \
		HELLOLX.ELF=$(BUILD_DIR)/hellolinux.elf HELLOHI.ELF=$(BUILD_DIR)/hellohi.elf \
		AUXV.ELF=$(BUILD_DIR)/auxvdump.elf HMUSL.ELF=$(BUILD_DIR)/hellomusl.elf \
		HDYN.ELF=$(BUILD_DIR)/hellodyn.elf LD-MUSL.SO=$(BUILD_DIR)/ld-musl.so \
		CATFILE.ELF=$(BUILD_DIR)/catfile.elf SIGTEST.ELF=$(BUILD_DIR)/sigtest.elf \
		FORKTEST.ELF=$(BUILD_DIR)/forktest.elf THREAD.ELF=$(BUILD_DIR)/threadtest.elf \
		MMAP.ELF=$(BUILD_DIR)/mmaptest.elf FORKLOOP.ELF=$(BUILD_DIR)/forkloop.elf \
		TTY.ELF=$(BUILD_DIR)/ttytest.elf WRITEF.ELF=$(BUILD_DIR)/writefile.elf \
		DIRTEST.ELF=$(BUILD_DIR)/dirtest.elf

# cache=directsync makes guest disk writes go straight to the host image file
# (no QEMU writeback cache), so files the kernel writes via the FAT layer
# persist across reboots.
run-qemu-fat: check-qemu $(BUILD_DIR)/Image $(FATDISK)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) \
		-drive file=$(FATDISK),if=none,id=hd0,format=raw,cache=directsync -device virtio-blk-device,drive=hd0 \
		-kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# A real ext2 root filesystem with a proper directory tree (/lib /bin /etc /tmp)
# built on the host with genext2fs. The kernel's Lateran layer detects ext2 and
# serves real Unix paths (/lib/ld-musl-aarch64.so.1, /bin/<prog>) with real
# inode metadata. Dynamic programs load their interpreter from its true path.
EXT2DISK ?= $(BUILD_DIR)/ext2.img
GENEXT2FS ?= genext2fs
ROOTFS := $(BUILD_DIR)/rootfs
$(EXT2DISK): $(BUILD_DIR)/ld-musl.so $(BUILD_DIR)/hellodyn.elf $(BUILD_DIR)/catfile.elf $(BUILD_DIR)/cxx_audit.elf $(BUILD_DIR)/mutextest.elf $(BUILD_DIR)/mutextest_dyn.elf $(BUILD_DIR)/mutextest_cxx.elf $(BUILD_DIR)/stdmutex.elf \
             $(BUILD_DIR)/dirtest.elf $(BUILD_DIR)/writefile.elf $(BUILD_DIR)/komoe.elf \
             $(BUILD_DIR)/argvecho.elf $(BUILD_DIR)/udpecho.elf $(BUILD_DIR)/tcpclient.elf \
             $(BUILD_DIR)/tcpserver.elf $(BUILD_DIR)/hellocxx.elf \
             $(BUILD_DIR)/dlopentest.elf \
             $(BUILD_DIR)/scmtest.elf \
             userprog/libstdc++.so.6 userprog/libgcc_s.so.1 | $(BUILD_DIR)
	@command -v $(GENEXT2FS) >/dev/null || { echo "missing genext2fs: brew install genext2fs"; exit 1; }
	rm -rf $(ROOTFS)
	mkdir -p $(ROOTFS)/lib $(ROOTFS)/bin $(ROOTFS)/etc $(ROOTFS)/tmp $(ROOTFS)/dev \
	         $(ROOTFS)/dev/pts $(ROOTFS)/dev/shm $(ROOTFS)/mnt $(ROOTFS)/proc
	# /dev placeholder files: genext2fs can't make device nodes, so empty files
	# act as stat() targets while linux_abi's openat early-intercept routes
	# read/write to the in-kernel FdKind::devnull/devtty/... handlers.
	touch $(ROOTFS)/dev/null $(ROOTFS)/dev/zero $(ROOTFS)/dev/random $(ROOTFS)/dev/urandom
	touch $(ROOTFS)/dev/tty $(ROOTFS)/dev/console $(ROOTFS)/dev/ptmx
	cp $(BUILD_DIR)/ld-musl.so   $(ROOTFS)/lib/ld-musl-aarch64.so.1
	# Dynamic C++ runtime: install the musl libc shared object alias
	# (libc.so == ld-musl.so on musl), libstdc++ for C++ stdlib, and
	# libgcc_s for unwind/throw across shared boundaries. Allows
	# dynamic-linked C++ programs (and dlopen) to actually resolve symbols
	# beyond a static link.
	mkdir -p $(ROOTFS)/usr/lib
	cp $(BUILD_DIR)/ld-musl.so   $(ROOTFS)/lib/libc.musl-aarch64.so.1
	cp userprog/libstdc++.so.6   $(ROOTFS)/usr/lib/libstdc++.so.6
	cp userprog/libgcc_s.so.1    $(ROOTFS)/usr/lib/libgcc_s.so.1
	# musl's per-arch ld-config: tells the dynamic linker which directories
	# to scan for shared libraries. /lib + /usr/lib match Alpine's layout.
	printf '/lib\n/usr/lib\n' > $(ROOTFS)/etc/ld-musl-aarch64.path
	cp $(BUILD_DIR)/hellodyn.elf $(ROOTFS)/bin/hello
	cp $(BUILD_DIR)/catfile.elf  $(ROOTFS)/bin/catfile
	cp $(BUILD_DIR)/dirtest.elf  $(ROOTFS)/bin/dirtest
	cp $(BUILD_DIR)/writefile.elf $(ROOTFS)/bin/writefile
	cp $(BUILD_DIR)/komoe.elf    $(ROOTFS)/bin/komoe
	cp $(BUILD_DIR)/argvecho.elf $(ROOTFS)/bin/argvecho
	cp $(BUILD_DIR)/udpecho.elf  $(ROOTFS)/bin/udpecho
	cp $(BUILD_DIR)/tcpclient.elf $(ROOTFS)/bin/tcpclient
	cp $(BUILD_DIR)/tcpserver.elf $(ROOTFS)/bin/tcpserver
	cp $(BUILD_DIR)/hellocxx.elf $(ROOTFS)/bin/hellocxx
	cp $(BUILD_DIR)/cxx_audit.elf $(ROOTFS)/bin/cxx_audit
	cp $(BUILD_DIR)/mutextest.elf $(ROOTFS)/bin/mutextest
	cp $(BUILD_DIR)/mutextest_dyn.elf $(ROOTFS)/bin/mutextest_dyn
	cp $(BUILD_DIR)/mutextest_cxx.elf $(ROOTFS)/bin/mt_cxx
	cp $(BUILD_DIR)/stdmutex.elf $(ROOTFS)/bin/stdmutex
	cp $(BUILD_DIR)/dlopentest.elf $(ROOTFS)/bin/dltest
	cp $(BUILD_DIR)/scmtest.elf $(ROOTFS)/bin/scmtest
	# Optional static busybox (aarch64): drop a binary at userprog/busybox to
	# enable. busybox uses argv[0] basename to pick its applet, so we install
	# a symlink under each name (Lateran ext2 follows symlinks). Image is one
	# busybox binary (~1.1 MiB) + N tiny symlinks regardless of applet count.
	if [ -f userprog/busybox ]; then \
	  cp userprog/busybox $(ROOTFS)/bin/busybox; \
	  chmod +x $(ROOTFS)/bin/busybox; \
	  for a in \
	      sh ls cat echo pwd mkdir rmdir rm cp mv true false sleep env uname head tail wc \
	      init login getty passwd su id whoami \
	      find grep sed awk tr cut sort uniq tee less more cmp diff \
	      stat touch chmod chown chgrp ln basename dirname du df which xargs file \
	      ps top free dmesg date uptime nproc kill killall pidof \
	      clear tty stty yes seq hostname dd sync test type \
	      tar gzip gunzip mount umount \
	      printf expr od hexdump xxd md5sum sha256sum strings; do \
	    ln -sf busybox $(ROOTFS)/bin/$$a; \
	  done; \
	  mkdir -p $(ROOTFS)/sbin; \
	  ln -sf /bin/busybox $(ROOTFS)/sbin/init; \
	  ln -sf /bin/busybox $(ROOTFS)/sbin/getty; \
	fi
	printf 'Welcome to Index (ext2 rootfs).\n' > $(ROOTFS)/etc/motd
	# Phase H identity. uid 0 = crowley (Aleister Crowley as Academy City's
	# hidden supreme authority; the canonical name). `root` is kept as a
	# second entry at the same uid 0 so upstream Linux software that resolves
	# the literal string "root" via getpwnam keeps working (sudo, chown
	# root:root, cron, systemd unit User=root, sshd, ...). getpwuid(0) still
	# returns the FIRST match -- so whoami / id / `\u` in the shell show
	# `crowley`, not `root`. The alias is misdirection in-universe.
	# /etc/passwd with empty password field.  busybox login skips authentication
	# when field 2 is empty -- the demo mode for a tutorial OS where you just
	# type the username and get a shell.  Set a real password later with
	# `passwd` (also a busybox applet).
	printf 'crowley::0:0:Aleister Crowley:/:/bin/sh\nroot::0:0:root (alias of crowley):/:/bin/sh\nstudent::1000:1000:Academy student:/tmp:/bin/sh\n' > $(ROOTFS)/etc/passwd
	printf 'wheel:x:0:crowley\nroot:x:0:crowley\nstudent:x:1000:\n' > $(ROOTFS)/etc/group
	# Shadow with empty password (matches /etc/passwd above).
	printf 'crowley::19905:0:99999:7:::\nroot::19905:0:99999:7:::\nstudent::19905:0:99999:7:::\n' > $(ROOTFS)/etc/shadow
	# busybox init reads /etc/inittab.  `::sysinit:cmd` runs once; `::respawn:cmd` restarts on exit.
	printf '::sysinit:/bin/echo Index booting\n::respawn:/sbin/getty -L 9600 /dev/console linux\n' > $(ROOTFS)/etc/inittab
	printf 'index\n' > $(ROOTFS)/etc/hostname
	printf '127.0.0.1\tlocalhost\n10.0.2.15\tindex\n' > $(ROOTFS)/etc/hosts
	$(GENEXT2FS) -d $(ROOTFS) -b 32768 -B 1024 $@
	@echo "built ext2 rootfs $@"

run-qemu-ext2: check-qemu $(BUILD_DIR)/Image $(EXT2DISK)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) \
		-drive file=$(EXT2DISK),if=none,id=hd0,format=raw,cache=directsync -device virtio-blk-device,drive=hd0 \
		-kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# Like run-qemu-fat but attaches the FAT disk as virtio-blk over PCIe -- exactly
# what the UTM app's GUI "VirtIO" disk produces. Exercises the Underground (PCIe)
# discovery + virtio-modern-over-PCI path instead of virtio-mmio.
run-qemu-fat-pci: check-qemu $(BUILD_DIR)/Image $(FATDISK)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) \
		-drive file=$(FATDISK),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# Disk (virtio-blk-pci) + a virtio-net-pci NIC on user-mode (SLIRP) networking,
# for the MisakaMail stack. filter-dump captures all NIC traffic to a pcap so the
# ARP/ICMP frames can be inspected. SLIRP answers ARP + ICMP echo for the gateway
# 10.0.2.2, so `net ping` works here. NET defaults to user networking.
run-qemu-net: check-qemu $(BUILD_DIR)/Image $(FATDISK)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) \
		-drive file=$(FATDISK),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
		-object filter-dump,id=d0,netdev=n0,file=$(BUILD_DIR)/net.pcap \
		-kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# Like run-qemu-ext2 but with a virtio-net NIC. SLIRP forwards host UDP 5556
# to guest 5556 (Phase Net-1 udpecho) and host TCP 5557 to guest 5557 (Phase
# Net-3 tcpserver), so `nc -u 127.0.0.1 5556` / `nc 127.0.0.1 5557` reach
# the guest listeners. Same pcap dump for inspection.
run-qemu-ext2-net: check-qemu $(BUILD_DIR)/Image $(EXT2DISK)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) \
		-drive file=$(EXT2DISK),if=none,id=hd0,format=raw,cache=directsync -device virtio-blk-device,drive=hd0 \
		-netdev user,id=n0,hostfwd=udp::5556-:5556,hostfwd=tcp::5557-:5557 -device virtio-net-pci,netdev=n0 \
		-object filter-dump,id=d0,netdev=n0,file=$(BUILD_DIR)/net.pcap \
		-kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

# Host share directory mounted into the guest at /host via virtio-9p
# (StiylMagnus driver, 9P2000.L). Override on the command line:
#   make run-qemu-ext2-share SHARE_DIR=/path/on/host
SHARE_DIR ?= $(BUILD_DIR)/host-share
$(SHARE_DIR):
	mkdir -p $(SHARE_DIR)
	@if [ -z "$$(ls -A $(SHARE_DIR) 2>/dev/null)" ]; then \
	  printf 'hello from host\n' > $(SHARE_DIR)/HELLO.txt; \
	  printf '#!/bin/sh\necho host script ran\n' > $(SHARE_DIR)/runme.sh; \
	  chmod +x $(SHARE_DIR)/runme.sh; \
	  mkdir -p $(SHARE_DIR)/subdir && printf 'nested\n' > $(SHARE_DIR)/subdir/inner.txt; \
	fi

run-qemu-ext2-share: check-qemu $(BUILD_DIR)/Image $(EXT2DISK) $(SHARE_DIR)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) \
		-drive file=$(EXT2DISK),if=none,id=hd0,format=raw,cache=directsync -device virtio-blk-device,drive=hd0 \
		-fsdev local,id=fs0,path=$(SHARE_DIR),security_model=mapped-xattr \
		-device virtio-9p-device,mount_tag=hostshare,fsdev=fs0 \
		-kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)

utm-args: $(BUILD_DIR)/Image
	@echo "$(QEMU) -M $(QEMU_MACHINE) -accel $(UTM_ACCEL) -cpu $(UTM_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) -kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)"

qemu-args: $(BUILD_DIR)/Image
	@echo "$(QEMU) -M $(QEMU_MACHINE) -cpu $(TCG_CPU) -m $(QEMU_MEMORY) -smp $(SMP) -nographic -serial $(QEMU_SERIAL) -kernel $(BUILD_DIR)/Image $(QEMU_EXTRA)"

# Bundle a ready-to-import kernel image plus quick UTM setup notes.
dist: $(BUILD_DIR)/Image utm-instructions
	@mkdir -p $(BUILD_DIR)/utm
	cp $(BUILD_DIR)/Image $(BUILD_DIR)/utm/Index-Image
	@echo "wrote $(BUILD_DIR)/utm/Index-Image and $(BUILD_DIR)/utm/BOOT-UTM.txt"

utm-instructions: | $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/utm
	@printf '%s\n' \
	  "Index (arm-Index) on UTM" \
	  "=========================" \
	  "" \
	  "1. UTM -> Create a New Virtual Machine." \
	  "2. Pick Emulate (this uses the QEMU backend). Do NOT use Apple" \
	  "   Virtualization: it has no PL011 serial, so there is no output." \
	  "3. Operating System: Linux." \
	  "4. Enable 'Boot from kernel image directly'." \
	  "     Linux kernel    : Index-Image (this folder)" \
	  "     Initial ramdisk : (leave empty)" \
	  "     DTB             : (leave empty; the virt machine generates one)" \
	  "5. Architecture: ARM64 (aarch64). System: QEMU 'virt'." \
	  "6. CPU: Default/Host. Memory: 1024 MB or more." \
	  "7. Optional (Apple Silicon): enable 'Use Apple Hypervisor' for HVF" \
	  "   acceleration -- still the QEMU backend, just faster." \
	  "8. Save, then open Serial in UTM to reach the Necessarius shell." \
	  "" \
	  "Type 'help' at the Index> prompt. 'uptime' shows the LastOrder timer beats;" \
	  "'spawn'/'sleeper'/'prodcons' add MisakaNetwork threads, 'sisters' lists them;" \
	  "'ls'/'cat <name>' read GrimoireFS; 'poweroff' / 'reboot' exit via PSCI." \
	  > $(BUILD_DIR)/utm/BOOT-UTM.txt

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
