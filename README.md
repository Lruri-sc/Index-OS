# arm-Index

`arm-Index` 是 [Apple-Index](../Apple-Index) 内核面向 **UTM** 的移植分支。原工程以
Apple Silicon + m1n1 为第一目标，本分支把第一目标换成 **UTM 的 QEMU 后端、ARM64 `virt`
机器**（Apple Silicon 上可开 HVF 加速，跑得很快；其它 Mac 上用 TCG 仿真），并据此调整
平台识别、启动横幅和构建/运行流程。

> 重要：必须用 UTM 的 **QEMU 后端**，**不要用 Apple Virtualization 后端**。后者是
> Virtualization.framework，串口是 virtio-console 而不是 `virt` 机器的 **PL011**；
> 本内核只驱动 PL011，在 Apple Virtualization 下不会有任何串口输出。HVF 只是 QEMU
> 后端的一个加速开关，和 Apple Virtualization 不是一回事。

内核仍是 C++20 freestanding，命名沿用《魔法禁书目录》词条；硬件事实写在注释里。

## 与 Apple-Index 的差异

- **ext2 文件系统（`Lateran` 进化）—— 正宗 Unix 路径 + 真权限 + 真 rootfs**:`Lateran` 现在挂载时
  **检测磁盘格式**——ext2(magic 0xEF53)优先,否则回退 FAT16,公共 API 不变、内部路由,usermode/linux_abi **零改动**。
  ext2 后端(`src/index/ext2.cpp`)解析 superblock/块组描述符/inode/目录项,支持 direct+单/双间接块、长文件名、
  按 `/` 的真路径解析、**真 inode 元数据**(mode/uid/gid)。写侧:块/inode **位图分配**(同步 superblock+组描述符
  空闲计数)、写文件、`mkdir`(新 inode+`.`/`..`+父链接数)、`unlink`(链接数到 0 释放)、目录项 slack 切分/扩块。
  rootfs 用主机 `genext2fs` 从目录树造(`make run-qemu-ext2`),布局正宗 FHS:`/lib/ld-musl-aarch64.so.1`、
  `/bin/<程序>`、`/etc`、`/tmp`。**动态程序从真 `PT_INTERP` 路径加载解释器**(退役了硬映射 hack)。铁证:
  `exec /bin/hello`(**动态** musl)从真 `/lib` 加载解释器跑通、`exec /bin/dirtest` `mkdir WORK`+写文件+`readdir`、
  `exec /bin/writefile` 写 `/OUT.TXT` → **重启** → `cat /OUT.TXT` 仍在 → 主机镜像+空闲计数都正确;FAT 镜像零回归。
  命名:`Lateran`(罗马正教大档案库)本就是"档案库",配带层级+权限的完整 FS 更贴。
- **可写 FAT16（`Lateran` 写回）—— Linux 程序写的文件能存到磁盘、跨重启还在**:`Lateran` 之前只读。
  现在整条写链路打通,**真持久化**:
  - **`Underline` virtio-blk 写**:`underline_write` 对称于读(请求类型 `VIRTIO_BLK_T_OUT`、数据描述符**设备可读**、
    状态字节设备写、轮询 used 环)。`diskwrite` 命令写已知模式到末扇区再读回比对。
  - **`Lateran` FAT 写层**:`fat_set`(写回**所有 FAT 副本**)、`alloc_cluster`(扫空闲簇+标 EOC+零化)、`free_chain`、
    `to_83`/`find_dirent`/`find_free_slot`(8.3 目录项管理)。`lateran_write_file` 创建/覆盖文件:释放旧簇链 →
    分配新链 → 写数据扇区 → 更新目录项(首簇+大小);`lateran_unlink` 释放簇链+标删目录项。
  - **Linux 写系统调用**:`openat` 解析 `O_CREAT/O_TRUNC/O_WRONLY/O_RDWR/O_APPEND`,`write(fd)`/`writev` 对可写文件
    fd 走 **read-modify-write**(读整文件→在偏移处拼接→写回),`unlinkat` 删文件。fd 加 `writable` 标志。
  - **元数据预留**:`LateranEntry` 带 `uid/gid/mode`(FAT 存不下 Unix 元数据,故合成默认值:dir `0755`/file `0644`,
    FAT 只读属性剥写位),内核 FS 接口已就绪,给 Phase H 用户模块平滑扩展;完整持久化留待后续。
  - **子目录 + 路径解析 + mkdir**:目录操作泛化成 `DirLoc`(根=固定扇区 / 子目录=簇链),`dir_nth_sector`/
    `dir_find`/`dir_free_slot`(子目录满了自动扩一簇) 对两者通用;`resolve_parent` 按 `/` 解析路径到
    (父目录, 叶名)。`lateran_mkdir` 分配簇、写 `.`/`..`、在父目录建目录项;`lateran_list_dir` 列任意目录。
    Linux 侧:`mkdirat(34)`、`unlinkat(35)`、`getdents64(61)`(opendir/readdir,struct linux_dirent64),
    `openat` 对目录也给 fd(给 getdents 用)。
  铁证(`make run-qemu-fat`,已加 `cache=directsync` 让写直达主机镜像):
  - `WRITEF.ELF`(musl `open(O_CREAT)`+`write`)写 `OUT.TXT` → **重启全新 VM** → 内核 `cat OUT.TXT` 仍读出。
  - `DIRTEST.ELF`(musl `mkdir`+`open("WORK/LOG.TXT")`+`write`+`readdir`)建子目录、写文件、列出 `./ ../ LOG.TXT`
    → 重启后 `cat WORK/LOG.TXT` 仍在 → 主机 `.img` 里 `WORK` 目录和内容**真落盘**。
  内核 shell 也有 `fwrite <名> <文本>` / `frm <名>` / `diskwrite` 直接测。**至此 Linux 程序能创建目录、写文件、
  并把结果持久化到磁盘——能干活、能保存。**
- **TTY 行规程（termios + 原始/熟模式 + Ctrl-C→SIGINT）—— 能跑交互式程序**:之前控制台只有
  固定的行缓冲轮询读,任何切 raw 模式的交互程序(行编辑、读单键、TUI)都跑不了。现在给 Linux 路径加了
  **termios 状态**:`ioctl(TCGETS/TCSETS)` 让程序读/改终端属性(`isatty()` 也靠 TCGETS 认出 fd 0/1/2 是
  终端),`read(fd 0)` 按 `c_lflag` 分流——**canonical(熟)模式**做行编辑 + 按 `ECHO` 回显、newline 返回;
  **raw(原始)模式**逐字节返回、不回显(程序自己显示)。`ISIG` 置位时 **Ctrl-C(VINTR=0x03)** 触发 `SIGINT`:
  装了 handler 就经 sigframe 跑 handler、被中断的 read 返回 `-EINTR`(经 `rt_sigreturn` 恢复),没装就默认终止。
  铁证 `TTY.ELF`(musl):`isatty(0)=1`,切 raw 后输入 `abcXY` 逐键读出(无回显),`a b <Ctrl-C> d e` 时
  第 3 键报 `<SIGINT caught>`(handler 跑了)、程序继续。**范围**:Ctrl-C 在程序阻塞读 stdin 时工作(同步);
  "纯计算时异步打断"需要把 PL011 RX 中断接 `Aleister` + 前台进程组,留作后续。
- **物理内存回收 + 真 CoW —— 系统能无限期跑下去**:之前每个进程退出都**永久泄漏**它的页表和所有
  缺页装入的物理页(`TreeDiagram` 是只进不出的 bump 分配器,`fork` 还是 eager 整页拷贝),连续跑十几个
  程序就会耗尽物理页起不了新进程。现在:① `TreeDiagram` 加 **free-list**(freed 页经高半区别名串成栈),
  `allocate_page` 先取回收页;② `PersonalReality v2` 加**按物理页的引用计数**(从 RAM base 索引的数组),
  叶子页可被多个地址空间共享、最后一个释放时才还给 `TreeDiagram`;③ **真 CoW fork**:`pr2_fork` 不再
  eager-copy,而是父子**共享物理页 + 双方置只读 + refcount++**,第一次写触发权限 fault → `pr2_handle_fault`
  复制出私本(`refcount==1` 直接升可写,`>1` 才拷);④ `pr2_destroy` 进程退出时遍历页表,叶子页 `page_unref`
  (0 才 free)、回收每个 L2/L3/L1 表页(对比内核 L1 跳过继承的内核条目);⑤ **地址空间引用计数**让
  `clone(CLONE_VM)` 的线程共享地址空间、**末线程退出才拆**,exec 替换旧地址空间也回收。铁证:`tree` 看
  可用页数,跑 `FORKLOOP.ELF`(20 轮 fork+CoW+exit)前后**页数完全不变**(258708→258708);跑完全部 7 种
  Linux 程序(动态链接/线程/fork/mmap)后页数**回到启动基线**——零泄漏。这是"演示" → "能一直活着"的分水岭。

