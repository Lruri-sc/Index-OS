# Index

`Index`(代号 *Index Librorum Prohibitorum*）是一个从零写起的 **ARM64 freestanding C++ 操作系统内核**，
在 **UTM / QEMU `virt` 机器**上启动，并能直接运行**未经修改的 Linux aarch64 二进制**——包括完整的
**OpenJDK 26（`java` + `javac`）**、**OpenSSH（真机 HVF 上可登录）**、busybox、以及静态/动态链接的 musl 与
glibc 程序。内核是 C++20 freestanding。

> **运行平台必须用 UTM 的 QEMU 后端**（Apple Silicon 上可开 **HVF** 加速；其它 Mac 用 TCG 仿真）。
> **不要用 Apple Virtualization 后端**——它的串口是 virtio-console 而非 `virt` 机器的 **PL011**，本内核只驱动
> PL011，在 Apple Virtualization 下不会有任何串口输出。HVF 只是 QEMU 后端的一个加速开关，和 Apple
> Virtualization 不是一回事。

---

## 构建 & 运行

工具链是 LLVM（clang++ / ld.lld / llvm-objcopy）；不在 PATH 时传前缀：

```sh
# 内核 → build/Image
make LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/

# ext2 根文件系统镜像（主机 genext2fs 从目录树构建：/lib /bin /etc /tmp …）
make build/ext2.img LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/

# 本地跑（QEMU；带磁盘 + 网络）
make run-qemu-ext2     LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
make run-qemu-ext2-net LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/

# 主机目录共享（virtio-9p，挂到 guest /host）
make run-qemu-ext2-share SHARE_DIR=/path/on/host LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

非交互测试惯例：往串口管道喂输入，例如
`{ sleep 16; printf 'crowley\n'; sleep 5; printf 'cmd\n'; ...; } | qemu ... -serial mon:stdio`。
（登录用户 **uid 0 = `crowley`**，空密码。）

**部署到 UTM**：`build/Image` 作 kernel，ext2 镜像作 VirtIO 盘（UTM GUI 的「VirtIO」即 virtio-blk-pci）。
真机网络端口转发须用 UTM 的 **Emulated VLAN**（Shared 网络不支持转发）。

---

## 能力概览

### 启动 / 平台
- AArch64 入口清 BSS、建早期栈、装异常向量，进入 C++ `kmain`；从 **DTB** 发现 PL011、GIC、`/psci`、
  `/memory`、framebuffer、virtio 设备。
- **EL2→EL1 启动降级**：UTM/HVF 与 `virtualization=on` 把内核引导在 EL2，`boot.S` 的 `el2_to_el1`
  设 `HCR_EL2.RW`、放开 EL1 定时器、`eret` 降到 EL1h（主核 + PSCI 唤醒的副核都先降级）。
- **`Othinus`（PSCI）**：从 DTB 读 conduit（`hvc`/`smc`）做 `CPU_ON` / `SYSTEM_OFF` / `SYSTEM_RESET`。
- **`Aleister`（GIC）**：支持 **GICv2 与 GICv3**（HVF 要求 v3：系统寄存器 CPU 接口 + 每核 redistributor +
  SPI 的 IROUTER/IGROUPR/priority/ISENABLER 完整路由）；按 intid 注册/分发 handler，ack/EOI。
- **`LastOrder`**：ARM **虚拟**定时器（CNTV，PPI 27）100 Hz 系统心跳（HVF 占着 EL2 时物理定时器会被陷入，
  虚拟定时器两种情况都能从 EL1 访问）。

### 内存 / MMU
- **`Teleport`**：三级页表打开 MMU，内核搬到 **higher-half**（TTBR1 `0xFFFFFF80…`）+ 恒等别名（TTBR0，
  给 device MMIO / DTB / 物理栈）；39 位 VA 下低 39 位 = 物理地址，**一张页表同时服务 TTBR0/TTBR1**。
  内核段 **W^X** 细粒度保护（.text 只读可执行、数据 NX）。
- **`PersonalReality v2`**：每进程私有地址空间 + ≤N 项 **VMA 表** + **按需分页**（EL0 缺页→命中 VMA→取页/
  零填/从 ELF 文件副本拷段→装 L3→TLBI）。
- **物理内存回收 + 真 CoW**：`TreeDiagram` free-list 回收页、按物理页引用计数、`pr2_fork` 共享+写时复制、
  进程退出遍历页表 `page_unref`、地址空间引用计数让 `clone(CLONE_VM)` 线程共享、末线程退出才拆——
  **零泄漏，系统能无限期跑**。
- **`DarkMatter`**：可回收内核堆（kmalloc/kfree，首次适配 + 前向合并）。

### 调度 / SMP
- **`MisakaNetwork`**：抢占式优先级调度器；`Sister` 是内核线程、`Esper` 是 EL0 用户进程。
- **SMP 多核（8 核）**：`Othinus` PSCI 逐个唤醒副核，每核自建 MMU/GIC CPU 接口/定时器并参与调度；
  EL0 在所有核上并行运行。`g_esper_lock` 单锁覆盖 Esper 状态边、reschedule 经 SGI IPI 跨核。
- **同步原语**：`Imprimatur`（信号量）、`Judgement`（互斥锁，带 `Level Upper` 优先级继承）、
  `RadioNoise`（有界阻塞消息队列）、`AntiSkill`（自旋锁）。

### Linux ABI（运行未修改的 Linux aarch64 程序）
- **双 ABI 派发**：`sniff_abi` 按 ELF 头区分 Index 原生 / Linux；Linux 程序走 `linux_abi.cpp` 的
  Linux AArch64 syscall 表 + SysV 启动栈（argc/argv/envp/auxv）。**静态 + 动态链接**都直接跑
  （`PT_INTERP` 解释器由内核加载、重定位全在 EL0 由 ld.so 完成）。
- **覆盖面**：文件 I/O、`mmap`（含文件 demand-paging）、`brk`、完整**信号**（rt_sigaction/rt_sigreturn/
  sigframe，SROP 防护）、`fork`/`wait4`、**线程**（clone + futex + TLS）、**ptrace「Mental Out」**
  （PEEK/POKE/单步/PTRACE_SYSCALL/GETREGSET）、**epoll / timerfd / signalfd**、**inotify「Kazakiri」**、
  **真 tmpfs「Testament」**（/tmp、/dev/shm）、pipe/SCM_RIGHTS fd 传递、pty、`statx` 优雅回退等。
- **FPSIMD / TLS 上下文切换**：NEON 状态、`TPIDR_EL0` 在切换时保存恢复（否则 musl/crypto/memcpy 会出错）。

### 文件系统
- **`Lateran`（ext2）**：挂载时检测格式——ext2(0xEF53)优先、否则回退 FAT16。ext2 解析
  superblock/块组/inode/目录项，支持 **direct + 单/双/三重间接块**（三重间接让 >64MiB 的大文件、
  如 JDK 的 143MiB `lib/modules` 可读）、真路径解析、真 inode 元数据（mode/uid/gid/时间）；
  写侧：块/inode 位图分配、写文件、`mkdir`/`unlink`/`rename`、目录项管理。**2MiB 块缓存** + **dentry 缓存**
  + **顺序检测 read-ahead**。
- **`Bookshelf`（堆上可写）/ `GrimoireFS`（只读烤镜像）**：内存文件系统，`ls`/`cat` 统一查看。
- **`StiylMagnus`（virtio-9p）**：主机目录共享（9P2000.L，读写双向）挂到 guest `/host`。

### 网络
- **`MisakaMail`（virtio-net）+ `Antenna` 套接字层**：以太网 / ARP / IPv4 / ICMP / **UDP / TCP**，
  **DHCP**（自动取 IP）+ **DNS**；Linux ABI 的 socket 系统调用（AF_INET/AF_UNIX、bind/connect/listen/
  accept/recvmsg…）全部经 SLIRP。`net ping` 出站、`net serve` 入站应答。
- **`Underground`（PCIe 主桥 / ECAM）**：枚举 PCIe、分配 BAR、开 Bus Master；virtio-blk/net 同时支持
  **virtio-mmio 与 virtio-pci** 两种传输。

### 设备
- `Underline`（virtio-blk，mmio + pci）、`MisakaMail`（virtio-net）、`RandomVector`（virtio-rng）、
  `Tsuchimikado`（virtio-serial）、`StiylMagnus`（virtio-9p）、`ElectroMaster`（PL011 UART）、
  `IdolTheory`（PL031 RTC）、`Aleister`（GICv2/v3）、`ArtificialHeaven`（framebuffer）。

### 用户态（rootfs 里的真实程序）
- **busybox**：`sh` 交互 + 脚本文件 + `#!` shebang（`fcntl F_DUPFD` 真实现 + console POLLIN 条件化）。
- **OpenSSH 9.9**：`sshd` 在真机 **Apple HVF + GICv3 + SMP=8** 完整登录跑通（KEX + 公钥认证 + exec + 多会话）。
- **OpenJDK 26**：`java`（含 AppCDS 自动归档，暖启亚秒）与 **`javac`**（编译器，能编译 + 运行）都工作。
- **完整 C++ 运行时**：libstdc++.so.6 / libc++、std::thread/mutex/condition_variable/async、
  thread_local、jthread 全部可用。

---


---

## 目录结构

```text
src/
  arch/aarch64/   启动代码、异常向量(IRQ / EL0 svc)、misaka_switch、user_switch(EL1↔EL0)、CPU 辅助
  drivers/        ElectroMaster(UART) / Aleister(GIC) / Othinus(PSCI) / Underline(virtio-blk) /
                  Underground(PCIe) / MisakaMail(virtio-net) / RandomVector / Tsuchimikado / 其它 virtio
  index/          内核核心 + DTB / 调度器 / 同步原语 / 内存(Teleport/PersonalReality/DarkMatter/TreeDiagram) /
                  文件系统(Lateran/ext2/Bookshelf/GrimoireFS) / 网络(antenna/dhcp/dns) /
                  usermode + linux_abi(Linux ABI/启动栈/ptrace/epoll/inotify/tmpfs) / Necessarius
  user/           嵌入镜像、在 EL0 运行的 Index 原生用户程序
userprog/         独立编译成 ELF、从磁盘加载运行的程序 + 用户态共享 libc(academy_city) + Linux-ABI 测试
tools/            镜像构建脚本
linker.ld         裸机链接脚本（higher-half VA）
Makefile          交叉构建 + UTM/QEMU 运行目标
```

---

## 关键技术决策

- **PSCI conduit 从 DTB 读**：客户机在 EL1 时 `method = "hvc"`，`virtualization=on`（EL2）时变 `"smc"`；
  硬编码任一种在另一配置下会异常，运行时读 DTB 才稳。
- **用虚拟定时器（CNTV）**：HVF 占着 EL2 时 EL1 访问物理定时器（CNTP）会被陷入抛异常；CNTV 两种情况都能用。
- **MMU 用 39 位 VA**：T0SZ=T1SZ=25 从 level 1 起步；`virt` 的 RAM 从 0x40000000 起、低于它全是 MMIO，
  L1 entry 0 设 device、其余 normal 即正确分型。PA 位宽从 `ID_AA64MMFR0_EL1` 读（TCG 与 Apple host 不同）。
- **一张页表当 TTBR0+TTBR1**：高半区 VA 低 39 位 = 物理地址，保留恒等别名让 device MMIO / DTB / 物理栈
  始终可达，higher-half 切换不会把串口变黑。
- **virtio-blk 用 doorbell-poll 而非完成中断**：HVF 上 qemu 主循环只在 vCPU vm-exit 时推进，doorbell（MMIO
  store）的 vm-exit 即完成请求；完成中断（enable SPI）在此平台会 hang，故用 doorbell 轮询（间隔调优过）。
