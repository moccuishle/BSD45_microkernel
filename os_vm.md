# BSD45 的虚拟地址实现

> 一句话：**段式平坦 + 两级页表**。内核把 4.4BSD 的 `pmap.c` 当成「PTE 写入器」用；真正的地址空间管理（页目录、页表、COW、按需分页、压缩内存）是 `kernel/sys/usermode.c` 自己手写的，**没有** `vm_map` / `vm_object` / `vm_page` 那套抽象。

---

## 1. 涉及的文件（只列真正参与编译的）

| 文件 | 行数 | 角色 |
|---|---|---|
| `kernel/arch/i386/isa/locore.s` | 1725 | 启动时**手搓**页目录和页表，开分页，跳到高半区 |
| `kernel/arch/i386/isa/pmap.c` | 1768 | 4.4BSD pmap：`pmap_map` / `pmap_enter` / `pmap_pte`，只写 PTE |
| `kernel/sys/usermode.c` | 5200+ | **地址空间主体**：页目录创建、用户页映射、COW、缺页、mmap、页分配器 |
| `kernel/sys/usermode_asm.s` | 100+ | `iret` 进 ring3、`resume_user` 恢复上下文并切 CR3 |
| `kernel/arch/i386/ctx.c` | 150+ | `hal_address_space_activate()` = 写 CR3 |
| `kernel/vm/zcomp.c` | 269 | 内存压缩池（128 槽 × 2 KB，RLE） |
| `kernel/vm/pagecache.c` | 458 | 文件页缓存（128 页），mmap 文件映射的后端 |
| `kernel/thread/kthread.c` | 1000+ | 内核栈按页映射到 `0xFF000000`（S410） |

**没有编译的**（全是 4.4BSD 原树搬来的参考代码，构建脚本一行都没引用）：
`vm/vm_map.c`（72 KB）、`vm_fault.c`、`vm_object.c`、`vm_page.c`、`vm_pageout.c`、`vm_swap.c`、`swap_pager.c`、`vnode_pager.c`、`device_pager.c`、`vm_kern.c`、`vm_mmap.c`。
所以这个系统里**没有 vm_map 区间树、没有 pageout 守护、没有 swap**。

---

## 2. 硬件模型

- **分段**：GDT 全平坦（base 0 / limit 4 GB），内核用 `0x08/0x10`，用户用 `0x1b/0x23`。分段在这里只提供特权级隔离，不做地址翻译。
- **分页**：32 位两级页表，页大小 4 KB，**没有 PAE，没有 4 MB 大页**。

地址拆解（`usermode.c:594`、`621` 等处手写）：

```
pdi = va >> 22            // 页目录索引，0..1023
pti = (va >> 12) & 0x3ff  // 页表索引
off = va & 0xfff
```

- **CR3 = 页目录物理地址**。换 CR3 就是换地址空间；`hal_address_space_activate()` 里只有一句 `mov %eax,%cr3`（`arch/i386/x86.asm:69`）。
- **TLB 刷新**：`tlbflush()`（`arch/i386/isa/locore.s:906`）就是重载 CR3；`pmap_enter()` 每次改完 PTE 都调一次，**没有延迟/批量刷新**。

---

## 3. 地址空间全景

