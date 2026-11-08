# dict 模块需求与设计（S0）

- 日期：2026-11-08
- 状态：**S2 已实现**。可编译、可测、可安装；未打 tag（本轮不发布）
- 目标包：`dict`（`ct-pkg/dict`），仓库 https://github.com/ct-pkg/dict
- 上游：https://github.com/redis/redis ，核对基线 tag `7.2.4`（源码在 `src/`）
- 选型依据：`ct/docs/design/map-options.md`（提交 `c568c71`）§3.C / §5 / §6.S2

---

## 0. 结论摘要

1. **语义就是三件事**：值借用、增量 rehash（每次操作搬少量桶，没有全表停顿）、
   safe iterator（遍历中可删当前元素）。这三条是与 `hashmap` / `std::map` 的分界线。
2. **两种形状**：`DictStrPtr`（表持有的 `char *` 键副本 → 借用的 `void *` 值）与
   `DictIntPtr`（表持有的 4 字节 `int` 键槽 → 借用的 `void *` 值）。键由 `keyDestructor`
   回收；值永远不归包所有。
3. **键拷贝是 S2 的裁决**：`put` 复制键，调用方之后释放或改写自己的缓冲区都不影响条目。
   这样"覆盖保留首个键指针"才不会变成悬垂引用（Q1）。值仍然只借用，不拷贝。
   要连值一起托管，用 `hashmap`。
4. **OOM 语义 = 进程终止，两个包共同的契约**（`hashmap` 采用同一语义，见 §6 与
   `hashmap/docs/design.md` §6）。这不是能力缺失，而是刻意选择：`dict.c` 的分配点没有
   返回 `NULL` 的检查路径，`zmalloc` 的默认 handler 打印后 `abort()`；`hashmap` 虽然能用
   `hashmap_oom()` 检出失败，但链式 `put` 没有错误通道，检出后也只能"静默丢失 + 事后查
   状态"，因此同样终止。`reserve` 是唯一需要区分成因的入口：逻辑拒绝返回
   `DICT_INVALID`，真正的分配失败仍走同一 handler 终止（§6）。S2 只覆盖逻辑失败
   （键不存在、参数非法），不承诺 OOM 可恢复。
5. **迭代**用 safe iterator：`next()` 预取下一项，`removeCurrent()` 删除当前项后仍可继续。
   `key()` / `value()` 是借用指针，`next()` / `removeCurrent()` 之后失效。
6. **不提供自动析构**：只给 `release()`；`Drop` 由调用方单元自己写。
7. **公开面零 C 类型**：`dict` / `dictEntry` 只出现在 shim；句柄是包装结构
   `struct ct_dict_strptr` / `struct ct_dict_intptr`，不用 `void *` 宿主。
8. **许可**：Redis 7.2 及更早为 BSD-3-Clause（2026-11-08 读 `COPYING` 原文），
   `COPYING` 与每个源文件头必须保留，不得用 Redis 名义背书。7.4+（RSALv2/SSPL）与
   8.0+（AGPLv3）不得引入。`requires = ct >= 0.20.3`，S2 实测。

### 0.1 对 `map-options.md` 的三处事实更正（读 7.2.4 源码所得）

| 原文 | 7.2.4 实际 | 影响 |
| --- | --- | --- |
| 抽离 `serverassert` | `dict.c` 包含的是 `redisassert.h` | S2 的 include 闭包清单要改 |
| `dictGetKey` / `dictGetVal` 是宏 | 7.2.4 是**函数**（`src/dict.h` 约 171–172 行） | 不再需要宏展开，直接调用 |
| 调 `dict_can_resize` | 7.2.4 用 `dictSetResizeEnabled(int)` 改**进程级**开关 | 不能按表配置，v1 不暴露 |

另外 `dict.h` 还带 `mt19937-64.h`，`dict.c` 依赖 `fmacros.h`、`zmalloc.h`、
`redisassert.h`、`siphash`；抽取清单见 §12。

---

## 1. 场景与判据