- **Linux ELF 兼容（Phase A–E）—— 真·未修改的 Linux 二进制(静态 + 动态链接)在自己内核上跑起来**:
  整条 "拿 stock Linux/aarch64 工具链编出来的 ELF 直接执行" 链路打通,**静态和动态链接都行**。
  铁证(`make run-qemu-fat`,需要 `zig`):
  - `komoe$ HMUSL.ELF`(`zig cc -target aarch64-linux-musl -static`)→ `hello musl: a real Linux
    binary on arm-Index` + `exited code 0`,**零 ENOSYS**。
  - `komoe$ HDYN.ELF`(同程序 `-dynamic`)→ 内核把它的 `PT_INTERP=/lib/ld-musl-aarch64.so.1` 映射到
    盘上的 `LD-MUSL.SO`(vendored 的真 musl 动态链接器),在 `0x50_0000_0000` 加载、**自重定位**、
    解析符号、跳主程序,同样打印并退出 0。`malloc`/`strcpy`/格式化 `printf` 等经 ld.so 解析的库函数全部正常。
  五个阶段:
  - **A 双 ABI 派发**:`Esper` 加 `Abi { Index, Linux }`,`sniff_abi` 在加载时按 ELF 头嗅探
    (PT_INTERP → Linux dynamic;EI_OSABI=3 → Linux;ET_EXEC → Linux;e_entry ≥ 0x400000 → Linux;
    否则 Index)。`el0_sync_dispatch` 的 SVC 分支顶端按 `Esper::abi` 二选一:Index 走原 1..15 的
    switch,Linux 走新建 `linux_syscall_dispatch`(Linux AArch64 编号)。两表号段不交集,Index 零回归。
    Index 的 exit 提取成共享 `exit_and_schedule`,Linux exit/exit_group 复用,所以 fork/exec/wait
    对 Linux 子进程同样成立。
  - **B 按需分页 + 自由地址空间**(`PersonalReality v2`):取代 v1 的固定 14 页池。每个 Linux `Esper`
    有一张从 `TreeDiagram` 物理页建的私有 L1,L2/L3 按缺页**惰性分配**;一张 ≤32 项的 **VMA 表**
    (`Vma{start,end,prot,kind,file_src,file_off,file_size,seg_pad}`)是缺页判定的依据。EL0 data/instr
    abort 路由到 `pr2_handle_fault`:命中 VMA → 取物理页 → 零填或从 ELF 文件副本拷段(用 `seg_pad`
    处理 PT_LOAD 不页对齐的偏移、`file_size` 之外的 `.bss` 留零)→ 装 L3 → TLBI;未命中才报 fault 杀进程。
    铁证:`HELLOLX.ELF`(PIE,低 VA)和 `HELLOHI.ELF`(ET_EXEC,链接在 `0x400000`)都跑通——证明任意 VA、
    内核没预映射的区域也能 demand-page。
  - **C SysV 启动栈**:`linux_build_startup_stack` 在栈顶按 AArch64 SysV ABI 摆 `argc/argv/envp/auxv`
    (`AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/RANDOM/...`),用 `pr2_write_user` 写进 demand-paged 栈(faulting
    页随写随入)。`AUXV.ELF` 手写汇编遍历 auxv 打印,核对 `AT_PHDR=0x400040`、`AT_ENTRY`、`AT_RANDOM`
    指针全部正确、`argc=1`。
  - **D Linux 系统调用表**:`linux_syscall_dispatch` 实现 musl 启动 + printf 所需的一组:
    `write`/`writev`(经控制台)、`read`/`ioctl`(TIOCGWINSZ)/`fstat`/`newfstatat`(让 stdio 选行缓冲)、
    `brk`(在 Brk VMA 内移动 break)、`mmap`(MAP_ANONYMOUS 在进程 mmap 区 bump 出 VMA)/`munmap`/`mprotect`、
    `set_tid_address`/`set_robust_list`/`rt_sig*`(no-op)、`tgkill`(转 `Fortis931`)、`getrandom`/
    `clock_gettime`/`uname`/`get*id`、`exit`/`exit_group`。未实现的统一 `-ENOSYS` 并打印号码便于排查。
  - **E 动态链接**(`PT_INTERP` + 解释器加载):主程序的 `PT_INTERP` 一旦存在,内核把解释器名映射到盘上
    `LD-MUSL.SO`、在 `kLinuxInterpBase`(0x50_0000_0000)用同一套 `map_elf_loads` 建解释器的 PT_LOAD VMA、
    把初始 PC 设成**解释器**入口、auxv 里 `AT_BASE`=解释器 base、`AT_PHDR/AT_ENTRY` 仍指主程序——之后
    `R_AARCH64_*` 重定位全由 ld-musl 自己在 EL0 完成,内核一个 reloc 都不碰。为容纳 633 KiB 的解释器,
    `DarkMatter` 堆 256 KiB→**8 MiB**、ELF 读缓冲 64 KiB→1 MiB(都是 .bss 静态增长,1 GiB RAM 无压力)。
    解释器 ELF 副本和主程序一样常驻堆(file-backed VMA 每次缺页从它读),`exit`/`exec` 一并释放。
  另外修了一个**所有 EL0 程序都受益的底层 bug**:内核 `-mgeneral-regs-only` 从没给 EL0 开 FP/SIMD,
  `boot.S` 新增 `enable_fp_simd` 在每个核 EL2→EL1 后置 `CPACR_EL1.FPEN=0b11`——否则 musl 优化版 memcpy
  的 NEON 指令会 trap(`EC=0x07`)。静态 hello 恰好没踩到,动态链接器一上来就踩了。
  测试二进制:`userprog/hellolinux.S`(手写 Linux 汇编,验 A/B/C)、`userprog/auxvdump.S`(验 C)、
  `userprog/hellomusl.c`(zig+musl 编,`-static` 验 D、`-dynamic` 验 E)、`userprog/ld-musl-aarch64.so.1`
  (vendored 的真 musl 动态链接器)。命名上**不引入新词条**——Linux ABI 是外部标准,内核侧实体仍叫自家名
  (`PersonalReality`/`Esper`/`Fortis931`),只有 `Abi` 枚举和 `linux_abi.cpp` 是中性技术名。
- **Linux ELF 兼容 Phase F —— 文件IO / 信号 / fork / 线程 / mmap,真实程序的完整运行时**:
  在静态+动态加载(A–E)之上补齐让真实 Linux 程序"能干活"的运行时,分五波,每波用 zig+musl 编的真程序验证:
  - **文件 I/O**:`openat`/`read`/`readv`/`lseek`/`fstat`/`close` 复用 Esper fd 表 + Lateran/Bookshelf 查找。
    铁证 `CATFILE.ELF`(musl `fopen`/`fread`)打印盘上 `HELLO.TXT`。
  - **信号投递**:`rt_sigaction` 存 handler;`kill`/`tkill`/`tgkill`/`raise` 在用户栈上构造 Linux aarch64
    **rt_sigframe**(siginfo + ucontext/mcontext,偏移精确匹配)、跳 handler、`rt_sigreturn`(139) 恢复;
    自信号用活的 trap frame、跨 Esper 信号改其保存上下文(经 `Fortis931`)。铁证 `SIGTEST.ELF`:
    `signal(SIGUSR1)` → `raise` → `handler: caught signal 10` → 回到 main `caught=10`。
  - **fork + wait**:`clone`(无 CLONE_VM)经 `pr2_fork` 把父地址空间**整页 eager-copy** 成独立子空间
    (read-only ELF 镜像引用计数共享);`wait4` 复用 Esper wait 机制 + Linux 状态编码。铁证 `FORKTEST.ELF`:
    `fork`+`waitpid` → `reaped pid=7 WIFEXITED=1 code=42`。
  - **线程**:`clone(CLONE_VM)` 建共享地址空间的 Esper(同 TTBR0/VMA/镜像),用 clone 给的栈 + `CLONE_SETTLS`
    的 TLS;`futex`(WAIT/WAKE,按用户 VA 的**物理地址**做等待队列)+ `set_tid_address`/`clear_child_tid`
    (线程退出零写+futex 唤醒 joiner)。每线程 `TPIDR_EL0` 在上下文切换时**保存+恢复**(否则会冲掉
    musl 用户态设的 TLS)。铁证 `THREAD.ELF`:`pthread_create`+`join` → `worker: sum(1..100)=5050` →
    `main: joined, returned 5050`。
  - **文件 mmap**:`mmap(fd, MAP_PRIVATE)` 把文件快照进常驻缓冲、建 File VMA,缺页从中拷入(私有页,写不回盘);
    `mprotect` 真改 VMA 权限 + 重写已映射页(否则 musl 把线程栈 mmap 后 mprotect 成可写会死循环)。
    铁证 `MMAP.ELF`:`mmap` 后经指针读出 `HELLO.TXT` 内容。
  另修的底层点:缺页处理器现在能服务**权限 fault**(present 页按 VMA 当前 prot 重算,支持 mprotect 升权);
  ELF 镜像加载后**缩成实际大小**(原先每进程驻留 1 MiB 读缓冲,几个程序就把堆撑爆)。命名仍不引入新词条。
  Phase F 之后真正的 CoW(现 fork 是 eager-copy)、socket、epoll/poll 等留待后续。
- **新增 `AcademyCity`（用户态共享 libc）—— 所有 Esper 程序的家**:学园都市是所有能力者
  共同的家;这里是每个 EL0 程序静态链接的迷你 libc。提供字符串(`ac_strlen`/`ac_strcmp`/`ac_memcpy`/
  `ac_memset`/`ac_parse_uint`)、原始 IO(`ac_putc`/`ac_puts`/`ac_putln`/`ac_putn`/`ac_putx`/`ac_getln`)、
  以及 **`ac_printf`**(支持 `%d`/`%u`/`%x`/`%s`/`%c`/`%%`,内部用 128 字节行缓冲减少 syscall),全建
  在 `usys.h` 的原生 syscall 包装之上。设计:v1 **没有 malloc**——任何 `static char heap[]` 全局都需要
  运行时重定位,而我们的 PIE 加载器不应用 `.rela.dyn`(只处理 PT_LOAD),所以 AcademyCity 严格保持
  栈分配 + 字面量常量。构建:`academy_city.cpp` 预编译成 `academy_city.uo`,每个用户 ELF 在 `ld -pie`
  时把它链进去(`-Wl,--gc-sections` 自动剔除没用到的),库本身一份编译被所有程序共用。已迁移
  `INIT.ELF`/`WC.ELF`/`LOOPER.ELF` 改用 `ac_printf`:`INIT.ELF` 从一连串 putc 改成
  `ac_printf("pid=%d i=%d\n", pid, i)`,`WC.ELF` 从三段 `uputs`+`uputdec` 拼接改成
  `ac_printf("wc: bytes=%d words=%d lines=%d\n", ...)`——读起来跟 stdio 的 printf 几乎一样。铁证:
  `INIT.ELF` 输出与迁移前**完全相同**(`pid=2 i=0\n` 三次),`INIT.ELF | WC.ELF` → `wc: bytes=30 words=6
  lines=3`(经 `Aiwass` 管道与迁移前字节完全一致),`WC.ELF < HELLO.TXT` → `wc: bytes=35 words=6 lines=1`
  (经 `<` 重定向)。`KOMOE.ELF`/`FORKDEMO.ELF`/`SPIN.ELF` 保留原生 usys.h 写法(它们的工作流更适合手写
  helper,且 academy_city.h 本身 include 了 usys.h,无回归)。命名取自学园都市本身——所有 Esper 的家。
- **新增 `Fortis931`（kill + 信号 MVP）—— `kill <pid>` 能从远处把进程烧掉**:Stiyl Magnus 的远隔
  火葬咒文,凭名字就能烧死目标;这里是 signals 的第一层。新增 `kill(pid, sig)` 系统调用(15)和
  Komoe 的两个新语法 `cmd &`(后台 fork+exec 不 wait,打印 pid)和 `kill <pid>`(发 SIGTERM)。
  设计:`fortis931_kill(target_slot, sig)` 一次性收尾被杀对象 —— 释放它的所有管道引用(读端少了
  写者看到 EPIPE,写端少了读者看到 EOF)、置 `exit_code = 128 + sig`、置 `EsperState::exited`,
  若它的父进程正在 `wait()` 阻塞(用 `wait_pipe_idx < 0` 跟 pipe 阻塞区分)就唤醒父并直接交回 pid +
  退出码完成 reap。**MVP 不做**:信号处理函数注册(`sigaction`)、`sigreturn`、信号掩码、SIGCHLD,
  以及异步 Ctrl-C(需要把 UART 中断接到 `Aleister`,目前 UART 是轮询)。SIGINT/SIGKILL/SIGTERM
  当前全是"默认终止",区别将在加入 handler 时才出现。新增 `LOOPER.ELF`(永远循环打印 pid+i)作为
  kill 目标用于端到端验证。铁证:`make run-qemu-fat` → 进 Komoe → `LOOPER.ELF &` 后台 → `kill 2`
  → 看到 `[komoe] SIGTERM -> pid 2`,LOOPER 立刻停止输出。命名取自 Stiyl 的咒文 Fortis 931
  (远隔火葬),与已有 `Aiwass`(管道,阿莱斯特的守护灵通讯)同属"魔法侧"主题。
