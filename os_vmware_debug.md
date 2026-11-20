# 在 VMware 中调试 BSD45 内核：可复现的方法

> 目标读者：接手本仓库的其他模型 / 工程师。
> 本文只写**在本机实测过**的做法，以及**实测失败**的做法（附证据），
> 避免下一个人重复踩坑。
> 环境：Windows 10/11 + VMware Workstation 17（`C:\Program Files (x86)\VMware\VMware Workstation\`）。

---

## 0. 结论速览（先看这张表）

| 通道 | 结论 | 怎么用 |
|---|---|---|
| **快照 `.vmem` + `klog_buf`** | ✅ 唯一稳定可靠 | `vmrun snapshot` → `vm_screen.py --klog` |
| 快照 `.vmem` 的 `0xB8000` | ❌ 恒为 0 | VGA 是设备内存，不在 RAM dump 里 |
| 串口 `fileType = "file"` | ❌ 抓不到（0 字节） | 别浪费时间 |
| 串口 `fileType = "network"` | ❌ VM 不主动连 | 同上 |
| `vmrun captureScreen` | ❌ 需要 VMware Tools | 报"不允许匿名客户机操作" |
| VMware `debugStub`（gdb） | ❌ Workstation 17 无此功能 | 端口直接 refused |
| 键盘交互 | ⚠️ 可用，但必须关掉 vUSB | 见 §2 vmx 配置 |
| QEMU `-serial file:` / `-debugcon` | ✅ 仅用于对照 | 串口全量日志用它 |

**核心做法**：VM 跑完后用 `vmrun snapshot` 拍快照，主机侧读 *普通 RAM*：

- 内核把控制台文本另存一份到 `klog_buf`（8KB，`pccon.c`），主机读它；
- 需要看内部状态时直接读内核全局量（`phys = sym - 0xFE000000`）。

**不要**去读快照里的 `0xB8000`：VMware 把 `0xA0000-0xBFFFF` 交给虚拟 SVGA，
`.vmem` 里这段恒为 0（QEMU 的 `screendump` 才是真显存）。

---

## 1. 建虚拟机（vmx 最小可用配置）

仓库里已有模板：`bsd45/vms/BSD45-proctest.vmx`（El Torito 从 ISO 启动）。
关键字段与**为什么**：

```ini
config.version = "8"
virtualHW.version = "12"
guestOS = "other"
memsize = "512"              # 动态内存上界需要 >64MB 才有意义
numvcpus = "1"               # 本内核是单 CPU 协作核，别开多核

# 从 ISO 启动（El Torito no-emulation，内核是 ELF32 不是 PE）
ide1:0.present = "TRUE"
ide1:0.deviceType = "cdrom-image"
ide1:0.fileName = "..\out\BSD45-ELF-x86.iso"
ide1:0.startConnected = "TRUE"
bios.bootOrder = "cdrom"

# 本内核只轮询 8042 键盘；VMware 17 默认给虚拟 USB 键盘，guest 永远收不到
keyboard.vusb.enable = "FALSE"
mouse.vusb.enable = "FALSE"
keyboard.allowBothIRQs = "FALSE"

usb.present = "FALSE"
ethernet0.present = "FALSE"
sound.present = "FALSE"
floppy0.present = "FALSE"
ide0:0.present = "FALSE"     # 纯 ISO 启动，不挂硬盘
```

> 不要开 `serial0.*`：见 §5，文件型串口抓不到内容，只会浪费时间。

新建一台 VM 的最快方式：复制现有 `.vmx`，改 `displayName` 和
`ide1:0.fileName`，然后删掉同目录的 `*.lck`、`*.vmem`、`*.vmsn`、
`nvram` 让 VMware 重新生成。

---

## 2. 构建 → 启动 → 关机

### 2.1 构建（Windows / PowerShell）

```powershell
cd C:\Users\Administrator\Desktop\work\bsd45
# 坑：环境里同时存在大小写两套代理变量会让 Start-Process 崩，先在进程内清掉
[Environment]::SetEnvironmentVariable('https_proxy',$null,'Process')
[Environment]::SetEnvironmentVariable('http_proxy',$null,'Process')