```
0x00000000  ┌────────────────────────────┐
            │ 用户态不可用（PDE0 启动后清掉） │
0x08048000  ├────────────────────────────┤ USER_LOAD：ELF 映像（text/data/bss）
            │ ... 空洞 ...                │
0x09000000  ├────────────────────────────┤ USER_HEAP_START（brk 起点）
            │ 堆（按需零页，可长到 0x0C000000）│ USER_HEAP_MAX
0x10000000  ├────────────────────────────┤ MMAP_BASE（VMA 区）
            │ mmap 映射（≤ 0x30000000）    │ MMAP_LIMIT / 单映射 ≤ 4 MB
0xBFC00000  ├────────────────────────────┤ USER_STACK_TOP
            │ 用户栈 32 KB（向下生长）      │ USER_STACK_SIZE = 0x8000
0xC0000000  ├────────────────────────────┤ user_ok() 的上界
            │ 空洞                        │
0xFE000000  ├────────────────────────────┤ KERNBASE：内核恒等映射 12 MB
0xFE200000  │ 临时窗口：页目录/页表/页      │ SCRATCH_PD/PT/PAGE、SCRATCH2_*
0xFE300000  │ 压缩页池 64 页               │ ZCOMP_VA
0xFE400000  │ CD 映像窗口（≤ 4 MB）        │ CD_IMAGE_VA
0xFE500000  │ 异步 I/O 每槽一页            │ AIO_VA
0xFE600000  │ tmpfs 节点页                 │ TMPFS_VA
0xFE700000  │ 用户态 ring 缓冲             │ URING_KVA
0xFE800000  │ 块设备缓冲 64 页 / LAPIC(未映射)│ BUF_VA
0xFE900000  │ 文件页缓存 128 页            │ PCACHE_VA
0xFEA00000  │ 内核栈 16 × 8 KB             │ KSTACK_VA
0xFDC00000  │ 递归页表窗口 PTmap           │
0xFDFF7000  │ 页目录自身（PTD）            │
0xFF7FF000  ├────────────────────────────┤ VM_MAX_KERNEL_ADDRESS
0xFF800000  │ 备用递归窗口 APTmap（PDE 0x3FE）│
0xFFFFFFFF  └────────────────────────────┘
```

---

## 4. 启动：页表是汇编手搓的

`locore.s` 在做任何 C 代码之前就把分页打开了（`locore.s:217-310`）：

1. **算内核末尾**：`esi = round_up(_end) - SYSTEM`（`_end` 是链接符号，减 `SYSTEM=0xFE000000` 得到物理地址）。内核被 bootloader 装在**物理 0x10000**，所以这里算出的是 0x3xxxx 量级，不是 1 MB。
2. **清 BSS + 引导用内存**：从 `_edata` 清到 `_end + (UPAGES+5)*4K`。
3. `_IdlePTD = esi`：**页目录就放在内核末尾那一页**。
4. **内核页表（KPT）放在 PD 之后 4 页**（`_KPTphys`）。`fillkpt` 循环逐项填 `pte[i] = i*0x1000 | PG_V` —— 这就是**物理内存的恒等映射**。
5. **IO 映射**：接着把 `0xA0000..0x100000`（VGA/BIOS 区）填进 PT，属性 `PG_UW`。
6. **proc0 内核栈**：`UPAGES` 个 PTE 映射到 PD 的 `PPDROFF=0x3F6` 项。
7. **三张 PDE**：在 `0x3F8/0x3F9/0x3FA` 装上三个连续页表页，覆盖 `0xFE000000-0xFFBFFFFF`（12 MB）。这三个 PDE 指的就是前面的恒等页表，所以 **内核虚拟地址 0xFE0xxxxx 就是物理 0xxxxx**（`pccon.c` 里 `VGA_COLOR = 0xFE0B8000` 正是靠这个）。
8. **递归 PDE**：`0x3F7` 指向 PD 自己 → 形成 `PTmap`，之后可以用 `vtopte(va)` 直接算出任意 VA 的 PTE 指针。
9. `mov %esi,%cr3` + `orl $0x80000001,%cr0`（PG|PE）→ **分页开启**。
10. `pushl $begin; ret` 跳到高半区继续执行；`begin` 里把 `_PTD`（PD 第 0 项，即临时恒等窗口）**清零**，低端 4 MB 从此对内核也不可见，留给用户地址空间。

然后 `call _init386` → `pmap_bootstrap()`，再 `call _main`（`glue.c:322`）。

`pmap_bootstrap()`（`pmap.c:218-298`）在这个端口上**只做记账**，不重建页表：