- **新增 `Aiwass`（管道 + dup/dup2）—— Komoe 能 `cmd1 | cmd2`**:阿莱斯特唯一的通讯对象是守护灵
  Aiwass,一对一私语;这里是 Esper 之间的一对一字节流。新增 3 个系统调用 `pipe`/`dup`/`dup2`,
  每个 Aiwass 槽是一块 4 KiB 环形缓冲(从 `DarkMatter` 堆分配)+ 两侧引用计数(`read_refs`/
  `write_refs`),`Fd` 多了 `pipe_read`/`pipe_write` 两种新种类。读空管道时若有写者就**阻塞**:
  调度器**把 ELR 倒回 4 字节**让 svc 在唤醒时重新执行,标 Esper `waiting` + `wait_pipe_idx`、
  挑下一个就绪 Esper 切过去;写者写入后扫等待者把它们翻回 `ready`(write 阻塞→读者取走数据后唤醒
  对称)。最后一个写者关闭 → 读者下次重跑 svc 看到 `write_refs==0` 拿到 0(EOF);最后一个读者
  关闭 → 写者拿到 `EPIPE`(-1)。`fork` 时自动给孩子继承的 pipe fd 加引用,`exit`/`close`/`dup2`
  释放或转移引用,两侧都到 0 时把环形缓冲还 `DarkMatter`。`SYS_putc` 也改成走 fd 1(沿用)以让
  老用户程序自动尊重重定向。**Komoe 现在懂 `<` 和 `|`**:`cmd < file` open+`dup2(fd,0)` 后 exec;
  `cmd1 | cmd2` `pipe()` → fork 两个子进程、每边 `dup2` 到对应一端再 close 两端,父进程关掉自己
  两端再 `wait` 两次。新增 `WC.ELF`(读 stdin 计算字节/词/行数)用于端到端验证。铁证(`make run-qemu-fat`):
  `INIT.ELF | WC.ELF` → `wc: bytes=30 words=6 lines=3`(INIT 3 轮 `pid=2 i=N\n` 经 Aiwass 给 WC);
  `WC.ELF < HELLO.TXT` → `wc: bytes=35 words=6 lines=1`(磁盘 35 字节经 `<` dup2 到 stdin);
  `FORKDEMO.ELF | WC.ELF` → `bytes=118 words=18 lines=4`(父子两进程都继承 fd 1=pipe_write,grandchild
  写入同一管道也被 WC 读到)。
- **新增 `MisakaMail`（virtio-net 网卡 + 网络栈）—— 内核能联网**：在 `Underground` PCIe 总线上挂
  **virtio-net** 网卡（vendor `0x1af4`、device `0x1000`/`0x1041`，复用与 virtio-blk 完全相同的 modern
  建立流程，只是 **两条队列** RX/TX + 每缓冲 12 字节 `virtio_net_hdr`；轮询 used ring，**不用中断**），
  并手写一个最小网络栈:**以太网 / ARP / IPv4 / ICMP**（含 IP/ICMP 校验和）。协商 VERSION_1 + MAC、从
  device-config 读出网卡 MAC。`net ping [ip]` **出站**:ARP 解析下一跳 → 发 ICMP echo → 收 echo reply
  打印来源/seq/RTT;`net serve` **入站**:循环应答别人对本机的 ARP 请求和 ICMP echo（让本机可被 ping）。
  铁证:`make run-qemu-net`(QEMU SLIRP 用户网络)下 `net ping` → `reply from 10.0.2.2 seq=0`,且
  `filter-dump` 抓的 pcap 经 `tcpdump` 解析是**格式完全正确**的 ARP 请求 + ICMP echo（校验和对，否则
  对端不会应答）。命名取自妹妹们在网络上互传信号的信箱,与 `MisakaNetwork`(调度器)、`RadioNoise`(内核
  信箱)同属妹妹主题。队列只被前台 `net` 命令访问,**无需加锁**。
- **新增 `Underground`（PCIe 主桥）+ virtio-blk over PCIe**：UTM app 在 GUI 里加的「VirtIO」磁盘其实是
  **`virtio-blk-pci`**（挂 PCIe 总线），而 `Underline` 原本只扫 virtio-MMIO，所以 UTM 里一直 `no block
  device`。新增 `Underground`——PCIe 主桥 / ECAM 配置空间枚举器：从 DTB 读 ECAM base(`virt` 是
  `0x4010000000`，现代 high-ecam)，按 bus:dev:fn 访问配置空间，扫总线找设备(vendor `0x1af4` 的 virtio-blk
  `0x1001`/`0x1042`)，给 memory BAR 从 `0x10000000` 窗口分配地址并开 Bus Master(`-kernel` 直引导无固件，
  BAR 未分配，得自己来)。`Underline` 随之学会**virtio modern over PCI**：解析 vendor-specific 能力链定位
  common/notify/device 三个结构、用 common-config 建队列、算出 notify 地址——**块传输核心(描述符链 +
  avail/used 轮询)与 MMIO 完全共用**，只是换了发现/建队列/notify 三处。因为是轮询，**不需要 PCI 中断/MSI**。
  `Teleport` 顺带把 ECAM 所在 GiB 映射成 device 内存。铁证:`make run-qemu-fat-pci`(用 `virtio-blk-pci`,
  精确复刻 UTM GUI)启动出 `Underline: 16384 sector(s)` + `Lateran: 7 file(s)`,`komoe`/`cat`/`exec` 在 PCI
  盘上照常;`make run-qemu-fat`(MMIO)零回归。**UTM GUI 里挂一块 VirtIO 盘指向 `fat.img` 即可直接用**。
- **fork/exec + 系统调用（真正的进程模型）**：`Esper` 现在能自我繁殖和换装——新增 7 个系统调用
  `fork`/`exec`/`wait`/`write`/`read`/`open`/`close`。`fork` 把当前进程的地址空间**急切整页拷贝**进
  一个子 `Esper` 槽(扩展 `PersonalReality` 的 `personal_reality_fork`),父返回子 pid、子返回 0;
  `exec` 从 Lateran 盘把 PIE ELF 原地换进当前槽;`wait` 阻塞父进程到子退出、回收子并取回退出码(子退出
  时父地址空间不在场,退出码**延迟到父被重新装载 TTBR0 时回写**)。`write`/`read` 走控制台(read 行
  输入+回显),`open`/`read`/`close` 读 Lateran/Bookshelf/GrimoireFS 文件(每 `Esper` 一张小 fd 表,
  0/1/2 为控制台,fork 时连同打开的文件一起继承)。压轴是 **`Komoe`**——一个跑在 **EL0** 的迷你 shell
  (月詠小萌,照管学生/进程的老师;内核 shell 是 `Necessarius`,这是用户态那一个):读一行 →
  `fork`+`exec`+`wait` 跑磁盘程序、`cat <文件>` 走 `open`/`read`/`write`/`close`、`exit` 回内核。铁证:
  `exec FORKDEMO.ELF` 打印父/子两行 + `reaped pid=N code=7`;`komoe` 里敲 `INIT.ELF` 能 fork/exec/wait
  跑它、`cat HELLO.TXT` 读出盘上真文件、嵌套 fork(Komoe→FORKDEMO→孙进程)也对;坏文件 `exec`/`open`
  返回 -1 而不崩。Esper 全程**在主核单线程**跑,所以这套**不需要锁**(`DarkMatter` 堆已是 SMP 安全)。
- **SMP 多核（启动其它核 + 全面加锁）**：`ArtificialHeaven` 从 DTB 的 `/cpus` 读出每个核的
  MPIDR；`kmain` 在主核上经 `Othinus` 的 PSCI `CPU_ON` 逐个唤醒副核。每个副核在自己的入口
  （`secondary_entry`，物理地址、MMU 关）里复用主核已建好的页表打开 MMU（`teleport_enable_secondary`，
  共享的 inner-shareable 页表 + 主核 `dc cvac` 到 PoC 让关 MMU 的副核读得到寄存器配置），初始化
  本核 GICC（`aleister_init_cpu_interface`）和本核虚拟定时器（`last_order_arm_secondary`，PPI 27
  banked，**只有主核累加全局心跳**），把逻辑核号写进 `TPIDR_EL1`，然后把自己变成本核的 idle
  `Sister` 加入调度。`MisakaNetwork` 升级成**真正跨核并行**：每核有自己的 `current`，所有 runqueue
  状态由一把新自旋锁 `AntiSkill g_sched_lock` 保护，调度器锁**持锁过 `misaka_switch`、由被切入方
  释放**（恢复的上下文走自己的 `unlock`，全新 Sister 在 `sister_trampoline` 里释放），新增 `running`
  状态 + owner 核杜绝两核抢同一个 Sister。三个分配器（`TreeDiagram`/`DarkMatter`/`IndexLibrorum`）、
  控制台、以及建在调度器之上的同步原语（`Imprimatur`/`Judgement`/`RadioNoise`）全部改用 `AntiSkill`
  加锁。EL0/`Esper` 这一版仍只在主核跑（收敛爆炸半径）。`make run-qemu SMP=4` 启动后 `smp` 列出
  各核 + 其当前 Sister；`spawn` 几个 worker 再 `sisters`，可见多个 Sister 同时 `run@<不同核>`、
  `work` 并行增长（铁证）。
- **EL2→EL1 启动降级(支持在 EL2 被引导)**：UTM/HVF(Apple Silicon)和 QEMU `virtualization=on`
  会把内核引导在 **EL2**,而内核配置的是 **EL1** 的 MMU/定时器寄存器——在 EL2 跑这些不生效,
  `teleport_enable` 打不开 MMU、第一次高半区 MMIO 直接卡死。`boot.S` 新增 `el2_to_el1`:开机若在
  EL2,设 `HCR_EL2.RW`(EL1=AArch64)、清 `SCTLR_EL1`、放开 EL1 定时器(`CNTHCTL_EL2`/`CNTVOFF_EL2`)、
  `eret` 降到 EL1h 再继续;主核和被 `CPU_ON` 唤醒的副核都先降级。PSCI 经 DTB 指定的 conduit(EL2 下是
  `smc`)照常工作(QEMU 在指令层拦截 PSCI)。用 `qemu -M virt,virtualization=on` 可复现/验证 EL2 路径。
- **第一目标改为 UTM / QEMU `virt`**：`Testament` 把该机器显示为 `UTM / QEMU virt`，
  启动横幅的 `target` 改为 `UTM ARM64 virt (QEMU) + DTB`。
- **新增 `Othinus`（PSCI 电源控制）**：从 UTM/QEMU 传入的 DTB 里读取 `/psci` 节点的
  `method`（`hvc` 或 `smc`）和 `compatible`，据此用正确的 conduit 调用 PSCI
  `SYSTEM_OFF` / `SYSTEM_RESET`，可以真正把 UTM 虚拟机关机或重启。找不到时对
  `virt` 默认回退到 EL1 的 `hvc`。
- **新增 `Aleister`（GICv2 中断控制器）**：从 DTB 读 GIC 的 GICD/GICC 地址，初始化
  distributor + CPU interface，按中断号注册/分发 handler，并做 ack/EOI。
