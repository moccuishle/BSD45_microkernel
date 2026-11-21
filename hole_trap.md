# BSD45 Windows 构建踩坑记录（内核尺寸 / 安装失败）

> 从 `ai_install_gccenv.md` 的「踩坑记录」中拆分出来的两个构建期问题。
> 编号沿用原文档（坑 10、坑 11），方便和旧笔记/提交信息对照。
> 环境安装类的问题（pacman、镜像源、签名、mingw32 安装等）仍在 `ai_install_gccenv.md`。

---

## 坑 10：Windows 编出的内核比 Ubuntu 大 4 倍，install 直接卡死

- **现象**：同一份源码，Ubuntu 编译的 `bsd45.elf` 只有 123,544 字节，Windows/MSYS2 编出来 572,328 字节；用这个内核打出的 ISO 在 VMware 里敲 `install` 后**完全没有输出**，卡死。
- **定位过程**（三步就能锁定）：
  1. `objdump -h` 对比两个 ELF：Windows 版 `.data` = 411 KB、`.bss` = 9 KB；Ubuntu 版 `.data` = 10 KB、`.bss` = 419 KB —— **同一批数据换了段**。
  2. 用 `nm` 统计符号所在段：**349 个 static 变量全在 `.data`，95 个全局变量全在 `.bss`**，规律非常干净。
  3. `objdump -h build/obj/sys/stubs.o` 看到 `static char heap[256KB]` 落在 **`.data$heap`** 段，且 `stubs.o` 文件自己就 266 KB —— 零初始化数据真的被写进了文件。
- **根因**：`i686-w64-mingw32-gcc` 是 **PE/COFF 目标**。开了 `-fdata-sections` 后，所有 *static* 的零初始化变量被放进 `.data$<name>` 段并**真实携带全零内容**；Linux ELF 工具链则放进 `.bss`（SHT_NOBITS，不占文件）。所以**跟 32/64 位无关，装 i686 工具链解决不了**。
- **为什么会导致 install 卡死**：bootloader 按 manifest 里的 `kernelSize`（就是 ELF 文件大小）把内核整块读进内存。572 KB 时加载失败 → ISO 挂载失败 → 敲 install 毫无反应。
- **对策（已内置进构建流程）**：`tools/pe_bss_fix.py` 在链接前扫一遍所有 `.o`，把满足「段名 `.data$*` + 无重定位 + 内容全零」的段改写成 BSS 段：`VirtualSize` 保留原大小、`SizeOfRawData` 置 0、标志改为 `CNT_UNINITIALIZED_DATA`，并把段名改成短名 `.bss$<序号>`。这是纯表示层改写（全零且无重定位的段按定义就是未初始化数据），语义完全等价。
  - `build_kernel.ps1` 链接前自动调用；`kernel/Makefile.win` 也加了同一步。
  - 效果：`.data` 411 KB → 10 KB，ELF 572 KB → 146 KB，`readelf -S` 里 `.bss` 变成 `NOBITS`。
- **两个必须注意的细节**（踩过才知道）：
  1. **只改段属性不够**。PE 的输出节**不能含空洞**，如果段名仍是 `.data$xxx`，链接器照样把它并进 `.data` 并把零写进磁盘。必须**改名成 `.bss$xxx`** 才会并入 `.bss` 输出节。
  2. **段名不能原地改 string table**：`.data$`（6 字节）比 `.bss$`（5 字节）长，原地替换会移位，破坏所有 `/<offset>` 引用和符号名。所以要直接写进段头那 8 字节内联字段（`.bss$<序号>` 最多 8 字符）。
- **根治方向**（本机编译器跑不起来，未实测）：去掉 `-fdata-sections` 让 GCC 回到默认的 `.bss`，或改用真正的 ELF 交叉工具链（`i686-elf-gcc`）；MSYS2 自带的 ld 只支持 `i386pe` 一种 emulation，`-m elf_i386` 走不通。

---

## 坑 11：install 走到一半报 `FAILED (see messages above)`，实际是 embed.o 陈旧