```c
avail_start = 0x100000;                 // 强制从 1 MB 开始可分配
avail_end   = maxmem << 12;             // 再减掉 msgbuf
virtual_avail = atdevbase + 0x100000 - 0xa0000 + 10*NBPG;
kernel_pmap->pm_pdir = (pd_entry_t *)(0xfe000000 + IdlePTD);  // 指向 locore 建好的 PD
```

---

## 5. pmap：只负责写 PTE

`pmap_map(virt, start, end, prot)` 是最常用的入口（`pmap.c:411`）：

```c
while (start < end) {
    pmap_enter(kernel_pmap, virt, start, prot, FALSE);
    virt += PAGE_SIZE;  start += PAGE_SIZE;
}
```

`pmap_enter()`（`pmap.c:872`）的核心就三步：

1. `pte = pmap_pte(pmap, va)` —— 通过递归窗口拿到 PTE 指针；
2. 如果原来映射的是同一物理页，只更新保护/接线标志，跳到最后；
3. 否则 `pmap_remove()` 掉旧映射，把 `pa | pte_prot(prot) | PG_V` 写进 PTE，最后 `tlbflush()`。

`pmap_pte()`（`pmap.c:1167`）的判断很关键：

- `pmap == kernel_pmap` 或「该 PD 就是当前 CR3」→ 用 `vtopte(va)`，即 `PTmap + (va>>12)`，走**当前**页目录的递归窗口；
- 否则切 `APTDpde` 到目标页目录，用 `avtopte(va)`（`APTmap`）—— 这是给「改别的进程页表」准备的备用递归窗口。

> 注意：`locore.s` 只装了递归 PDE `0x3F7`，**没有装 `APTmap` 用的 `0x3FE`**。而且全系统调用 `pmap_map`/`pmap_enter` 时传的都是 `kernel_pmap`，所以 `avtopte()` 那条分支实际上从没走到 —— 用户页表全部由 `usermode.c` 通过 `SCRATCH_PD/PT` 窗口直接改。

### 一个必须知道的事实：`pmap_init()` 从未被调用

`pmap_init()` 定义了（`pmap.c:351`），但整个系统里**没有调用点**（`vm_init.c` 没编译）。后果：

```c
vm_first_phys = vm_last_phys = 0;      // 全局变量，BSS 里本来就是 0
...
if (pa >= vm_first_phys && pa < vm_last_phys) { /* 挂 pv_table */ }   // 永远不成立
```

所以：

- **`pv_table` 是空的**，pmap 不记录「哪些虚拟地址映射了同一个物理页」；
- `pmap_page_protect()`、`pmap_remove_all()`、`pmap_copy_on_write()`、`pmap_is_modified()` 这些依赖 pv 链的接口在运行时**全是死代码**（全系统 0 处调用）；
- 反向映射这件事，改由 `usermode.c` 自己的 **`page_ref[]` 字节表**承担（见第 8 节）。

也就是说：**这里的 pmap 退化成了一个「往 PTE 里写物理页号」的函数库**，其余 VM 语义都在 `usermode.c`。

---

## 6. 内核虚拟窗口：全局共享

上面那张表里 `0xFE200000` 以上的窗口全部落在 PDE `0x3F8-0x3FA`，而所有用户页目录都会**复制这三个 PDE**（见下节）。所以这些窗口是**全系统共享的**，不随进程切换而变：

- 好处：内核在任何进程上下文里都能用同一套窗口读写任意物理页；
- 代价：改窗口就是改所有地址空间；`smp.c:170` 的注释说得很直白 —— 映射 LAPIC 会推进 `pmap_map` 的内部状态、破坏 `exec` 依赖的页表布局，所以 `lapic_map()` 里直接 `lapic_va = 0` 放弃映射。

⚠️ `BUF_VA`（`fs/vfs_bio.c:49`，0xFE800000，64 页）和 `LAPIC_VA`（`arch/i386/smp.c:19`，同样 0xFE800000）**地址重叠**。因为 LAPIC 映射被禁用，目前没有真正冲突，但哪天启用 SMP 就会互相踩。