| 场景 | `std::map` | `hashmap` | `dict` |
| --- | --- | --- | --- |
| 值由调用方长期持有，表只借用 | ⚠️ 需自己管指针 | ❌ 连值一起拷贝 | ✅ 首选 |
| 键可以是短命缓冲区，插入后即可释放 | ⚠️ 需自己留副本 | ✅ 会拷贝 | ✅ 会拷贝 |
| 十万级以上，扩容不能有停顿 | ❌ | ⚠️ 一次性 | ✅ |
| 遍历中删除 | ❌ | ❌ | ✅ |
| 想免掉第三方依赖 | ✅ | ✅ 单文件 | ❌ 抽取 8 个文件 |
| OOM 要能优雅降级 | ✅ 返回 -1 | ❌ abort（同本包） | ❌ abort |

判据只有两条是调用方可见的：**表是否持有你的键或值**、**扩容是否会造成停顿**。
本包持有键（副本）、只借用值；扩容是增量的。

---

## 2. 句柄与宿主

| CT 句柄 | C 侧包装 | 宿主 | 说明 |
| --- | --- | --- | --- |
| `DictStrPtr *` | `struct ct_dict_strptr { dict *d; }` | `DictStrPtr *` | 表持有的 `char *` 键副本 → 借用 `void *` 值 |
| `DictIntPtr *` | `struct ct_dict_intptr { dict *d; }` | `DictIntPtr *` | 表持有的 4 字节 `int` 键槽 → 借用 `void *` 值 |
| `DictStrPtrIter *` | `dictIterator *` + 当前 `dictEntry *` | `DictStrPtrIter *` | safe iterator |
| `DictIntPtrIter *` | 同上 | `DictIntPtrIter *` | safe iterator |

一个形状一个包装类型，理由同 `hashmap`：同一宿主不能有两个 `put` 签名。
`namespace dict` 只放构造器（无接收者的能力）。

---

## 3. 所有权与生命周期

### 3.1 表本身

- `newStrPtr()` / `newIntPtr()` 返回的句柄由调用方唯一拥有；`release()` 释放表与全部条目。
- `release(0)` 空操作；非空句柄只允许 `release()` 一次。
- **没有自动析构**，`Drop` 由调用方写。
- `clear()` 等价 `dictEmpty(d, NULL)`：删掉全部条目并释放键副本 / 键槽，不释放值指向的
  对象，表本身保留。

### 3.2 键：复制并由表持有

- `put("alice", v)` **复制** `"alice"`（`zmalloc(strlen + 1)` + `memcpy`）；条目里的键
  指针指向这份副本。调用方之后 `free` 或改写自己的缓冲区，都不影响条目。
- 键副本由表拥有：`del` / `clear` / `release` 通过 `dictType` 的 `keyDestructor`
  （`zfree`）释放，每个存活条目恰好一份。
- 键按 C 字符串处理（`strlen` / `strcmp`），**不允许内嵌 `0x00`**。
- **覆盖只替换值，键指针保持首次插入的那份副本**（Q1 裁决：跟随上游 `dictReplace`）。
  由于键是表持有的副本，这个指针不会悬垂；旧值指针被替换但不释放，值仍归调用方。
- `DictIntPtr` 的键同样由表持有：shim 写进一个 4 字节键槽（`zmalloc(4)`），
  `keyDestructor` 用 `zfree` 回收。
- **上游查找的边界（Q4，S2 实测）**：`dictFind` 用查询字节重算哈希、按字节比较，不比较
  条目里冻结的哈希。若某天存储键被原地改写，旧字节必不可达，而新字节在掩码桶相同时仍可能
  命中。本包的公开 API 只暴露表持有的副本，调用方无法制造这种状态；此条只作为内部语义记录。

### 3.3 值：借用

- `void *` 值按指针保存，`clear()` / `release()` **不会**释放 pointee。
- 值可以为 `0`；`get` 用状态码表达"找到/未找到"，不靠空指针判定。
- 需要容器托管 pointee 时，调用方在清理前自行释放；本包不提供 `valDestructor` 钩子。

### 3.4 迭代器

- `iter()` 返回 safe iterator，内部持有 `dictIterator *` 与当前 `dictEntry *`；
  `release()` 必须调用，`release(0)` 空操作。