- **新增 `LastOrder`（ARM 通用定时器 / 系统心跳）**：通过 `Aleister` 路由的定时器
  中断，每跳一次累加 tick 计数，是将来调度器的地基。用的是**虚拟定时器**
  （CNTV，中断号 27）而不是物理定时器（CNTP，27 号 vs 30 号），原因见下。
- **异常向量新增 IRQ 路径**：IRQ 进 `irq_entry`，把通用寄存器加上 `ELR_EL1` / `SPSR_EL1`
  存到**当前线程自己的栈**上，调用 C 分发器，再恢复并 `eret`。把 ELR/SPSR 也存进每个
  线程的中断帧，是抢占式切换能正确恢复 PC/PSTATE 的关键。
- **新增 `MisakaNetwork`（抢占式调度器）+ `Sister`（线程）**：每个 `Sister` 有独立栈和
  保存的寄存器上下文；每次 `LastOrder` 心跳在 EOI 之后做一次 round-robin 切换
  （`misaka_switch` 汇编换 callee-saved + sp）。带一个 idle Sister，可 `spawn` 出
  busy worker 观察抢占。
- **Sister 支持睡眠/阻塞 + 协作式 yield**：`misaka_network_sleep(ticks)` 把当前
  Sister 标成 `sleeping`（记下唤醒 beat）并让出 CPU；调度器每个 beat 唤醒到期的
  睡眠 Sister，`pick_next` 跳过没就绪的。`yield` / `sleep` 是从线程上下文发起的
  协作式切换（切换期间关中断保证原子）。
- **Sister 优先级（esper Level 0–5）**：调度器改为**严格优先级 + 同级 round-robin**——
  总是运行就绪 Sister 里 Level 最高的。shell 在 Lv5 永不挨饿，idle 在 Lv0。shell 等输入
  时改成 `sleep` 1 tick 轮询，好让低优先级 Sister 拿到 CPU。`levels` 命令 spawn 一个
  Lv4 和一个 Lv2 worker，可见 Lv4 把 Lv2 饿死。
- **新增 `Imprimatur`（计数信号量）+ Sister `blocked` 状态**：线程 `wait` 拿不到许可
  就进 FIFO 队列阻塞（`blocked`，只能被 `post` 唤醒，不受定时器影响）；`post` 把许可
  直接交给排队最久的线程（direct hand-off）。生产者-消费者用它做无忙等的交接。
- **新增 `Judgement`（互斥锁）**：二元 + 带 owner 的锁，建在调度器的 block/unblock 队列上；
  释放时把所有权**直接移交**下一个排队者。`judgement` 命令让两个 Sister 抢一把锁去
  改共享计数器，结束后 `共享计数器 == judge-A.work + judge-B.work`，证明没有丢更新。
- **优先级继承（`Level Upper`）防优先级反转**：Sister 现在分**自身 Level**（base）和
  **有效 Level**（可被临时拉高）。高 Level Sister 阻塞在一把被低 Level Sister 持有的
  `Judgement` 上时，把持锁者**临时提升到等待者的 Level**（`Level Upper`，原作里临时拉高
  能力等级的装置），让它越过中等优先级的 hog 跑完临界区并释放，解锁时再恢复。`invert`
  命令（别名 `pi`）摆出 Lv1 持锁 / Lv3 hog / Lv4 等待者的经典反转：只有靠继承，Lv4 的
  pi-high 才能取得进展（`sisters` 里能看到 `pi-low Lv1->4` 的临时提升和 boost 计数）。
- **新增 `RadioNoise`（有界阻塞消息队列 / 信箱）**：把已有原语**组合**起来——两个
  `Imprimatur` 信号量分别数空槽/满槽（满了 send 阻塞、空了 recv 阻塞），一个 `Judgement`
  锁护住环形缓冲下标。`radio` 命令（别名 `mailbox`）spawn 一个发送者(发递增序列)和一个
  更慢的接收者(校验顺序)：`errors` 恒为 0（FIFO 正确），`sent - recv` 恒等于缓冲容量
  （发送者被"满"卡住，`sisters` 里 rn-send 显示 `block`），一次验证整条并发栈协同工作。
- **新增 `DarkMatter`（内核堆 kmalloc/kfree）**：256 KiB arena 上的**首次适配 free-list**
  分配器，alloc 会按需切块、free 会把相邻空闲块**前向合并**，16 字节对齐。和 bump 分配器
  （只进不出）、页分配器不同，它能把内存**还回来**复用。`heap` 命令演示 alloc/free/复用/
  合并：释放中间块后再 alloc 同尺寸会**重用同一地址**，全部释放后空闲块合并回 1 块。
- **device MMIO 改走高半区别名**：`mmio::read32/write32` 给设备地址或上 `g_mmio_offset`
  (MMU 开之前 0、之后 `kHighHalfBase`),所有设备访问(PL011/GICv2/virtio)走内核 TTBR1
  高半区映射,**内核访问设备不再依赖 TTBR0**,把 TTBR0 腾给每个进程。
- **新增 `PersonalReality`(每进程独立地址空间)+ 真正的内存隔离**：每个磁盘加载的 `Esper`
  拿到一张私有 TTBR0 页表(从静态 4KB 页池建 L1/L2/L3),把 ELF 和栈映射到固定低地址
  (代码 `0x10000`、栈顶 `0x40000`)的**私有物理页**。切换 Esper 时切 TTBR0 + 刷 TLB
  (`enter_user` 启动时、yield/exit/抢占切换时)。每进程的 L1 继承内核映射(内核自己的低地址
  栈在 EL1 仍有效)但只覆盖 entry 0 给私有低 1 GiB,内核条目是 EL1-only,所以进程之间、
  进程与内核之间内存互相**隔离**。命名取自能力者私有的"个人现实"。
  - 铁证:`coexec INIT.ELF INIT.ELF` 两个进程都加载在**同一个 VA `0x10000`** 却是**不同物理页**
    (`PA 0x...f1000` vs `0x...ff000`)——各自独立地址空间。
- **新增 `Esper`(用户进程抽象 + 进程表)**：每个在 EL0 跑的程序是一个 `Esper`(能力者,
  与内核线程 `Sister` 区分),有 **pid、状态(ready/running/exited/faulted)、退出码、独立的
  arena 槽(加载区 + 栈)和完整 EL0 上下文**。`exec`/`user` 创建一个 Esper 并分配 pid;
  `SYS_exit(code)` 记录退出码,EL0 fault 记录为 faulted;`SYS_getpid` 返回真实 pid。
  `ps` 列出进程表。
- **并发多进程(协作式 + 抢占式)**：`coexec <a> <b>` 把多个 ELF 各加载到自己的 arena 槽并发跑。
  - **协作式**:`SYS_yield` 在 EL0 trap 分发器里保存当前 Esper 的整套 EL0 上下文(x0–x30 +
    SP_EL0/ELR/SPSR)、挑下一个就绪 Esper、恢复它再 eret。
  - **抢占式**:EL0 现在**开中断运行**(SPSR I 位清零)。timer IRQ 打进来时,IRQ 帧被传给
    分发钩子;若中断的是 EL0(SPSR.M==0),就**在 IRQ 帧里换掉当前 Esper 的上下文**,eret 直接
    恢复另一个 Esper —— 进程被时间片抢占,**不需要主动 yield**。`coexec SPIN.ELF SPIN.ELF`
    跑两个**从不 yield** 的纯计算程序,输出仍交替(`1212...`),即为抢占的铁证。
  - timer tick 现在分流:中断 EL0 → 抢占 Esper;中断内核 → 调度 Sister(`MisakaNetwork`)。
  - 某个 Esper fault 只终止它自己,其余 Esper 和内核继续跑。
- **新增 ELF 加载器(从磁盘加载并执行用户程序)**：`exec <名>` 从 `Lateran` FAT 盘读一个
  **位置无关 ELF**(ET_DYN/PIE),解析 ELF64 头和 PT_LOAD 段,加载进一块 EL0 可读写可执行的
  区域,跳到入口在 EL0 运行。用户程序([userprog/init.cpp](userprog/init.cpp))单独编译成
  PIE ELF(只用 svc + 立即数 → **零重定位**,放哪都能跑),由 `tools/mkfatfs.py` 打进 FAT 盘。
  完整跑通 **virtio-blk → FAT16 → ELF 解析 → 加载 → EL0 执行 → 系统调用** 这条链路。
- **新增 `Lateran`（只读 FAT16 文件系统）**：在 `Underline` 磁盘上读**真正的 on-disk 格式**。
  解析扇区 0 的 BPB、算出 FAT/根目录/数据区布局,列根目录(8.3 名,跳过长名/卷标/子目录),
  按 FAT 簇链读文件。和 `GrimoireFS`(烤在镜像)、`Bookshelf`(在堆上)并列,`ls`/`cat`
  统一查看(标 `[fat]`)。`tools/mkfatfs.py` 手工生成确定性的 FAT16 镜像(不依赖 mtools/
  newfs),`make run-qemu-fat` 一键挂上。
- **新增 `Underline`（virtio-blk 块设备驱动）**：UTM/QEMU `virt` 的 virtio-mmio 传输上的
  块设备。扫描 0x0a000000 起的 32 个 virtio-mmio 槽找 DeviceID=2 的块设备,做特性协商、
  建 split virtqueue(描述符表 + avail/used 环),用 3 个描述符(请求头/数据/状态)发读请求,
  轮询 used 环拿结果。**同时支持 legacy(version 1,QEMU/UTM 默认)和 modern(version 2)**
  两种 virtio-mmio。设备 DMA 用物理地址,驱动把高半区 VA 转成物理地址。`disk [扇区]` 命令
  读一个 512 字节扇区并 dump。这是真正的持久存储 I/O——读到的字节就是磁盘镜像里的内容。
- **新增 `Bookshelf`（可写文件系统）**：和 `GrimoireFS`（只读、烤在镜像里）相对，
  `Bookshelf` 是个**可写**的内存文件系统，文件内容存在 `DarkMatter` 堆上（write 时分配、
  rm 时释放）。`write <名> <文本>` 创建/覆盖、`rm <名>` 删除、`cat`/`ls` 跨两个 FS 统一
  查看（标 `[ro]`/`[rw]`）。覆盖会先释放旧内容，删除会把堆还回去。
- **新增 EL0 用户态 + 系统调用（内核→操作系统的分水岭）**：内核跑在 EL1，用户程序跑在
  **EL0**。用户的代码/栈页由 `Teleport` 标成 **EL0 可访问**（代码 AP=11+PXN：EL0 可执行、
  EL1 不可执行；栈 AP=01+NX），内核其余页仍是 EL1-only，构成真正的特权边界。
  `enter_user`/`leave_user`（汇编）在 EL1↔EL0 间桥接：保存内核上下文、`eret` 下到 EL0，
  用户 `exit` 时再 `leave_user` 解绕回内核。用户唯一的通道是 `svc`——
  `sync_lower_aarch64` 向量路由到 `el0_sync_dispatch`，按 ESR EC=0x15 识别系统调用
  （`putc`/`getpid`/`exit`，x8 传号、x0–x5 传参、x0 返回）。非系统调用的异常（用户碰内核
  内存）被当作 **EL0 fault** 报告并**终止用户、保住内核**。`user` 命令跑一个打印
  "Hello from EL0!" 的用户程序；`userfault` 跑一个故意越权的程序演示隔离。