---

## 7. 用户地址空间：自己建 PD/PT

### 7.1 建页目录

`user_pd_create()`（`usermode.c:569`）：

```c
pdpa = page_alloc();                        // 一页物理内存当 PD
pd = map_page(SCRATCH_PD, pdpa);            // 通过临时窗口访问它
for (i = 0; i < 1024; i++) pd[i] = 0;
for (i = 0x3f6; i <= 0x3fa; i++)            // 只共享 5 个 PDE
    pd[i] = ((unsigned *)kernel_pmap->pm_pdir)[i];
```

复制的 5 项分别是：`0x3F6` proc0 内核栈、`0x3F7` 递归窗口、`0x3F8-0x3FA` 内核 12 MB。**用户空间 0-4 GB 的其余 1019 个 PDE 全是 0**，按需创建。

### 7.2 改页表的唯一姿势：临时窗口

内核不能直接解引用任意物理地址，所以 `usermode.c` 用三个固定 VA 当「手术窗口」：

| 常量 | 地址 | 用途 |
|---|---|---|
| `SCRATCH_PD` | 0xFE200000 | 映射「正在编辑的页目录」 |
| `SCRATCH_PT` | 0xFE201000 | 映射「正在编辑的页表」 |
| `SCRATCH_PAGE` | 0xFE202000 | 映射「单个数据页」（清零/拷贝/空闲链） |
| `SCRATCH2_PD/PT/PAGE` | 0xFE203000/40/50 | 第二组，供 COW 同时持有源和目的 |

`map_page(va, pa)`（`usermode.c:546`）就是 `pmap_map(va, pa, pa+4096, RW)` 再返回 `(void*)va`。

> 这里有个真实修过的 bug：早期所有函数都用全局的 `SCRATCH_PD` 指向「当前进程」，但在 `fork`/`exec` 之后它指向的已经不是要编辑的那个进程了，**结果静默地把父进程的堆解除映射**。所以现在 `user_map()` 等函数都显式传 `pdpa` 参数（`usermode.c:584-588` 的注释）。

### 7.3 用户页操作

| 函数 | 位置 | 行为 |
|---|---|---|
| `user_map(pdpa, va, pa)` | `usermode.c:591` | PDE 无效就分配页表；PTE = `pa\|PG_V\|PG_RW\|PG_u`（**永远可写**） |
| `user_map_prot(pdpa, va, pa, prot)` | `usermode.c:638` | 同上，但 `PG_RW` 取决于 `PROT_WRITE`（mmap / ELF 用） |
| `user_unmap(pdpa, va)` | `usermode.c:663` | PTE 清零 + `tlbflush()` |
| `user_getpa(pdpa, va)` | `usermode.c:677` | 反查物理页，未映射返回 0 |
| `user_pte_ptr(pdpa, va, &pt)` | `usermode.c:620` | 返回 PTE 下标；PDE 无效时 `*pt = 0` 并返回 0 |

### 7.4 用户布局

| 区间 | 地址 | 谁建立 | 何时真正占页 |
|---|---|---|---|
| ELF 映像 | `0x08048000` 起 | `load_elf()` | exec 时**立即**全部映射 |
| 堆 | `0x09000000` - `brk` | `brk` 只记录上限 | 首次访问缺页才分配零页 |
| mmap | `0x10000000` - `0x30000000` | `do_mmap()` 只登记 VMA | 首次访问缺页才填充 |
| 栈 | `0xBFC00000` 往下 32 KB | `build_args()` 只映射参数页 | 首次压栈缺页才分配零页 |

---

## 8. 物理页分配器

`page_alloc()` / `page_free()`（`usermode.c:455` / `494`）：

```
bump 指针 next_pa 从 0x200000 开始（内核占 0x10000-0x1FFFFF 之外）
每次 +4096；到 0x3F000000（约 1008 MB）panic("out of physical memory")
释放的页压进 LIFO 空闲链，next 指针直接写在页首（经 SCRATCH_PAGE 访问）
```