- `key()` / `value()` 借用当前条目，`next()` 或 `removeCurrent()` 之后失效。
- `removeCurrent()` 之后 `key()` / `value()` 返回 0 / 空，直到下一次 `next()`。

---

## 4. 对外契约

**链式说明（裁决 D-1，2026-11-08）**：`put` / `clear` / `reserve` 返回接收者，按 ct-pkg
默认的 OO / 链式准则执行。`map-options.md` §4.3 / §7 的"不做链式"是 ct 仓写的规划文字，
与 ct-pkg 的默认准则冲突时以 ct-pkg 为准；这不是笔误。

状态码（`DictTypes.h.ct`）：

| 常量 | 值 | 含义 |
| --- | ---: | --- |
| `DICT_OK` | 0 | 成功 |
| `DICT_NOT_FOUND` | 1 | 键不存在 |
| `DICT_INVALID` | 2 | 空键 / 空句柄 / 扩容被拒 |

### 4.1 `namespace dict`（`Lib`）

| 方法 | 签名 | 说明 |
| --- | --- | --- |
| `newStrPtr` | `DictStrPtr *newStrPtr()` | 建 `char *` → `void *` 表；失败即 abort（见 §6） |
| `newIntPtr` | `DictIntPtr *newIntPtr()` | 建 `int` → `void *` 表 |

两形状共用进程级哈希种子：首次建表时用 `mt19937-64` 播种（与 Redis 启动流程一致），
不暴露 setter。测试不依赖桶序。

### 4.2 `DictStrPtr *`

| 方法 | 签名 | 说明 |
| --- | --- | --- |
| `put` | `DictStrPtr *put(const char *key, void *value)` | 复制键；插入或覆盖（覆盖只换值，保留首个键副本）；返回接收者 |
| `get` | `int get(const char *key, void **out)` | `out` 可为 0，只判存在性 |
| `del` | `int del(const char *key)` | 删除条目并释放键副本；返回 `DICT_OK` / `DICT_NOT_FOUND` |
| `len` | `int len()` | 条目数，上限 2^31-1 |
| `clear` | `DictStrPtr *clear()` | 删条目并释放键副本，不释放值；返回接收者 |
| `reserve` | `DictStrPtr *reserve(int buckets)` | 桶数向上取 2 的幂；逻辑拒绝 → `DICT_INVALID`；真实 OOM → abort（§6） |
| `status` | `int status()` | 最近一次数据 / 扩容操作的状态 |
| `iter` | `DictStrPtrIter *iter()` | 新建 safe iterator |
| `release` | `void release()` | 释放表与键副本；不释放值 |

### 4.3 `DictStrPtrIter *`

| 方法 | 签名 | 说明 |
| --- | --- | --- |
| `next` | `int next()` | 1 = 有当前条目，0 = 结束 |
| `key` | `char *key()` | 借用当前条目的键副本 |
| `value` | `void *value()` | 借用当前条目的值 |
| `removeCurrent` | `int removeCurrent()` | 删除当前条目，返回 `DICT_OK` / `DICT_NOT_FOUND`；之后仍可 `next()` |
| `release` | `void release()` | 释放迭代器 |

### 4.4 `DictIntPtr *` / `DictIntPtrIter *`

与 §4.2 / §4.3 同构，签名差异：

- `put(int key, void *value)`；
- `get(int key, void **out)`；
- `del(int key)`；
- `IntPtrIter.key()` 返回 `int`。

---

## 5. 迭代与删除

- 使用 upstream `dictGetSafeIterator` / `dictNext` / `dictReleaseIterator`
  （`src/dict.h`）。safe iterator 在返回当前条目**之前**就预取了下一项，
  因此 `removeCurrent()` 不会打断遍历。
- 支持的模式：

  ```
  for (it = d.iter(); it.next(); ) {
      if (drop(it.value())) it.removeCurrent()
  }
  ```

- `removeCurrent()` 只删当前条目；**不承诺**在遍历中用 `del()` 删除其它键（v1 未测试，
  行为按上游可能可用，但不写进契约）。