- **新增 `GrimoireFS`（只读文件系统）**：一张烤进镜像的"grimoire"表（文件名 + 内容），
  支持 `ls` 列出、`cat <名字>` 打印。是 `Bookshelf`（未来 VFS）的第一张书架。
- **新增 `Teleport`（MMU / 地址翻译）+ 打开 MMU + W^X + higher-half**：
  - **三级页表**：L1（1 GiB）→ 内核所在 GiB 用 L2（2 MiB）→ 内核头 2 MiB 用 L3（4 KiB 页）。
    entry 0 是 device 内存（GIC/PL011/flash），RAM 是 normal write-back 可缓存。
  - **W^X**：靠细粒度 4 KiB 页给 `.text` 设只读+可执行、`.rodata` 只读+NX、`.data`/`.bss`
    读写+NX。`wxtest` 命令往 `.text` 写一下，会触发权限 fault（ESR EC=0x25、DFSC=0x0F、
    WnR=1），证明保护生效。
  - **higher-half**：内核 link 在高位 VA `0xFFFFFF8040080000`，`Teleport` 同时设
    TTBR1（高半区，内核）和 TTBR0（恒等别名，给 device MMIO 和 boot DTB）。因为 39 位区
    只看 VA[38:0]，高 VA 的低 39 位等于物理地址，所以**一张页表同时服务 TTBR0/TTBR1**。
    `kmain` 末尾把 VBAR 抬到高别名，再从高 VA 调 shell——之后 shell、调度器、异常处理都跑在
    高半区（`wxtest` 的 ELR/FAR 都是 `0xffffff80…` 即可佐证）。
- **链接地址改为高半区 VA**（见上）：内核以前 link 在地址 0、后来钉在加载地址 0x40080000，
  现在 link 在 `0xFFFFFF8040080000`。代码靠 PC 相对（`adrp`）寻址，MMU 关时仍在物理地址
  正常跑；只有静态数据里的**绝对指针**（如 GrimoireFS 字符串表）带高 VA，而它们只在跑进
  高半区之后才被解引用，正好由 TTBR1 翻译。
- **Necessarius 新增 `poweroff` / `reboot` / `uptime` / `sisters` / `spawn` / `sleeper`
  / `prodcons` / `levels` / `judgement` / `invert` / `radio` / `ls` / `cat` / `wxtest`
  / `heap` / `user` / `userfault` 命令**（别名 `off` / `reset` / `ticks` / `judge` / `pi`
  / `mailbox`）。
- **Makefile 改为 UTM 中心**：`run-utm`（QEMU+HVF）、`run-qemu`（QEMU+TCG）、
  `dist`（产出可直接拖进 UTM 的 `Image` + 引导说明），pin 了 `gic-version=2`，加了
  头文件依赖追踪（`-MMD -MP`），去掉了 m1n1 payload 目标。

> 为什么从 DTB 读 PSCI conduit：UTM/QEMU 默认让客户机跑在 EL1，`method = "hvc"`；
> 一旦开启 `virtualization=on`（客户机在 EL2），`method` 会变成 `"smc"`。
> 硬编码任一种都会在另一种配置下触发异常，所以运行时读 DTB 才稳。
>
> 为什么用虚拟定时器：QEMU 后端纯 TCG 时客户机独占 EL1，物理定时器（CNTP）能用；但
> 一旦开 HVF 加速，hypervisor 占着 EL2，EL1 访问 CNTP 会被陷入而抛异常。虚拟定时器
> （CNTV）在两种情况下都能从 EL1 访问，所以统一用它。
>
> 为什么 MMU 用 39 位 VA：T0SZ=T1SZ=25 时翻译从 level 1 起步。`virt` 的 RAM 从
> 0x40000000 开始、低于它的全是 MMIO，所以 L1 entry 0 整块设成 device、其余设成 normal
> 就正确分型了。PA 位宽（44/40）从 `ID_AA64MMFR0_EL1` 读出，TCG 和 Apple host 给的值不同。
>
> 为什么一张页表能同时当 TTBR0 和 TTBR1：39 位区只用 VA[38:0] 走表，高半区 VA 的低 39 位
> 恰好等于物理地址，所以 `g_l1` 同时挂在 TTBR0（恒等）和 TTBR1（高半区），物理 P 既能在
> P 访问、也能在 `kHighHalfBase|P` 访问。保留恒等别名是有意为之：device MMIO、boot DTB、
> 各 Sister 的物理栈都还能直接用，higher-half 切换因此不会把串口或 DTB 变成不可达而黑屏。

## 当前能力

- UTM/QEMU `virt` 直接加载 `build/Image`，串口进入 `Necessarius`。
- AArch64 入口清 BSS、建早期栈、安装异常向量，进入 C++ `kmain`。
- `ArtificialHeaven` 扫描 DTB，发现 PL011 串口、GICv2、`/psci`、`/memory`、`simple-framebuffer`。
- `Othinus` 通过 PSCI 关机/重启整台 UTM 虚拟机。
- `Aleister` 初始化 GICv2，`LastOrder` 以 100 Hz 跑系统心跳，定时器中断在后台累加 tick。
- `MisakaNetwork` **严格优先级（Level 0–5）+ 同级 round-robin** 调度 `Sister` 线程，
  由心跳驱动抢占，支持 `sleep`/`yield`/阻塞与定时唤醒。
- **SMP 多核**：`Othinus` 经 PSCI `CPU_ON` 启动 DTB 里发现的全部副核，每核自建 MMU/GICC/定时器
  并加入 `MisakaNetwork`，从一条共享 runqueue 真正并行调度 `Sister`；调度器、三个分配器、控制台、
  同步原语全部由自旋锁 `AntiSkill` 加锁（`smp` 看各核当前 Sister）。
- `Imprimatur` 计数信号量 + `Judgement` 互斥锁让 Sister 互相阻塞/唤醒、互斥访问临界区，
  `Judgement` 带 `Level Upper` 优先级继承防优先级反转；都建在 `AntiSkill` 调度锁上以保 SMP 正确。
- `RadioNoise` 有界阻塞消息队列（信号量 + 互斥锁组合）在 Sister 间按 FIFO 传数据。
- `Teleport` 打开 MMU 并把内核搬到 **higher-half**（TTBR1）+ 恒等别名（TTBR0），内核段
  按 **W^X** 细粒度保护（.text 只读可执行、数据 NX）。
- `GrimoireFS`（烤在镜像）、`Bookshelf`（堆上可写）、`Lateran`（磁盘上的 FAT16 只读）三个
  文件系统，`ls`/`cat` 统一查看。
- `IndexLibrorumProhibitorum` 早期 bump allocator、`TreeDiagram` 物理页分配器，均避开
  内核镜像、DTB、早期堆和保留区；`DarkMatter` 是能回收的内核堆（kmalloc/kfree）。
- **EL0 用户态**：用户程序在 EL0 跑，经 `svc` 系统调用与内核交互；越权访问会被隔离，
  内核报告 fault 后继续运行。
- `Underline` virtio-blk 驱动从虚拟磁盘读扇区（`disk` 命令），真正的持久存储 I/O；经 `Underground`
  PCIe 主桥支持 **virtio-mmio 和 virtio-pci 两种传输**（后者即 UTM GUI 的「VirtIO」盘）。
- ELF 加载器 `exec <名>` 从 FAT 盘加载 PIE 程序并在 EL0 执行,完整的"从磁盘运行程序"链路。
- `Esper` 进程抽象 + **并发多进程 + 地址空间隔离**:pid/退出码/状态,`coexec` 并发跑多个
  ELF,协作 `yield` + timer 抢占两种切换,每进程经 `PersonalReality` 拥有私有 TTBR0 地址空间。
- **fork/exec + 系统调用**:`fork`/`exec`/`wait` 进程控制 + `read`/`write`/`open`/`close` I/O;
  `Komoe` 是跑在 EL0 的迷你 shell(读命令 → fork+exec+wait 跑磁盘程序 / cat 文件 / exit 回内核)。
- **`Aiwass` 管道 + dup/dup2**:用户进程间的一对一字节流(`pipe`/`dup`/`dup2` 系统调用)。
  Komoe 现在懂 `cmd1 | cmd2` 和 `cmd < file`,`SYS_putc` 改成走 fd 1 自动尊重重定向。
  铁证:`INIT.ELF | WC.ELF` 通过管道传 30 字节、`WC.ELF < HELLO.TXT` 把磁盘文件灌给 stdin。
- **`Fortis931` kill 系统调用 + 后台运行**:Komoe 懂 `cmd &`(后台运行)和 `kill <pid>`(发 SIGTERM);
  内核 `fortis931_kill` 释放被杀进程的管道引用、置 exited、唤醒父 `wait`。MVP 不做 handler /
  sigreturn / 异步 Ctrl-C(后者需要 UART IRQ)。铁证:`LOOPER.ELF &` 后 `kill <pid>` 立刻停。
- **`AcademyCity` 用户态共享 libc**:每个 EL0 ELF 预链接 `academy_city.uo`,提供 `ac_printf`/
  `ac_puts`/`ac_strlen`/`ac_memcpy` 等;`INIT.ELF`/`WC.ELF`/`LOOPER.ELF` 已迁移用 `ac_printf` 写输出。
- **Linux ELF 兼容(静态 + 动态 + 完整运行时)**:`sniff_abi` 按 ELF 头区分 Index/Linux,Linux 程序走
  `linux_abi.cpp` 的 Linux AArch64 syscall 表 + `PersonalReality v2`(VMA + 按需分页)+ SysV 启动栈。
  静态(`HMUSL`)、动态(`HDYN` + `LD-MUSL.SO`)都直接跑;运行时支持**文件 I/O、信号(rt_sigaction/
  rt_sigreturn)、fork+waitpid、pthread(clone+futex+TLS)、文件 mmap**——真实 musl 程序能干活。Index 零回归。
- `Necessarius` 串口 shell 可用。

## 目录