统计量：`page_nalloc` / `page_nfree` / `page_ninuse`，由 `vfs_page_stats()` 导出（monitor 的 `vstat`）。

**内存压力钩子**（`usermode.c:469`）：

```c
if (page_free_list == 0 && page_ninuse > 4096 && !in_zcomp && cur != 0)
        zcomp_reclaim(8);      // 先压缩 8 个私有匿名页，再考虑推进 bump
```

没有 swap 设备，这就是唯一的回收层。

---

## 9. 缺页：三条路径 + COW

### 9.1 入口

`int 14`（`T_PAGEFLT`）→ `alltraps` → `trap()`（`glue.c:225`）：

```c
if ((frame->tf_cs & 3) != 0) {            // 用户态
    unsigned va = rcr2();                 // CR2 = 出错地址
    if (frame->tf_trapno == T_PAGEFLT && user_page_fault(va, frame->tf_err))
        return;                           // 修好了，iret 重试那条指令
    ... 其余异常投递信号（SIGSEGV 等）
}
```

内核态缺页**不修复**，直接打印 `*** kernel TRAP 14` 并停机 —— 所以内核访问用户内存前必须先用 `user_ok()` 校验（第 12 节）。

### 9.2 路由表

`user_page_fault()`（`usermode.c:2240`）：

| `err` 位 | 含义 | 处理 |
|---|---|---|
| `err&4 == 0` | 不是用户态访问 | 返回 0（交给内核 panic） |
| `err&1 == 0`（P=0） | 页不存在 | 依次试 `zcomp_refault` → `demand_page` → `mmap_fault` |
| `err&7 != 7` | 存在但只读/非用户 | 返回 0 → SIGSEGV |
| `err==7` | 写一个 COW 页 | 独占或复制（下文） |

### 9.3 三条「不存在」路径

**① 压缩页回流** `zcomp_refault(va)`（`usermode.c:854`）
PTE 里带 `PG_ZCOMP` 标记 → 解压到新物理页 → 重装 PTE → 激活地址空间。

**② 按需零页** `demand_page(va)`（`usermode.c:2212`）

```c
if (page >= USER_HEAP_START && page < cur->uk_brk_mapped)      ;   // 堆
else if (page >= USER_STACK_TOP - USER_STACK_SIZE && page < USER_STACK_TOP) ; // 栈
else return 0;                                                // 其它 → SIGSEGV
pa = page_alloc();  zero_page(pa);  user_map(cur->uk_pd, page, pa);
```

**③ mmap 填充** `mmap_fault(va)`（`usermode.c:814`）

| VMA 类型 | 动作 |
|---|---|
| 匿名（`vp == 0`） | 分配零页，按 `prot` 映射 |
| `MAP_SHARED` + 文件 | 直接映射**页缓存那一页**（共享） |
| `MAP_PRIVATE` + 文件 | 分配新页、从页缓存拷一份、`pagecache_unref` |

### 9.4 COW 写故障（`err == 7`）

```c
if (page_ref_get(pa) <= 1) {                 // 只剩我一个
    pt[pti] = (pt[pti] & ~PG_COW) | PG_RW;   // 直接改成可写
} else {
    npa = page_alloc();
    bcopy(map(SCRATCH_PAGE, pa), map(SCRATCH2_PAGE, npa), 4096);
    page_put(pa);                            // 引用计数 -1
    pt[pti] = npa | PG_V | PG_RW | PG_u;
}
```

`PG_COW = 0x200`、`PG_ZCOMP = 0x400`（`usermode.c:49-50`）都借用 i386 PTE 的软件可用位（bit 9/10），硬件忽略。

---

## 10. fork：写时复制

`copy_address_space_cow(srcpd)`（`usermode.c:2165`）：