- 遍历中 `put()`：上游 safe iterator 允许新增键；v1 不承诺"新键一定被本次遍历看到"，
  文档只保证"不会崩溃、不会漏掉遍历开始前已存在的键"。
- 迭代器不自动释放；`next()` 返回 0 后仍需 `release()`。

---

## 6. 失败与 OOM 语义

**两个包共同的契约**：`dict` 与 `hashmap` 的写入路径都不提供可恢复的 OOM，内存不足即
进程终止。`hashmap` 一侧的检出点与文案见 `hashmap/docs/design.md` §6（每次 `put` 后查
`hashmap_oom()`，置位即 `abort`）。这让调用方在两者之间迁移时"写失败怎么办"的答案保持
不变：不办，因为进程已经不在了。

上游事实：

- `dict.c` 的分配全部走 `zmalloc` / `zcalloc` / `zrealloc`，而这些分配点**没有** `NULL`
  检查；`zmalloc` 的默认 OOM handler 是打印后 `abort()`。
- 唯一的非终止分配入口是 `dictTryExpand()` 内的 `ztrycalloc`：失败返回 `NULL` 而不调用
  handler。它还丢弃 `_dictExpand` 的返回值，未扩容也报 `DICT_OK`（Q3）。
- 因此"返回 `NULL` 让调用方降级"不是抽取层的可选项，除非逐路径改写上游。

本包契约：

1. **OOM = 进程终止（abort）**，与 Redis 及 `hashmap` 一致。这是两个包共同的对外契约
   （README §5.5 与排错表）：内存不足时调用方拿不到错误码，进程会退出。
2. 返回值只表达逻辑失败：键不存在（`DICT_NOT_FOUND`）、参数非法（`DICT_INVALID`）。
3. `reserve()` 按成因分化（Q2）：先复刻 `_dictExpand()` 的逻辑拒绝条件（正在 rehash /
   `ht_used[0] > buckets` / 同一 2 的幂 / 尺寸溢出），命中即返回 `DICT_INVALID`，不触碰
   分配器；条件都不成立才调用 `dictTryExpand()`，此时失败就是真正的分配失败，走同一
   `zmalloc` OOM handler 终止。调用后用 `dictSlots()` 差分兜底（Q3）。
4. 不提供 OOM handler 注册：CT 没有闭包，注册 C 函数指针等于把 C 类型泄露到公开面；
   需要自定义 allocator 的场景不在 v1。
5. 若将来要软失败，必须同时做两件事：把 `zmalloc` 换成返回 `NULL` 的配置，并逐个改写
   `dict.c` 的分配点。这是一次独立切片，不在 S2。

> **裁决 D-2（2026-11-08，已定）**：接受 OOM = abort，并把它写成**两个包共同的**对外契约
> （README §5.5 与本节第 1 条；`hashmap` 同步采用，见其 §6）。理由是上游分配点没有 `NULL`
> 分支，软失败只是把错误变得更晚、更隐蔽；`hashmap` 虽有 `hashmap_oom()` 可检，但链式
> `put` 没有错误通道，检出后同样只能静默丢失。软失败若要做，是独立切片，不在 S0 讨论。

---

## 7. shim 数量估算

设每种形状的公开方法数为 14（句柄 9 + 迭代器 5），`namespace` 构造器 2 个。

| 项 | `DictStrPtr` | `DictIntPtr` | 新增一种形状 |
| --- | ---: | ---: | ---: |
| 包装结构 `ct_dict_*` | 1 | 1 | +1 |
| `dictType` 常量 | 1 | 1 | +1 |
| 哈希函数 | 1（SipHash + `strlen`） | 1（整数哈希） | +1 |
| 比较函数 | 1（`strcmp`） | 1（整数相等） | +1 |
| 键复制 / 释放 | 2（`zmalloc` / `zfree`） | 2（`zmalloc` 4 / `zfree`） | 2 |
| 构造器 | 1 | 1 | +1 |
| 迭代器包装 | 1 | 1 | +1 |
| mapping 行 | 14 | 14 | +14 |