.\build_kernel.ps1 -Define 'VM_PROCTEST_AUTOTEST'   # 内核（带自动测试入口）
.\build_iso.ps1                                     # 打包 ISO（KernelLba=8192）
```

`VM_PROCTEST_AUTOTEST` 的作用：`monitor()` 打印 banner 后直接
`start_userland("/bin/proctest")`，不需要键盘输入就能跑到测试结束
（宏定义在 `kernel/sys/mon.c`）。

### 2.2 启动（必须放后台任务里）

```bash
cd /c/Users/Administrator/Desktop/work/bsd45
rm -f vms/serial.log
"/c/Program Files (x86)/VMware/VMware Workstation/vmrun.exe" \
    -T ws start vms/BSD45-proctest.vmx nogui
sleep 300      # ← 关键：这个 shell 不能退出
```

**坑**：`vmrun start` 返回后 vmx 是独立进程，但如果外层 shell/任务被
工具杀掉（进程组回收），vmx 会跟着死。所以启动命令要用后台任务方式跑，
后面挂一个长时间的 `sleep`；验证完再 `vmrun stop`。

### 2.3 关机

```bash
vmrun -T ws stop vms/BSD45-proctest.vmx hard
```

`soft`（ACPI）会挂住——guest 没有 ACPI 驱动，别用；`hard` 秒退。
系统状态确认：

```bash
vmrun -T ws list          # 列出正在运行的 VM
```

### 2.4 键盘交互（可选）

`vmrun start` **不加** `nogui` 会开窗口；启动后先点一下 VM 窗口再敲键盘
（VMware 要捕获输入）。进 `bsd45>` 后可以 `usermode` / `shell` 手动试。

---

## 3. 拿输出：快照 + 读 VGA 文本（推荐）

VMware 跑完（或跑到中途）后：

```bash
vmrun -T ws snapshot vms/BSD45-proctest.vmx s1
ls vms/*-Snapshot1.vmem                                   # 512MB 的 guest 物理内存镜像
python tools/vm_screen.py vms/...Snapshot1.vmem --klog build/bsd45.elf
```

输出是内核控制台的完整文本（`klog_buf` 是内核里的一份 RAM 副本，
`pccnputc()` 每写一个字符就追加，8KB 够放整段启动日志）。
**这是本仓库唯一可靠的 VMware 观测手段。**

`--klog` 之外还能直接读内核全局变量（低端物理内存是恒等映射）：

```bash
python tools/vm_screen.py vms/...Snapshot1.vmem --mem 0xb8000 4000   # 原始 hex
python tools/vm_screen.py vms/...Snapshot1.vmem --sym build/bsd45.elf procs 8
```

`--sym` 用 ELF 符号表算物理地址：**phys = symbol_value - 0xFE000000**。
所以 `procs[0].uk_pid`、`kthreads[0].t_state`、`page_ninuse` 全都能从
快照里读出来——等于一个"离线调试器"，不需要 gdb。

> 快照会短暂暂停 guest，不影响后续运行；不需要时
> `vmrun deleteSnapshot vms/BSD45-proctest.vmx s1` 清掉，否则 `.vmem` 占 512MB。

### 3.1 用这个方法定位到的真实 bug（示例）

本次就是这样抓到"VMware 里根本不启动"的：

```
# 快照里 0x1000 处内核 .text 全 0、0x10000 却有 ELF 头、0x90000 也是 0
# → stage1 读进来了，但 stage2 没被搬过去 → stage1 卡在 read_loop
```

然后按 2KB 块比对"暂存区 vs ELF 文件"：

```python
elf=open('build/bsd45.elf','rb').read()
d=open('vms/...Snapshot1.vmem','rb').read(0xB0000)
staged=d[0x10000:0x10000+len(elf)]
print(hex(len(elf)))                       # 0x97ecc = 618KB
for i in range(0,len(elf),2048):
    if staged[i:i+2048]!=elf[i:i+2048]:
        print('first mismatch at file %#x -> phys %#x' % (i,0x10000+i)); break
# → first mismatch at 0x90000 -> phys 0xa0000
```

结论：**BIOS `INT 13h` 不会往 0xA0000（显存窗口）里写**，stage1 的
暂存区上限就是 640KB−0x10000 = 576KB。内核 ELF 一旦超过 576KB，
最后那段就永远读不进来，内核 .data 是残缺的 → 各种"布局敏感"的怪现象
（ATAPI/ISO 挂载失败、控制台乱码，其实都是半截内核在跑）。

根因不在引导器，而在**镜像被无谓地撑大了 450KB**：见 §4。

---

## 4. 构建期的两个坑（会让 VMware 里的表现完全对不上）

### 4.1 `python` 解析到 Microsoft Store 桩 → `pe_bss_fix.py` 被静默跳过

`build_kernel.ps1` 用 mingw32（PE/COFF 目标）编译，`static char heap[256KB]`
这类零初始化静态量会被放进 `.data$xxx`（**带 256KB 真实内容**）。所以要用
`tools/pe_bss_fix.py` 把它们改写成 `.bss`。

旧代码：

```powershell
$py = Get-Command python -ErrorAction SilentlyContinue   # ← 拿到 WindowsApps 桩
if ($py) { & $py.Source tools\pe_bss_fix.py --recurse build\obj }
```

`%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe` 是 Store 别名，执行它
只会弹商店并返回非 0，**什么都不做**。结果：

| | 镜像大小 | .data | 能否被 stage1 读完 |
|---|---|---|---|
| 没跑 fixer | 618,188 B | 447 KB | ❌ 超过 576KB，VMware 直接不启动 |
| 跑了 fixer | 176,012 B | 1 KB | ✅ |

修法：`tools-find.ps1` 新增 `Get-PythonExe`（跳过 `\WindowsApps\`、
实际执行 `python -c "print(1)"` 验证），`build_kernel.ps1` 改用它，
并且**找不到可用 python 时直接报红警告**，不再静默。

同一份源码在 Ubuntu（ELF 工具链）上本来就是 123KB——这就是
"Windows 版内核特别大"的真相。

### 4.2 内核 console 在 VMware 里被判"没有串口"

`pccon.c` 的旧探测只读 LSR 判 `(lsr & 0xe0) == 0x60`。VMware 的文件型
16550 在 LCR/FCR 被写之前 LSR 读回 `0x00`，于是 `com_present = 0`，
**整份日志被丢弃**（这就是"serial.log 0 字节"的一半原因）。

修法：先 `com_init()`（写 LCR/FCR/divisor）再探测，并用 16550A 的
scratch 寄存器（`0x3f8+7`）写读比对，LSR 只作兜底。

---

## 5. 为什么串口抓不到（实测记录，别重试）

1. `serial0.fileType = "file"` + `serial0.fileName = "serial.log"`：
   vmware.log 里有 `[msg.serial.file.open] The serial port output file
   "serial.log" already exists.`，文件被创建，但**始终 0 字节**。
   即使内核用 `-DVM_FORCE_COM1` 无条件 `outb` 也一样。
2. `serial0.fileType = "network"` + `tcp://127.0.0.1:9977`：
   先起 `python tools/vm_serial_sink.py 9977 vms/serial.log`（监听正常），
   再启动 VM —— **VM 不发起连接**，`accept()` 一直阻塞。
3. `vmrun captureScreen`：报
   `此虚拟机不允许执行匿名客户机操作。必须先调用 VixVM_LoginInGuest`
   —— 需要 VMware Tools，本 guest 没有。
4. `debugStub.enable = "TRUE"` / `debugStub.port = 8864`：
   `vmrun` 能启动，但 `127.0.0.1:8864` 直接 `ConnectionRefused`
   （Workstation 17 不实现 ESXi/Fusion 那个 gdb stub）。

所以：**VMware 里靠快照读 VGA；要全量串口日志就临时用 QEMU 对照**：

```bash
qemu-system-i386.exe -cdrom out/BSD45-ELF-x86.iso -boot d -m 512 \
    -display none -serial file:build/qemu_ser.log \
    -debugcon file:build/qemu_dbg.log -no-reboot
```

`stage2.s` 的早期检查点同时写 `0xE9`（QEMU debugcon）和 COM1，
`1234a5` 就是"stage1 检查点全过 + stage2 存下了 ELF 头"的字节证据。

---

## 6. 一次完整的调试回合（照抄即可）

```bash
cd /c/Users/Administrator/Desktop/work/bsd45

# 1) 构建
powershell -NoProfile -File .\build_kernel.ps1 -Define 'VM_PROCTEST_AUTOTEST'
powershell -NoProfile -File .\build_iso.ps1

# 2) 后台启动（另一个终端/任务里，末尾要有 sleep 保活）
vmrun -T ws start vms/BSD45-proctest.vmx nogui; sleep 300

# 3) 等测试跑完（proctest 约 30-60s），拍快照
vmrun -T ws snapshot vms/BSD45-proctest.vmx s1

# 4) 读屏
python tools/vm_screen.py vms/BSD45-proctest-Snapshot1.vmem | tail -25

# 5) 收工
vmrun -T ws stop vms/BSD45-proctest.vmx hard
vmrun -T ws deleteSnapshot vms/BSD45-proctest.vmx s1
```

---

## 7. 已知会咬人的细节

- **删快照**：`.vmem/.vmsn` 各 512MB，拍多了会占满盘。
- **ISO 被占用**：VM 还挂着光驱时 `build_iso.ps1` 会写出截断镜像
  （ISO 只有 ~2MB）。先 `vmrun stop`，必要时
  `Get-Process vmware-vmx | Stop-Process -Force`，再删 `out\*.iso` 重打。
- **`hdd/BSD45.JLFS` 每次构建都会变**，它是 git 跟踪文件，提交前
  `git checkout -- bsd45/hdd/BSD45.JLFS`。
- **快照里读到的内存是"拍的那一刻"**：要看崩溃现场就在崩溃后立刻拍；
  本内核 panic 会 `for(;;)` 停住，正好拍。
- **符号地址换算只对低端恒等映射区成立**：`phys = sym - 0xFE000000`。
  如果 `sym >= 0xFE400000`（VA 窗口里映射的是别的物理页，比如进程表
  窗口 `0xFE400000`、TCB 窗口 `0xFE600000`），这个换算**不成立**，
  要先用 `--mem 0x...` 找到对应物理页再读。
- **VMware 对未映射 I/O 端口返回 0xff**，会让轮询式串口/键盘驱动误判
  （历史上导致"键盘失灵"），改驱动时要考虑这一点。

---

## 8. 相关文件

| 文件 | 作用 |
|---|---|
| `bsd45/vms/BSD45-proctest.vmx` | 自动测试用 VM（512MB，ISO 启动） |
| `bsd45/tools/vm_screen.py` | 从 `.vmem` 读 `klog_buf` / dump 内存 / 读符号 |
| `bsd45/tools/vm_serial_sink.py` | TCP 串口接收器（VMware 不连，留作 QEMU/其他 VM 备用） |
| `bsd45/tools/vm_gdb.py` | gdb RSP 客户端（仅当 VM 支持 debugStub 时可用） |
| `bsd45/kernel/sys/mon.c` | `VM_PROCTEST_AUTOTEST` 自动测试入口 |
| `bsd45/kernel/driver/pccon.c` | COM1 探测 + `klog_buf` 控制台 RAM 副本 |
| `bsd45/tools/pe_bss_fix.py` | 把零初始化静态量从 `.data` 挪到 `.bss`（镜像瘦身 450KB） |

---

## 9. 当前未解问题（2026-11-21，留给下一个人）

同一份 ISO：

- **QEMU `-m 512`**：`proctest: PASS`（phase A 200、phase B 200 个并发子进程）。
- **VMware**：stage1 → stage2 → 内核入口都走到了，但**控制台初始化之前就停住**。

证据（从 `.vmem` 读出来的，不是猜）：

```
0x1000 : 66 b8 34 12 66 a3 72 04 ...   # 内核 start 已就位
0x90000: fa 66 b8 10 00 8e d8 ...      # stage2 已搬到 0x90000
fe01b004 (.data) = 1                   # locore 的 _cold = 1 → start 跑过了
fe01b010 = 0x00086000                  # _IdlePTD 已写 → 页表建好了
klog_buf 全空                          # 一次 printf 都没发生
```

也就是说死点在 `begin:` 之后、`cninit()`/第一次 `printf()` 之前。候选：

1. `locore.s` 里 `_Crtat` 的重定位（`subl $0xfe0a0000` + `atdevbase`）在 VMware
   的 I/O 窗口布局下算出的 VA 不可写 → `pccninit()` 清屏时缺页；
   验证方法：在 `begin:` 之后加 `klog_putc` 级检查点（`pccon.c` 的
   `klog_putc` 不依赖 console 已经初始化，可以直接调用）。
2. `_init386()` 里 `pmap_bootstrap()` 之前的某段（`printf("ps %x pe %x ")`
   就在 `pmap_bootstrap` 开头，它也没出来 → 说明还更早）。
3. VMware 的 8042/PCI 探测路径（`smp.c` 的 LAPIC 探测）在真实硬件模型上
   读到不同的值而自锁。

排查建议：直接用 QEMU 的 `-serial file:` 做"能跑通"的对照，然后在
`locore.s` / `glue.c` 的关键点插入 `klog_putc` 检查点，再拍 VMware 快照
定位。