```
dstpd = user_pd_create()
for 每个有效 PDE (0 .. 0x3F5):
    分配一个新页表页，清零
    for 每个有效 PTE:
        if PTE 可写: 清 PG_RW，置 PG_COW        // 父子都变只读
        dpt[j] = spt[j]                        // 子进程指向同一物理页
        page_ref_inc(pa); cow_shared++
    dpd[i] = 新页表 | 原 PDE 标志
```

**引用计数**是 `unsigned char page_ref[16384]`（`usermode.c:307-308`），一页一字节，`page_index(pa) = pa >> 12`。

`page_put()`（`usermode.c:358`）在计数降到 1 时真正 `page_free()`。

⚠️ **表只覆盖 64 MB**（16384 × 4 KB）。超过 64 MB 的物理页 `page_index()` 返回 0，`page_ref_get()` 恒返回 1，于是：

- `page_put()` 会**直接释放**共享页 → 潜在 use-after-free；
- COW 写故障会误判「独占」→ 直接改可写，**父子不再隔离**。

在 64 MB 的测试机（QEMU/VMware 默认）上跑不出来，但这是硬限制。

---

## 11. exec：换地址空间

`exec_image()`（`usermode.c:3443`）流程：

1. `vfs_namei` + `cd_map(len)`：把整个 ELF 读进内核窗口 `0xFE400000`（≤ 4 MB）；
2. `build_image()`：
   - `user_pd_create()` 新页目录；
   - `load_elf()`：遍历 `PT_LOAD`，**按页**分配物理页、清零、把文件字节 `bcopy` 到页内偏移处、`user_map_prot()` 按 `p_flags` 设权限；
   - `build_args()`：在 `USER_ARG_PAGE = 0xBFC00000-0x1000` 这一页上摆 `argc/argv/envp` 指针数组，返回初始 `esp`；
3. `mmap_teardown(cur)` 清 VMA 和引用；
4. `cur->uk_pd = pd`、`uk_brk_mapped = USER_HEAP_START`；
5. `hal_address_space_activate(pd)` 写 CR3；
6. 把 `entry/esp` 写回 `struct sysregs`，返回用户态时 `iret` 直接落到新映像。

`load_elf()` 里有个值得记的细节（`usermode.c:1530-1537` 注释）：`PT_LOAD` 只保证页**同余**不保证**对齐**（Linux 链出来的 `.data` 可能是 `0x080550e0`），所以它从「包含 p_vaddr 的那一页」开始映射，文件字节落在页内偏移处，并把 `memsz - filesz` 的部分留成零页。

---

## 12. 内核怎么访问用户内存

内核**不切换 CR3**（系统调用进来时 CR3 仍是该进程的），所以用户页在内核态是直接可见的。但必须先校验，否则内核态缺页 = 停机。

`user_ok(p, len)`（`usermode.c:3242`）：

```
范围检查：p >= USER_LOAD 且 p+len <= 0xC0000000，否则 EFAULT
逐页走页表 user_pte_ptr()：
    页不存在 → 依次试 zcomp_refault / demand_page / mmap_fault，成功就继续
    PTE 带 PG_COW → 调 user_page_fault(a, 7) 给它一个私有副本
全部通过才返回 1
```

`user_ok_w()` 额外要求每页 `PG_RW`（写权限）。

`ucopyin/ucopyout` 就是 `user_ok` + `bcopy` 而已 —— 因为没有 SMAP/SMEP，也不需要在 4 GB 之上另开 copy 窗口。

---

## 13. CR3 切换与「同进程不换表」

`switch_pd_for(p)`（`usermode.c:271`）：

```c
if (p->uk_pd != cur_pd) { cur_pd = p->uk_pd; return cur_pd; }
return 0;       // 同一个地址空间，别动 CR3
```

返回值交给 `resume_user(pd, regs)`，汇编里（`usermode_asm.s`）：

```asm
testl %eax,%eax
jz 1f                 // pd == 0 → 保持当前 CR3（同进程线程切换，省一次 TLB 全刷）
movl %eax,%cr3
1: iret
```