```text
src/
  arch/aarch64/      启动代码、异常向量(含 IRQ / EL0 svc 路径)、misaka_switch、
                     user_switch(EL1↔EL0 桥)、CPU 辅助
  drivers/           ElectroMaster(UART)、ArtificialHeavenCanvas、Othinus(PSCI)、
                     Aleister(GICv2)、Underline(virtio-blk 磁盘)
  index/             内核核心、DTB、ImaginaryNumberDistrict、LastOrder(timer)、
                     MisakaNetwork(调度器)、Imprimatur(信号量)、Judgement(互斥锁)、
                     RadioNoise(消息队列)、Aiwass(EL0 间管道)、Fortis931(kill/信号)、
                     DarkMatter(内核堆)、usermode(EL0/syscall)、linux_abi(Linux ABI 派发/
                     启动栈/PT_INTERP)、Teleport(MMU/W^X/higher-half)、Esper(用户进程/进程表)、
                     PersonalReality(+v2: VMA/按需分页/自由地址空间)、GrimoireFS(只读FS)、
                     Bookshelf(可写FS)、Lateran(FAT16磁盘FS)、Necessarius
  user/              嵌入镜像、在 EL0 运行的用户程序（只能用 svc 系统调用）
userprog/            独立编译成 PIE ELF、从磁盘加载运行的用户程序（init / spin / wc / looper）
userprog/academy_city.{h,cpp}  用户态共享 libc(每个 Index ELF 静态链接)
userprog/hellolinux.S / auxvdump.S / hellomusl.c  Linux-ABI 测试程序
userprog/ld-musl-aarch64.so.1  vendored 的真 musl 动态链接器(动态链接测试用)
tools/mkfatfs.py     生成确定性 FAT16 测试镜像（可打包 init.elf）
linker.ld            裸机链接脚本
Makefile             交叉构建 + UTM/QEMU 运行目标
```

## 命名约定（本分支新增）

- `Othinus`: PSCI 电源控制 —— 魔神，能终结世界（关机）也能重塑世界（重启）。
- `Underline`: virtio-blk 块设备 —— 学园都市深层的数据底层；这里是持久磁盘存储（支持 virtio-mmio 与
  virtio-pci 两种传输）。
- `Underground`: PCIe 主桥 / ECAM 配置空间枚举器 —— 学园都市地下纵横连接一切设施的脉络网络；这里是
  CPU 经它按 bus:dev:fn 寻址、枚举、给设备分配 MMIO 的总线 fabric，virtio-blk/virtio-net 挂在上面。
- `MisakaMail`: virtio-net 网卡 + ARP/IPv4/ICMP 栈 —— 妹妹们在网络上互传信号的信箱；内核对外的网络口，
  能 `ping` 出去、也能应答别人的 ping。与 `MisakaNetwork`(调度器)、`RadioNoise`(内核信箱)同属妹妹主题。
- `Aiwass`: 管道 —— 阿莱斯特唯一的通讯对象是他的守护灵 Aiwass，私人、单向、不可见；
  这里是 Esper 之间的一对一字节流(4 KiB 环形 + 两侧引用计数)，配合 `pipe`/`dup`/`dup2`
  让 Komoe 能 `cmd1 | cmd2` 和 `cmd < file`。与 `RadioNoise`(Sister 间 FIFO 信箱)、
  `MisakaMail`(对外网络口)同属"传话"主题但分层不同:RadioNoise 在内核线程之间，Aiwass
  在用户进程之间，MisakaMail 跨设备。
- `Fortis931`: kill / 信号 —— Stiyl Magnus 的远隔火葬咒文,凭名字烧死目标;这里是 signals
  层。`kill(pid, sig)` 系统调用 + Komoe 的 `kill <pid>` 内建 + `cmd &` 后台。MVP 只做默认
  终止(无 handler 注册,无 sigreturn,无 SIGCHLD,无异步 Ctrl-C),SIGINT/SIGKILL/SIGTERM
  当前等价。Komoe 的 `& ... | kill` 流程是验证 demo,长期目标是把 `Aleister` 接上 UART
  让 Ctrl-C 真正异步打断 EL0。
- `AcademyCity`: 用户态共享 libc —— 学园都市是所有 Esper 的家;这里是每个 EL0 程序
  静态链接的迷你 libc:`ac_printf`/`ac_puts`/`ac_putn`/`ac_strlen`/`ac_memcpy`/`ac_parse_uint`
  等。预编译为 `academy_city.uo` 一份,所有用户 ELF 共用。v1 无 malloc(PIE 加载器不应用
  `.rela.dyn`,全局指针初始化会失败,所以严格栈 + 字面量)。命名取自学园都市本身。
- `Aleister`: GICv2 中断控制器 —— 窗のないビル里的操纵者，把每个信号路由到该去的地方。
- `LastOrder`: ARM 通用定时器 / 系统心跳 —— 妹妹网络的管理者（MISAKA 20001），维持统一节拍。
- `MisakaNetwork`: 抢占式调度器 —— 并行运转的妹妹网络，由 `LastOrder` 的节拍协调。
- `Sister`: 一个线程 —— 网络里的一个妹妹节点，有自己的栈和上下文。
- `Level`: 调度优先级 —— 学园都市的能力等级（Lv.0–Lv.5）；shell 在 Lv5，idle 在 Lv0。
- `Imprimatur`: 计数信号量 —— 教会对书籍的"准印许可"，和禁书目录正好是一对真实术语；
  线程要拿到许可才能继续，拿不到就阻塞排队。
- `Judgement`: 互斥锁 —— 风纪委员，维持秩序；一次只有一个 Sister 能进临界区。
- `AntiSkill`: 自旋锁 —— 学园都市武装的警备员，是 `Judgement`（风纪委员）的硬核对位：
  Judgement 是会经调度器睡眠的高层互斥锁，AntiSkill 是**不能睡的场合**（调度器、分配器、
  控制台内部）用的底层忙等锁（`ldaxr`/`stxr` + `wfe`/`sev`，带 irqsave）。SMP 全面加锁靠它。
- `Level Upper`: 优先级继承 —— 原作里临时拉高能力等级的装置；这里把持锁者临时提升到
  等待者的 Level，化解优先级反转。
- `RadioNoise`: 消息队列 —— Sisters 的实验代号「Radio Noise」；妹妹们在网络上互传信号，
  这里是 Sister 间的有界阻塞 FIFO 信箱。
- `DarkMatter`: 内核堆 —— 垣根帝督的「暗黑物质」，从无创造物质又能让它消散；这里是能
  分配又能回收的 kmalloc/kfree。
- `Teleport`: MMU / 地址翻译 + W^X + higher-half —— 白井黑子的"空间移动"，把对象从一组
  坐标搬到另一组（虚拟地址 → 物理地址，多级页表 = 11 次元演算）。
- `GrimoireFS`: 只读文件系统 —— Index 记诵的魔导书；这里是烤进镜像的一小撮 grimoire。
- `Bookshelf`: 可写文件系统 —— 随手取放书的书架；文件内容存在 DarkMatter 堆上。
- `Lateran`: 磁盘只读文件系统 —— 罗马正教的大档案库；这里是 Underline 磁盘上的 FAT16。
- `Esper`: 用户进程 —— 学园都市的能力者；EL0 程序的 pid / 状态 / 退出码 / EL0 上下文,
  多个可并发(`coexec`),协作 `yield` + timer 抢占两种调度。
- `PersonalReality`: 每进程地址空间 —— 能力者私有的"个人现实";私有 TTBR0 页表,进程间内存隔离;
  `personal_reality_fork` 急切整页拷贝出子进程的私有地址空间。
- `Komoe`: 跑在 EL0 的迷你 shell —— 月詠小萌,照管学生/进程的老师;内核 shell 是 `Necessarius`,
  `Komoe` 是用户态那一个,读命令后 `fork`+`exec`+`wait` 跑磁盘程序、或 `cat` 文件。
  （系统调用 `fork`/`exec`/`wait`/`read`/`write`/`open`/`close` 沿用标准名,和 `putc`/`exit` 一样不加 Index 名。）

> SMP（多核）没有引入新实体名（最小新名约定）：CPU 核只是逻辑 `cpu` 序号（0..n-1），
> 真正的 MPIDR 另存；`Othinus` 扩展出 PSCI `CPU_ON`（让新核诞生），`MisakaNetwork` 升级成
> 真正跨核并行的调度器，唯一新词是上面的自旋锁 `AntiSkill`。将来的核间中断（IPI）会叫
> `MentalOut`（食蜂操祈的心理掌握，一对多广播）。

其余沿用 Apple-Index（`Index` / `ArtificialHeaven` / `Testament` / `ElectroMaster` /
`ImaginaryNumberDistrict` / `IndexLibrorumProhibitorum` / `TreeDiagram` / `Necessarius`
/ `ImagineBreaker`）。

## 依赖

macOS 上用 Homebrew 的 LLVM/LLD 和 QEMU：

```sh
brew install llvm lld qemu
```

LLVM 不在 `PATH` 时构建传前缀（`lld` 通常在 `/opt/homebrew/opt/lld/bin/`，Makefile 会自动找）：

```sh
make LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

## 构建

```sh
make check-tools LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
make LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

产物：

- `build/Index.elf`: 带符号 ELF。
- `build/Image`: ARM64 Linux Image 风格裸镜像（UTM/QEMU 用这个）。

## 在 UTM 里运行（GUI）

UTM 用 `-kernel` 直接引导，DTB 由 `virt` 机器自动生成，无需自己提供。

1. 先打包出可导入的文件：

   ```sh
   make dist LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
   # 产出 build/utm/Index-Image 和 build/utm/BOOT-UTM.txt
   ```

2. UTM → **Create a New Virtual Machine**。
3. 选 **Emulate**（这条路用 **QEMU 后端**）。**不要选 Apple Virtualization**。
4. Operating System 选 **Linux**。
5. 勾选 **Boot from kernel image directly**：
   - **Linux kernel**：`build/utm/Index-Image`
   - **Initial ramdisk**：留空
   - **DTB**：留空（`virt` 机器自动生成）
6. Architecture：**ARM64 (aarch64)**；System：**QEMU `virt`**。
7. CPU：Default/Host；**CPU Cores：4**（对应 `-smp 4`，SMP 才有多核）；Memory：**1024 MB** 或更多。
8. （磁盘，给 `Underline`/`Lateran`/`komoe` 用）在 **Drives** 里 **New** 一块盘，**Interface 选 VirtIO**，
   Image 指向 `build/fat.img`（`make run-qemu-fat` 会生成）。内核经 `Underground` 走 virtio-pci 识别它，
   启动应出 `Underline (disk): 16384 sector(s)` + `Lateran (FAT16): 7 file(s)`。不挂盘也能开机，只是没磁盘 FS。
9. （可选，Apple Silicon）在 VM 设置里开 **Use Apple Hypervisor / HVF** 加速——仍是
   QEMU 后端，只是更快。
10. 保存后打开 UTM 的 **Serial** 窗口，即可看到 `Index>` 提示符；敲 `komoe` 进 EL0 shell、`smp` 看多核。

## 在命令行里跑（和 UTM 同一条路径）

QEMU 后端 + HVF 加速（对应 UTM QEMU 后端开了 Hypervisor，Apple Silicon 上原生快）：

```sh
make run-utm LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

QEMU 后端 + TCG 仿真（对应 UTM QEMU 后端不开 HVF，非 Apple Silicon 也能跑）：

```sh
make run-qemu LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

带一块 virtio-blk 裸测试磁盘跑（给 `Underline` 驱动和 `disk` 命令读裸扇区）：

```sh
make run-qemu-disk LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

带一块 FAT16 磁盘跑（给 `Lateran` 文件系统读真文件，`tools/mkfatfs.py` 自动造镜像）：

```sh
make run-qemu-fat LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

同样的 FAT 盘、但走 **virtio-pci**（精确复刻 UTM app GUI 的「VirtIO」盘，走 `Underground` PCIe 路径）：