合计（S2 实测）：**42 个 `static inline` 定义**——30 个公开方法实现、4 个哈希/比较、
3 个键复制/回收、5 个共享辅助函数（播种、2 的幂、拒绝判定、OOM 触发、reserve 状态）——
外加 **30 行 mapping**（14×2 + 2 个构造器）、1 个 shim 头与抽取的上游文件（§12）。
新增形状的成本与已有形状数无关：1 个 `dictType` + 1 组 hash/compare + 1 个构造器 +
14 行 mapping。没有泛型，这个成本无法消除。

---

## 8. 与 `std::map` / `hashmap` 的差异（文档第一顺位）

| 维度 | `std::map` | `hashmap` | `dict`（本包） |
| --- | --- | --- | --- |
| 键 | 借用指针 | 深拷贝（strdup） | **表持有副本（S2 裁决，Q1）** |
| 值 | 借用，可配 `free_fn` | 借用，不配 | 借用 |
| 容量 | 定容 | 自动 | 自动 |
| 扩容 | 无 | 一次性搬全表 | 增量，每次操作搬少量桶 |
| 遍历中删除 | 无遍历 | 不支持 | **支持（`removeCurrent`）** |
| OOM | `put` 返回 -1 | 进程 abort（与本包相同） | **进程 abort** |
| 额外依赖 | 无 | 单文件 | 9 个上游文件 + 3 个本地适配头 |

---

## 9. 明确不做

- 不做统一 `Map` trait / 抽象层（所有权语义不同，且无泛型）。
- 不做自动析构 / `Drop`。
- 不做值拷贝；键的拷贝是固定的内部行为（§3.2），不提供 `keyDup` / `valDup` 可配置入口
  （要连值一起托管就用 `hashmap`）。
- 不做线程安全；`dict` 与 Redis 一样由调用方加锁。
- 不暴露 `dictScan`（回调式，CT 无闭包）。
- 不暴露 `dict_can_resize` / `dictPauseRehashing` / `dictSetResizeEnabled`（进程级全局，
  会跨表互相干扰）。
- 不提供 OOM 软失败与自定义 allocator。
- 不做 `dict_can_resize` 的按表配置、不做 `dictGetRandomKey` / `dictGetFairRandomKey`。
- 不修改上游源码（除新增一个本地 `redisassert.h` 适配头；任何修改都要在 S2 记录并复核）。

---

## 10. 切片与验收条件

### S2.0 · 上游抽取探针

- 冻结 `7.2.4` 快照，按 §12 清单拷入 `third_party/`，只补本地 `redisassert.h` 适配头。
- 逐个文件核对 include 闭包（`zmalloc.c` 可能牵出 `config.h` / `atomicvar.h`，
  `dict.c` 依赖 `fmacros.h`）；缺什么补什么，但补的每个文件都要在
  `third_party/README.md` 写明来源与理由。
- 用 `cc -Wall -Wextra` 单独编译 `dict.c`，零告警。

验收：`third_party/` 里每个文件的 sha256 与 `tools/third_party.sha256` 一致；
`COPYING` 与源文件头逐字保留。

### S2.1 · `DictStrPtr` 核心

- 落地 `newStrPtr` / `put` / `get` / `del` / `len` / `clear` / `reserve` / `status` /
  `release`。
- 测试：插入、覆盖、删除、清空、重复 `release(0)`；覆盖后 `key()` 返回首次插入的
  **表持有副本**（不是调用方缓冲区）；调用方改内容或释放缓冲区后仍可查；副本在
  `del` / `clear` / `release` 时配平释放。
- 用测试夹具把 `zmalloc` 的 OOM handler 换成固定文案后 `abort`，在子进程注入分配失败，
  断言 `put` / 构造器终止；同时断言 `reserve` 的逻辑拒绝不终止、真正的分配失败终止。

验收：`tests/run.sh` 全绿，`-Wall -Wextra` 零告警。

### S2.2 · `DictIntPtr`

- 覆盖 `int` 键槽的分配与回收；用堆分析/自建计数验证 `clear()` / `release()` 后
  键槽全部释放（每个条目一次 `zmalloc`，必须精确配平）。

### S2.3 · safe iterator