这是 KLT（内核级线程）带来的直接收益：同一进程的两个线程互相切换时，**不刷 TLB**。

---

## 14. 压缩内存（没有 swap 的回收层）

`vm/zcomp.c`：

- **池**：`ZCOMP_SLOT_COUNT=128` 个槽 × `ZCOMP_SLOT_SIZE=2048` = 256 KB，物理页在 `zcomp_init()` 里一次性分配并映射到 `ZCOMP_VA=0xFE300000`（64 页，每页放 2 个槽）。
- **编码**：游程编码（RLE），每对 `(count 1..255, byte)`；全零页编码成长度 0 的槽 0；压不进 2 KB 就判「不可压缩」，调用方保持原页。
- **PTE 表示**：`(slot << 12) | PG_ZCOMP | PG_u`，**不带 PG_V** —— 所以访问必缺页，走 `zcomp_refault()` 解压回来。
- **谁被压缩**（`zcomp_reclaim`，`usermode.c:1337`）：必须是 present + 可写 + 用户 + **非 COW** + **不在任何 VMA 里** + 引用计数为 1 的页。文件页和共享页一律不动（它们可以重新读）。
- **触发**：`page_alloc()` 里空闲链空且已用页 > 4096 时，一次压 8 页。

---

## 15. 文件页缓存（mmap 文件映射的后端）

`vm/pagecache.c` 自己实现了本该由 `vm_object`/`vnode_pager` 干的活：

- `NPCACHE=128` 页，每页一个物理页，映射到 `PCACHE_VA=0xFE900000`；
- 哈希表（`key = pagecache_key(vp)`，ISO9660 用 LBA，tmpfs 用 vnode id）× 32 桶 + LRU；
- 未命中时用 `vp->ops->readraw()` 填充（绕过页缓存的直读）；
- `read()` 也走 `pagecache_read()`，所以第二次 `cat`/`exec` 同一文件直接命中内存；
- `mmap` 的 `MAP_SHARED` 直接映射缓存页本身，`MAP_PRIVATE` 拷一份。

---

## 16. mmap 子系统

| 参数 | 值 | 位置 |
|---|---|---|
| VMA 槽位 | 每进程 **8 个**（`KPROC_NMAP`） | `kproc.h:24` |
| 地址范围 | `0x10000000` - `0x30000000` | `usermode.c:149-150` |
| 单次映射上限 | 4 MB | `usermode.c:151` |

- `do_mmap()`（`usermode.c:741`）**只登记 VMA**，不分配页；
- `munmap`（`usermode.c:1388`）逐页解除映射：共享页 `pagecache_unref_key()`，私有页 `page_free()`；支持从头收缩；
- `msync`（`usermode.c:1435`）把 `MAP_SHARED` 的脏页经 vnode `write()` 写回；
- **`mprotect` 是空实现**（`usermode.c:4718`，`r->eax = 0`，注释「pages are mapped eagerly with prot」）。

系统调用号：`mmap=197`、`munmap=73`、`mprotect=74`、`msync=65`、`brk=17`。

---

## 17. 限制与坑（读代码发现，未做运行时验证）