```sh
make run-qemu-fat-pci LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/
```

打印等价 QEMU 命令：

```sh
make utm-args     # QEMU + HVF
make qemu-args    # QEMU + TCG
```

默认（HVF 加速）参数：

```text
qemu-system-aarch64 -M virt -accel hvf -cpu host -m 1024M -nographic -serial mon:stdio -kernel build/Image
```

退出 `-nographic` 串口：`Ctrl-A` 然后 `X`；或者在 shell 里敲 `poweroff`。

## 预期输出

```text
Index kernel (arm-Index / UTM port)
  codename : Index Librorum Prohibitorum
  target   : UTM ARM64 virt (QEMU) + DTB
  district : Imaginary Number District
  EL                 : 1
  Testament          : UTM / QEMU virt
  DTB                : 0x0000000048000000 + 0x0000000000100000
  ElectroMaster      : pl011 @ 0x0000000009000000
  Othinus (PSCI)     : ready via hvc
  Aleister (GICv2)   : GICD 0x0000000008000000 GICC 0x0000000008010000
  ImaginaryDistrict  : 1 range(s)
    [0] 0x0000000040000000 + 0x0000000040000000
  AIMDiffusionField  : 0 range(s)
  ArtificialHeaven   : not found
  Teleport (MMU)     : on, 39-bit VA, 2 GiB mapped (PA 44-bit), kernel W^X
  IndexLibrorum      : 0x0000000040100000 .. 0x0000000040300000 (2 MiB)
  TreeDiagram        : 261127 / 261127 pages available
  first page         : 0x0000000040100000
  LastOrder (timer)  : 100 Hz, CNTFRQ 62500000 Hz
  MisakaNetwork      : 5 sister(s), preemptive round-robin
  MisakaNetwork SMP  : 4 / 4 core(s) online
  DarkMatter (heap)  : 256 KiB arena, kmalloc/kfree ready
  Underline (disk)   : no block device attached
  Lateran (FAT16)    : no FAT16 volume
  GrimoireFS         : 4 grimoire(s) on the Bookshelf
Index is alive; entering Necessarius.
Necessarius shell ready. Type 'help'.
Index>
```

> `CNTFRQ` 在 TCG 下是 62.5 MHz，在 Apple Silicon 的 HVF 下是 24 MHz —— 数值不同但
> 100 Hz 心跳一致。敲两次 `uptime` 应看到 `beats` 在增长，说明定时器中断在后台触发。
>
> 验证 SMP 真并行（`make run-qemu SMP=4`）：启动横幅报 `4 / 4 core(s) online`。`smp` 列出
> 四个核，启动时 cpu0 跑 `main`、其余跑各自的 idle。连 `spawn` 三个 worker 再 `sisters`，
> 三个 worker **同时**在**不同核**上 `run@`，`work` 几乎相等地并行增长（若是单核时分，三者
> 只能共享一核、总量约 1/3）；再 `smp` 可见 Sister 在核间迁移：
>
> ```text
> Index> sisters
> MisakaNetwork: 8 sister(s), 4 core(s)
>   [0] main   Lv5 state=run@1 runs=738 work=0    <= here
>   [5] worker Lv2 state=run@2 runs=110 work=7870
>   [6] worker Lv2 state=run@3 runs=117 work=7820
>   [7] worker Lv2 state=run@0 runs=112 work=7840
> ```
>
> 验证 SMP 下并发原语仍正确（`SMP=4` 真并行竞争）：`judgement` / `radio` / `prodcons` 跑几秒后
> `sisters` —— `Judgement shared counter == judge-A.work + judge-B.work`（互斥无丢更新）、
> `RadioNoise errors == 0`（FIFO 正确）、producer.work == consumer.work（信号量直接交接不丢）。
> 这些原语都建在 `AntiSkill` 调度锁上，跨核竞争依然成立。`SMP=1` 单核路径与所有经典 demo
> 零回归（EL0 `user`/`userfault` 隔离照常）。
>
> 验证抢占与阻塞：`spawn` 一个 busy worker、`sleeper` 一个睡眠 Sister，过几秒再 `sisters`。
> busy worker 的 `runs`/`work` 飞涨；sleeper 大部分时间是 `sleep`，每秒才被唤醒一次，
> 所以 `runs`/`work` 约等于经过的秒数——说明它真的在阻塞，没烧 CPU。例如约 5 秒后：
>
> ```text
> Index> sisters
> MisakaNetwork: 4 sister(s)
>   [0] main    state=ready runs=204 work=0    <= current
>   [1] idle    state=ready runs=203 work=0
>   [2] worker  state=ready runs=165 work=3660
>   [3] sleeper state=sleep runs=5   work=5
> ```
>
> 验证信号量：`prodcons` spawn 一对生产者/消费者。consumer 大部分时间是 `block`
> （在 `Imprimatur` 上等许可），producer 每秒 `post` 一次，两者的 `work` 始终相等
> （direct hand-off，不丢不重）：
>
> ```text
>   [2] producer Lv2 state=sleep runs=4 work=3
>   [3] consumer Lv2 state=block runs=4 work=3
> ```
>
> 验证优先级：`levels` spawn 一个 Lv4 和一个 Lv2 busy worker。严格优先级下 Lv4 把核占满，
> Lv2 几乎跑不到（`runs`/`work` 停在 0），shell 仍在 Lv5 保持响应：
>
> ```text
>   [2] lv4-high Lv4 state=ready runs=336 work=7163
>   [3] lv2-low  Lv2 state=ready runs=0   work=0
> ```
>
> 验证互斥锁：`judgement` spawn 两个 Sister 抢一把 `Judgement` 锁去改共享计数器。结束后
> 共享计数器恒等于两者 `work` 之和（没有丢更新），说明临界区互斥成立：
>
> ```text
>   [2] judge-A Lv2 state=sleep runs=42 work=42
>   [3] judge-B Lv2 state=sleep runs=42 work=42
>   Judgement shared counter = 84 (should equal judge-A.work + judge-B.work)
> ```
>
> 验证优先级继承：`invert` 摆出经典优先级反转 —— pi-low(Lv1) 持锁、pi-mid(Lv3) 当 hog、
> pi-high(Lv4) 等锁。只有靠 `Level Upper` 把 pi-low 临时提到 Lv4，它才能越过 hog 跑完临界区
> 释放锁，pi-high 才有进展。几秒后 `sisters` 能看到 `pi-low Lv1->4`（临时提升）、boost 计数
> 增长、pi-high `work` > 0（没有继承的话会卡在 0）：
>
> ```text
>   [2] pi-low  Lv1->4 state=ready runs=343 work=16
>   [3] pi-mid  Lv3    state=ready runs=171 work=8249
>   [4] pi-high Lv4    state=block runs=34  work=16
>   Level Upper boosts = 17 (priority inheritance; pi-high progresses only via these)
> ```
>
> 验证消息队列：`radio` spawn 一个发送者和一个更慢的接收者，经 `RadioNoise` 信箱传递
> 递增序列。`errors` 恒为 0 说明 FIFO 顺序正确；`sent - recv` 恒等于缓冲容量（4），且
> rn-send 卡在 `block`，说明发送者被"满队列"正确阻塞：
>
> ```text
>   [2] rn-send Lv2 state=block runs=37 work=0
>   [3] rn-recv Lv2 state=sleep runs=17 work=0
>   RadioNoise: sent=21 recv=17 errors=0 (FIFO mailbox; errors must stay 0)
> ```
>
> 验证内核堆：`heap` 连续 alloc 三块、释放中间那块、再 alloc 同尺寸。第二次 alloc 会
> **重用刚释放的地址**（first-fit），全部释放后空闲块**合并回 1 块**：
>
> ```text
> alloc a=0x..c440 b=0x..c840 c=0x..d020   used=3520 blocks=4 free_blocks=1
> freed b -> used=1520 free_blocks=2
> alloc d=0x..c840 (reused b's slot)        <- d 和 b 同地址，复用成功
> freed all -> used=0 free_blocks=1 (fully coalesced)
> ```
>
> 验证可写文件系统：`write note hello bookshelf` 建文件 → `ls` 看到 `note [rw] 15 bytes`
> → `cat note` 打印内容 → 再 `write note ...` 覆盖 → `rm note` 删除后 `ls` 里消失。文件内容
> 在 `DarkMatter` 堆上，覆盖会释放旧内容、删除会还堆：
>
> ```text
> Index> write note hello bookshelf
> wrote note
> Index> ls
>   motd [ro]
>   ...
>   note [rw] 15 bytes
> Index> cat note
> hello bookshelf
> ```
>
> 验证块设备：`make run-qemu-disk` 会自动造一块带已知内容的测试盘并挂上。进去敲 `disk`
> 读扇区 0,dump 出的字节正是盘里的内容,证明 virtio-blk 的 DMA 读路径通了:
>
> ```text
>   Underline (disk)   : 2049 sector(s), virtio-blk ready
> Index> disk
> sector 0 first 64 bytes:
> INDEX-UNDERLINE-SECTOR0! virtio-blk works.......................
> ```
>
> `load greeting 0` 把扇区 0 读进一个 Bookshelf 文件,`cat greeting` 就能看到——
> 完整跑通 **virtio-blk 设备 → DMA → 内核缓冲 → DarkMatter 堆 → Bookshelf 文件系统 → shell**
> 这条存储链路。
>
> 验证磁盘文件系统:`make run-qemu-fat` 挂一块 FAT16 盘。启动报 "Lateran (FAT16): 3 file(s)",
> `ls` 里出现 `[fat]` 文件,`cat HELLO.TXT` 直接读出磁盘上 FAT 格式里的真文件:
>
> ```text
>   Lateran (FAT16)    : 3 file(s) on the disk
> Index> ls
>   ...
>   HELLO.TXT [fat] 35 bytes
>   INDEX.TXT [fat] 49 bytes
>   README.TXT [fat] 55 bytes
> Index> cat HELLO.TXT
> Hello from the Lateran FAT16 disk!
> ```
>
> 验证 virtio-blk over PCIe（UTM GUI 的「VirtIO」盘走的就是这条）:`make run-qemu-fat-pci`（用
> `-device virtio-blk-pci` 而非 mmio）。`Underground` 从 ECAM 枚举到 PCI 上的 virtio-blk、分配 BAR、按
> modern 接口建队列,banner 与 mmio 路径**完全一样**(同样 16384 扇区、7 个文件),`komoe`/`cat`/`exec`
> 照常——证明 PCI 传输通了:
>
> ```text
>   Underline (disk)   : 16384 sector(s), virtio-blk ready
>   Lateran (FAT16)    : 7 file(s) on the disk
> Index> komoe
> komoe$ INIT.ELF
> [komoe] pid 2 exited code 0
> komoe$ cat HELLO.TXT
> Hello from the Lateran FAT16 disk!
> ```
>
> 验证从磁盘加载并执行 ELF:同样 `make run-qemu-fat`(FAT 盘里带了 `INIT.ELF`)。`exec INIT.ELF`
> 把这个 PIE 程序从磁盘读出、解析、加载到 EL0 区域并运行,它经系统调用打印自己的输出:
>
> ```text
> Index> exec INIT.ELF
> exec INIT.ELF
> Dropping to EL0 (user mode)...
> init: ELF from disk!
> pid=1
> Back in kernel (EL1); user ran at EL0, made 29 syscall(s).
> ```
>
> 验证并发多进程:`coexec INIT.ELF INIT.ELF` 把两份 `INIT.ELF` 各加载到自己的 arena 槽并发跑。
> 它们循环打印 pid+计数器、每轮 `yield`,输出**交替**出现 —— 两个独立进程在协作式切换:
>
> ```text
> Index> coexec INIT.ELF INIT.ELF
> Scheduling Esper(s) at EL0...
> pid=2 i=0
> pid=3 i=0
> pid=2 i=1
> pid=3 i=1
> pid=2 i=2
> pid=3 i=2
> All Espers finished; back in kernel (EL1).
> ```
>
> 验证地址空间隔离:`coexec INIT.ELF INIT.ELF` 两个进程都加载在**同一个虚拟地址 0x10000**,
> 但内核报出**不同的物理页**——它们是各自独立、互相隔离的地址空间(私有 TTBR0):
>
> ```text
> Index> coexec INIT.ELF INIT.ELF
>   loaded INIT.ELF at VA 0x0000000000010000 -> PA 0x00000000400f1000 (private)
>   loaded INIT.ELF at VA 0x0000000000010000 -> PA 0x00000000400ff000 (private)
> ```
>
> 验证抢占式调度:`coexec SPIN.ELF SPIN.ELF` 跑两个**从不 yield** 的纯计算程序。它们的输出
> 仍然交替(`1212...`)—— 既然没人主动让出,唯一能切换它们的就是 timer 中断在抢占 EL0:
>
> ```text
> Index> coexec SPIN.ELF SPIN.ELF
> Scheduling Esper(s) at EL0...
> 12122121211
> 2
> All Espers finished; back in kernel (EL1).
> ```
>
> 验证进程抽象:连续跑几个程序再 `ps`。每次 `exec`/`user` 分配一个新 pid,正常退出记 `exited`
> 带退出码,越权的记 `faulted`(某个 Esper fault 只终止它自己,其余继续):
>
> ```text
> Index> ps
> Espers (user processes):
>   [pid 1] INIT.ELF state=exited code=0
>   [pid 2] user state=exited code=0
>   [pid 3] userfault state=faulted
> ```
>
> 验证 fork/wait/exit(`make run-qemu-fat`,确定性):`exec FORKDEMO.ELF` —— 父 `fork` 出子进程、
> `wait` 收到子的 pid 和退出码(`fork` 返回值父=子pid、子=0;退出码经延迟回写交给父):
>
> ```text
> Index> exec FORKDEMO.ELF
> forkdemo: before fork
>   parent: forked child pid=2
>   child : pid=2, exiting with code 7
>   parent: reaped pid=2 code=7
> ```
>
> 验证 Komoe(EL0 shell,整套系统调用):`komoe` 进入后,敲程序名 → `fork`+`exec`+`wait` 跑它;
> `cat <文件>` → `open`/`read`/`write`/`close` 读盘上真文件;`exit` → 回到内核 `Index>`:
>
> ```text
> Index> komoe
> Komoe: an EL0 shell. commands: <prog> | cat <file> | exit
> komoe$ INIT.ELF
> pid=2 i=0
> pid=2 i=1
> pid=2 i=2
> [komoe] pid 2 exited code 0
> komoe$ cat HELLO.TXT
> Hello from the Lateran FAT16 disk!
> komoe$ exit
> Komoe: bye.
> ```
>
> 验证 EL0 用户态：`user` 下到 EL0 跑用户程序。它经 `svc` 系统调用打印 `Hello from EL0!`
> 和 `pid=1`，然后退出；内核报出"用户跑在 EL0、做了 N 次系统调用"（EL 从陷入时的 SPSR.M
> 推出，是硬证据）：
>
> ```text
> Index> user
> Dropping to EL0 (user mode)...
> Hello from EL0!
> pid=1
> Back in kernel (EL1); user ran at EL0, made 24 syscall(s).
> ```
>
> 验证 EL0 隔离：`userfault` 让用户程序去写内核地址。EL0 无权访问内核页，立即 fault；
> 内核报告后**终止用户、自己继续活着**（之后 `status`、再 `user` 都正常）。注意 ESR 的
> EC=0x24（来自**更低** EL 的 data abort），区别于 `wxtest` 的 EC=0x25（同级）：
>
> ```text
> [EL0 FAULT] ESR 0x000000009200004f ELR 0xffffff804008b144 FAR 0xffffff8040100000
>   user terminated; kernel survives.
> ```
>
> 验证 W^X / higher-half：`wxtest` 往只读 `.text` 写一下，立刻触发权限 fault 并停机。
> `ELR`/`FAR` 都是 `0xffffff80…`，既证明内核在跑高半区、也证明 `.text` 是只读：
>
> ```text
> [EXCEPTION] vector 4
>   ESR  0x000000009600004f   (EC=0x25 data abort, DFSC=0x0f 权限 fault, WnR=1 写)
>   ELR  0xffffff8040087ccc   (出错指令在高半区)
>   FAR  0xffffff8040080000   (写到的 .text 地址，高半区别名)
> ```