- 遍历全量断言"恰好访问每个键一次"。
- `removeCurrent()` 删除一半条目后继续遍历不崩、剩下的键仍可查。
- 断言 `removeCurrent()` 后 `key()` / `value()` 不再返回旧条目。
- 遍历中 `put` 新键：不崩、旧键不漏（不承诺新键可见）。

### S2.4 · 增量 rehash

- 预置 10 万条，边插边查；断言 rehash 期间 `len` 单调、旧键始终可读。
- 用 `reserve()` 预扩容后批量插入，验证与不预扩容结果一致。

### S2.5 · 文档与安装态

- README 10 节骨架填满；所有代码块实测后写入，输出逐字一致。
- `ct get` / `ct install` 安装态验证：`third_party/` 被完整拷贝（目录名不能是 `vendor`），
  `ldflags` 的 4 个 `.c` 都能找到。
- 未发布：`index/packages.txt` 保持注释，不打 tag、不 push（Q1–Q4 落地后的状态）。

---

## 11. 风险与待验证项

| # | 风险 | 处置 |
| --- | --- | --- |
| R-1 | 调用方缓冲区被改写 / 释放 → 条目悬垂 | 已改为表持有键副本（§3.2）；S2.1 用"改内容 / 释放后仍可查"与配平释放验证。上游原地改存储键的边界见 Q4，公开 API 不可达 |
| R-2 | `int` 键槽泄漏（漏 `keyDestructor`） | S2.2 计数配平；注入变异证明门禁会红 |
| R-3 | OOM 终止进程，调用方无法降级 | §6 明示为与 `hashmap` 的共同契约；需要软失败时另开切片 |
| R-4 | include 闭包比 `map-options.md` 列的更大 | S2.0 逐文件核对，`third_party/README.md` 记录 |
| R-5 | 许可边界：误用 7.4+/8.x | `ct.pkg` 与 README 双处写死 7.2.x；`third_party/README.md` 记 tag |
| R-6 | `redisassert.h` 的 `_serverAssert` 符号缺失 | S2.0 提供本地适配头（`abort` + 打印），不改上游 |
| R-7 | 进程级哈希种子未播种 → 可预测 | S2.0 首次建表时用 `mt19937-64` 播种，测试不依赖桶序 |
| R-8 | `pub type` 依赖 ct 0.20.3，未在本包实测 | S2.0 探针；失败则退回 shim typedef + 0.20.0 |

---

## 12. 上游清单与许可

从 Redis `7.2.4` 的 `src/` 抽取（S2 已落地到 `third_party/`）：

| 文件 | 理由 |
| --- | --- |
| `dict.c` / `dict.h` | 主体 |
| `siphash.c` / `siphash.h` | `dictGenHashFunction` |
| `zmalloc.c` / `zmalloc.h` | 分配器 |
| `mt19937-64.c` / `mt19937-64.h` | `dict.h` 直接包含，哈希种子随机源 |
| `redisassert.h` | `dict.c` 的断言；本地提供适配实现，不拷服务端依赖 |
| `fmacros.h` | `dict.c` 的第一个 include |

许可：BSD-3-Clause（`COPYING`，`Copyright (c) 2006-2020, Salvatore Sanfilippo`）。
必须保留 `COPYING`、每个源文件的版权头，README 注明"本包不是 Redis 官方发行版，
未获 Redis 背书"。7.4+ 与 8.x 不得引入。

S2.0 未验证项，已在 S2 逐条核对（结论以实测为准）：

- `zmalloc.c` 的 include 闭包：需 `fmacros.h`、`atomicvar.h`，以及空的 `config.h` /
  `solarisfixes.h`；补 `-Wall -Wextra` 后零告警（`tools/check.sh` 每次重编）。
- `dict.c` 只依赖 `dict.h` / `zmalloc.h` / `redisassert.h` / `fmacros.h`，**不**依赖
  `util.h` / `server.h` / `_serverAssert`；本地 `redisassert.h` 适配头足够（§12 表）。
- `dictNext` 在 `removeCurrent` 后继续遍历：S2.3 实测安全（safe iterator 先预取），
  `tests/iterator.ct` 343 条断言覆盖；契约见 §5。