- **现象**：内核起来、`install` 也跑起来了，日志一路正常：
  ```
  install: writing stage2 (0 bytes) at LBA 1
  install: writing /bsd45.elf to disk ...
  install: kernel written (146336 bytes, 286 sectors at LBA 8192)
  install: bad stage1_hdd size 4
  install: FAILED (see messages above)
  ```
- **定位**：`stage2 (0 bytes)` 就是信号。查 `build/embed.o` 只有 1652 字节，而 `build/embed.c` 里有 hello(10320)+init(10508)+stage1_hdd(512)+stage2(196) 共 21 KB 数据 —— **embed.o 是旧文件**（02:03）而 embed.c 是 04:23 重新生成的，`.o` 从没跟着更新，里面的 `stage2_bin` 只有 4 字节。
- **后果**：`install_to_disk()` 第 4 步校验 `stage1_hdd_bin_len != SECSZ` 直接返回 -1，MBR 都没写，磁盘装不起来。
- **对策**：
  1. 正常环境：重跑一次完整 `build_kernel.ps1` 即可（它会无条件重编 embed.o）。**分步/带 `-Only` 编译时最容易漏掉 embed.o**，检查 `ls -l build/embed.o` 应约 22 KB。
  2. 编译器跑不起来时（cc1 被拦但 `as.exe` 可用）：`python tools/embedc_to_s.py build/embed.c build/embed_new.s && as --32 -o build/embed.o build/embed_new.s`，把 `embed.c` 的数组重新汇编成目标文件，不需要编译器。
- **验证**：链接后核对 `_stage1_hdd_bin_len` 应为 512、`_stage2_bin_len` 应为 196（`readelf -s` + 读 .data 段即可）。

---

## 坑 12：进程表搬进 VA 窗口后，VMware 在 exec /bin/sh 后 TRAP 9 停机

- **现象**（VMware Workstation 17，512 MB，`NPROC=256`，`-Define VM_PROCTEST_AUTOTEST,VM_FORCE_COM1`）：
  ```
  execve /bin/sh: entry 8048000 esp bfbfffc0 pd 3c3000 argc 3
  *** kernel TRAP 9 (coprocessor overrun) err=0
  eip=fe00268f cs=8 ... ss=1f ... cr2=bfbffe64
  system halted.
  ```
  同一 ISO 在 QEMU 下能过，VMware 下必停；停在 `resume_user` 的
  `mov 0x1c(%esi),%edx`（`eip=fe00268f`）。
- **根因**：`resume_user` 先把 DS/ES/FS/GS 换成用户数据选择子（LDT，limit =
  `btoc(UPT_MIN_ADDRESS)` → `0xFDC00000`），**之后**才从 `struct sysregs`
  读 `edx/ebx/ecx/ebp`。S407 把 `procs[]` 搬到 `0xFE340000` 的窗口后，
  `&p->uk_regs` 已在 `0xFDC00000` 之上；ring0 里 DS 仍受该 limit 约束，
  VMware 严格执行 → 读 `uk_regs` 直接 #GP（4.4BSD 的向量表报成 TRAP 9）。
  QEMU 对该 limit 的处理更宽松，所以只在 VMware 暴露。
- **修复**（`kernel/arch/i386/usermode_asm.s`）：把 `edx/ebx/ecx/ebp` 全部
  在切换 DS **之前**读完；用 `%di`（而不是 `%dx`）作选择子暂存，避免破坏
  用户 edx；最后从栈上恢复 `edi/esi/eax`，再切 CR3 / iret。不要靠把 LDT
  limit 改成 4 GB 绕过，先按“换段前读完”修，语义最干净。
- **验证**：VMware 512 MB，`proctest` 输出
  `phase A 200/200`、`phase B live=200/200`、`reaped 200/200`、`PASS`；
  普通构建也能正常到 `bsd45>`。
- **顺带修的构建坑**：刚链接出的 32 位 `genassym.exe` 立即执行时，Windows
  偶尔返回 exit 0 但输出为空（杀软/文件锁窗口），导致 `assym.s` 0 字节。
  `build_kernel.ps1` 已改为最多 4 次、间隔 300 ms 重试。