## Necessarius shell

```text
help     显示命令列表
status   显示 EL、ElectroMaster、Othinus(PSCI)、ArtificialHeaven canvas
mem      显示内存范围和早期堆
dtb      显示 device tree 地址和大小
heaven   重画 ArtificialHeaven canvas
alloc    从 IndexLibrorumProhibitorum 分配一页
tree     显示 TreeDiagram 物理页区间
page     从 TreeDiagram 分配一页物理页
uptime   显示 LastOrder 定时器 tick 数和已运行秒数，别名 ticks
sisters  显示 MisakaNetwork 里的线程（含 Level、所在核 run@N）及其计数
smp      显示在线的核及每个核当前运行的 Sister
spawn    spawn 一个 Lv2 busy worker Sister
sleeper  spawn 一个每隔 ~1 秒醒一次的睡眠 Sister
prodcons spawn 一对生产者/消费者，共享一个 Imprimatur 信号量
levels   spawn 一个 Lv4 + 一个 Lv2 worker，演示优先级调度
judgement spawn 两个 Sister 抢一把 Judgement 互斥锁，别名 judge
invert   优先级反转演示，靠 Level Upper 继承化解，别名 pi
radio    发送者/接收者经 RadioNoise 信箱传 FIFO 消息，别名 mailbox
heap     演练 DarkMatter 堆（alloc/free/复用/合并）
user     下到 EL0 跑一个内嵌用户程序（经 svc 系统调用打印）
userfault EL0 程序故意越权访问内核内存，演示隔离（内核存活）
exec <名> 从 Lateran 磁盘加载一个 PIE ELF 并在 EL0 执行
coexec <a> <b> 并发(协作式)运行多个 ELF 程序
komoe    进入 Komoe —— 跑在 EL0 的迷你 shell(fork/exec/wait + read/write/open/close)
ps       列出 Esper 用户进程（pid / 状态 / 退出码）
ls       列出文件（GrimoireFS [ro] + Bookshelf [rw]）
cat <名> 打印文件（先查 Bookshelf 再查 GrimoireFS，如 cat motd）
write <名> <文本>  在 Bookshelf 创建/覆盖文件（堆上分配）
rm <名>  删除 Bookshelf 文件（释放堆）
disk [s] 从 Underline 虚拟磁盘读扇区 s（默认 0）并 dump
load <名> <s>  把磁盘扇区 s 读进 Bookshelf 文件 <名>
wxtest   往 .text 写以证明 W^X（触发 fault 后停机）
halt     进入 idle loop
poweroff 通过 Othinus 关机（PSCI SYSTEM_OFF），别名 off
reboot   通过 Othinus 重启（PSCI SYSTEM_RESET），别名 reset
imagine  触发 ImagineBreaker
```

## 下一步

- 把 higher-half 推到完全去掉恒等别名：给 device/DTB 单独建高半区映射，再撤掉 TTBR0。
- 把优先级继承做完整：处理嵌套锁 / 多等待者时按最高等待者的 Level 重算（当前单锁场景
  下解锁直接恢复到 base Level）。
- 在 `RadioNoise` 之上搭更高层抽象：读写锁、带超时的 recv、多生产者/多消费者压力测试。
- 给 `Lateran` 加子目录、FAT12/32、以及写支持（写回扇区 + 改 FAT/目录项），让 Komoe 的 `>` 重定向能用。
- ~~`fork`/`exec`、更多系统调用(read/write/open)~~ —— **已完成**(`Komoe` EL0 shell)。续做:
  按需调页/COW(现在 fork 是急切整页拷贝)、参数/环境传给 `exec`(argv/envp)、把 Esper 与 Sister 调度统一。
- ~~管道 + dup/dup2(`cmd1 | cmd2`)~~ —— **已完成**(`Aiwass`)。续做:多级流水线(`a | b | c`)、
  非阻塞 fd、`select`/`poll`。
- ~~kill + Komoe 的 `&` 后台~~ —— **已完成**(`Fortis931` MVP)。续做:UART IRQ 接 `Aleister` 让
  Ctrl-C 真正异步打断 EL0;`sigaction`/handler/sigreturn;信号掩码;SIGCHLD;作业控制(`fg`/`bg`)。
- ~~AcademyCity 用户态共享 libc(printf/strings)~~ —— **已完成**。续做:`ac_malloc` 需要 PIE 加载器
  应用 `.rela.dyn`(或限定无指针初始化的全局)、`ls`/`cp`/`mv`/`rm`/`grep` 等用户态 coreutils、
  argv/envp 传给 `exec` 后才有意义的命令行参数。
- 把内核栈也搬到高半区(现在内核栈在低地址、靠每进程 L1 继承内核映射才可用)。
- ~~让 UTM GUI 的 VirtIO 盘工作（PCIe）~~ —— **已完成**(`Underground` 主桥 + virtio-pci)。续做:
  在 `Underground` 上挂更多 PCI 设备(virtio-net 网卡、virtio-gpu)、写支持、多队列、MSI-X 中断(去掉轮询)。
- ~~SMP 多核（PSCI CPU_ON 启动其它核 + 调度器/分配器加锁）~~ —— **已完成**。续做：
  核间中断 `MentalOut`（GIC SGI）+ TLB shootdown；让 `Esper`（EL0 用户进程）能在任意核上跑
  （需跨核地址空间协调）；把 `Esper` 与 `Sister` 调度统一；负载均衡 / 核亲和。

## 参考

- UTM 文档: https://docs.getutm.app/
- QEMU `virt` 机器: https://www.qemu.org/docs/master/system/arm/virt.html
- Arm PSCI 规范: https://developer.arm.com/documentation/den0022/
- Arm GICv2 架构规范: https://developer.arm.com/documentation/ihi0048/
- Arm Generic Timer (AArch64): https://developer.arm.com/documentation/102379/
- Linux ARM64 boot protocol: https://www.kernel.org/doc/html/latest/arch/arm64/booting.html