1. **`page_ref` 只覆盖 64 MB** —— 超过之后 COW 失效且可能重复释放（见第 10 节）。65 MB 以上的虚拟机就会踩。
2. **VMA 只有 8 个槽**，且 `mprotect` 不生效；一个进程第 9 次 `mmap` 返回 `ENOMEM`。
3. **`brk` 只能升不能降**（`usermode.c:4239` 只更新更大的值），且堆起点 `0x09000000` 与 ELF 数据末尾之间是**空洞**：访问那里直接 SIGSEGV。
4. **用户栈固定 32 KB**（`USER_STACK_SIZE=0x8000`），`demand_page` 的范围是硬编码的，超出就 SIGSEGV —— 深层递归会挂。
5. **`pmap_init()` 从未调用**，`pv_table` 空 → `pmap_page_protect()` / `pmap_remove_all()` 等反向映射接口是死代码，谁调用都不生效。
6. **所有内核窗口共享同一套 PDE**，改一个窗口影响所有进程；也正因为如此，LAPIC 映射被主动放弃（`smp.c:170`）。
7. **`BUF_VA` 与 `LAPIC_VA` 同址** `0xFE800000`，SMP 一启用就会互相踩。
8. **每次 `pmap_enter` 都 `tlbflush`（重载 CR3）**，映射 N 页就刷 N 次 TLB，没有批量/延迟刷新。
9. **`usermode.c:134` 的注释过时**：写着「this port has no demand paging (a page fault stops the machine)」，但代码里缺页是能修的（`mmap_fault`/`demand_page` 都在跑）。别被注释误导。
10. **内核态缺页 = 停机**，所以任何直接解引用用户指针而没走 `user_ok()` 的新代码都是定时炸弹。

---

## 18. 怎么验证

monitor（`glue.c` 启动后进入）里的命令：

| 命令 | 输出 | 代码 |
|---|---|---|
| `mem` | Maxmem / physmem / avail / virt / kernel text,data,end | `mon.c:cmd_mem` |
| `vstat` | vnode 数、页分配/释放/在用、缓存计数 | `mon.c:cmd_vstat` |
| `pte <hex>` | 该 VA 的 PTE 值、PFN、V/RW/U 标志 | `mon.c:123` |
| `cow` | `cow: shared N faults N copies N` | `usermode.c:cow_print` |
| `zcomp` | 压缩/解压次数、节省字节、拒绝数、池满次数 | `zcomp.c:zcomp_print` |
| `vstress` | 100 次 ISO 读 + 100 次 tmpfs mkdir/rmdir 前后的页计数 | `mon.c:cmd_vstress` |

> 注意 `pte` 用的是 `pmap_pte(kernel_pmap, va)`，走的是**当前**页目录的递归窗口；在 monitor 里看用户 VA 未必准，看内核窗口最可靠。

用户态验收程序（`libexec/`）：

| 程序 | 验证点 |
|---|---|
| `cowtest` | fork 后子改进程改变量、父仍见旧值；`int $0x80` 431 号取 COW 计数，要求 faults > 0 |
| `mmaptest` | MAP_PRIVATE 文件映射内容 == `read()`；匿名映射写读回；MAP_SHARED + `msync` 后 `read()` 能看到改动 |
| `vmstat` | 读 `/dev/vmstat`（fd 类型 `FD_VMSTAT`）打印缓存计数 |
| `badptr` / `crash` | 故意访问非法地址，验证是 SIGSEGV 而不是内核 panic |

---

## 19. 与 4.4BSD 的差异

| 4.4BSD | BSD45 |
|---|---|
| `vm_map` 区间树管理所有映射 | 只有用户进程 8 个 VMA 槽 + 内核固定窗口 |
| `vm_object` + pager（vnode/swap/device） | 只有 `pagecache`（文件）+ `zcomp`（匿名压缩），无 swap |
| `vm_page` 全局页表 + 队列 + pageout 守护 | bump 分配器 + LIFO 空闲链，无 pageout |
| `pmap` 维护 pv_table（反向映射） | `pmap_init` 未调用，pv 链关闭，反查靠 `page_ref[]` |
| `fork` 复制 `vm_map`，COW 由 `vm_fault` 处理 | `copy_address_space_cow` 直接抄页表 + usermode 自己的缺页处理 |
| 内核栈在 `USRSTACK` 之上的 UPAGES 区（每进程一份） | 内核栈在 `0xFEA00000` 固定槽位（16 × 8 KB），所有进程共享同一 PDE |
| 用户地址空间 0 - `0xFDBFD000`（3.9 GB） | 0 - `0xC0000000`（3 GB），且实际只用了 4 个固定区间 |