- **同一任务里一并解决的内存探测坑**（S408）：VMware 给了 512 MB，内核却只
  打印 `bios 640K+64512K`、`Memory: maxmem 4000000`（只认 64 MB）。原因是
  `machdep.c` 只用 CMOS `0x17/0x18`（16 位 KB）探测扩展内存，超过 64 MB 会
  饱和；VMware 的 CMOS `0x34/0x35` 返回 `0x01d0`，也不是内存大小。
  修法：**stage1 / stage1_hdd 在实模式下跑 `INT 15h AX=E801`**（AX = 1MB~16MB
  的 KB 数、BX = 16MB 以上的 64KB 块数），把 `MEM1` + 一个字写到 `0x9000`
  （ISO 路径 stage2 源在 0x7E00、硬盘路径 stage2 在 0x8000，0x9000 两边都空），
  内核从 `KERNBASE+0x9000` 读，E801 不可用时回落到 CMOS。
  验证：`bios 640K+64512K e801 15360K. maxmem 1fedf000` → 511 MB；
  proctest 新增 phase C（200×384KB=75MB 同时占用）PASS。


## 坑 13：给内核加新 VA 窗口后，`TRAP 12 / cr2=0` 就死在 `pmap_enter` 里

**现象**：把 COW 引用计数表从 `0xFE900000` 搬到 `0xFEC00000`（locore 12MB
恒等映射窗口之外），内核在 `page_ref_init()` 阶段直接停机：

```
*** kernel TRAP 12 (stack fault) err=0
eip=fe112f8a ... cr2=0
```

**根因**：`pmap_enter()` 不会按需创建页表。`pmap_pte()` 对缺失的 PDE 返回 0，
下一行 `pmap_pte_pa(pte)` 就解引用 0，于是"卡在地址 0 上访存"，表现成 TRAP 12
而不是 TRAP 14，`cr2=0` 是唯一的线索（实际是 `*(int *)0`）。locore 只建了
`0x3f8..0x3fa` 三个 PDE。

**修法**：用 `kern_window_install()` 显式建表（分配页表页、清零、写
`pmd = pa | PG_V | PG_RW`），并让 `kern_table_alloc()` 在跨越 4MB 边界时自动
调用它；同时把 `pmap_enter()` 的 `pte == 0` 改成 `panic("pmap_enter: no page
table for va ...")`，以后再遇到一眼就能看出来。

**同一类坑**：内核栈是按需映射的，所以高 4MB 窗口的 PDE 要等到对应槽位第一次
使用时才出现——而用户页目录在开机时就拷走了内核 PDE。必须用
`kthread_prime_stacks()` 在启动时把每个窗口的页表先建好。

## 坑 14：VMware 图形界面会把 vmx 的光驱改回 `cdrom-raw / auto detect`

**现象**：`vmrun start ... nogui` 返回 0，串口 0 字节，`vmware.log` 里写着
`Cannot connect the virtual device ide1:0 because no corresponding device is
available on the host` → `No operating system was found`。

**根因**：只要在 VMware Workstation 里打开过这个虚拟机的标签页，关机时它会把

```
ide1:0.deviceType = "cdrom-image"
ide1:0.fileName = "..\out\BSD45-ELF-x86.iso"
```

改写成

```
ide1:0.deviceType = "cdrom-raw"
ide1:0.fileName = "auto detect"
ide1:0.autodetect = "TRUE"
```

之后 headless 启动就没有光驱可引导。

**修法**：跑自动测试前 `git checkout -- bsd45/vms/BSD45-proctest.vmx` 恢复；
或者用一份 GUI 没打开过的 vmx 副本做自动化。另外它还会把
`out\BSD45-ELF-x86.iso` 占住，导致 `build_iso.ps1` 追加内核失败——脚本现在会
直接报错（见 `build_iso.ps1` 的 try/catch 与长度校验）。
