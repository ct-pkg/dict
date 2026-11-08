# dict 实现说明（S2）

本文件记录 `dict` 包从 S0 设计到可编译 / 可测 / 可交付实现的过程：上游来源、include
闭包、适配层、设计裁决问题、OOM 分支处置，以及每条门禁的变异测试证据。需求以
[docs/design.md](design.md) 为准。

## 1. 交付状态

| 项 | 结果 |
| --- | --- |
| 编译 | `ct build` 通过；`-std=c11 -Wall -Wextra` 与 ct 的 `-std=c99 -Wall -Wextra` 均零警告 |
| 安装布局 | 真实 `ct get` / `ct install` 复制后构建、运行 demo，输出逐字匹配 |
| CT 测试 | `unit` 68、`iterator` 343、`rehash` 16、`chain` 17，共 444 条断言 |
| C 门禁 | `slots.c` 分配账目 + 增量 rehash 行为（1536）；`oom.c` 四路可注入 OOM（4）；`gen.c` ASan generation 守卫 + `pauserehash` 恢复（12）；`multi_tu` 双 TU 播种门禁（20 次） |
| 静态门禁 | `tools/check.sh` 共 91 项；`tests/run.sh` 另设每套件断言数下限（§7 M23） |
| 发布 | 未打 tag、未 push；`index/` 未改；交付物已提交（S1） |

## 2. 上游来源与完整性

- 上游：`redis/redis` tag `7.2.4`，`src/` 下的 dict 实现。
- tarball：`https://download.redis.io/releases/redis-7.2.4.tar.gz`
  sha256 `8d104c26a154b29fd67d6568b4f375212212ad41e0c2caa3d66480e78dbd3b59`。
- 快照位置：`third_party/`（**不是** `vendor/`：`ct get` 的 `copy_tree` 与 `sha_collect`
  会静默跳过名为 `vendor/` 的目录，安装后依赖会消失）。
- 文件表与逐文件来源见 `third_party/README.md`；完整性清单位于
  `tools/third_party.sha256`（13 个文件，含 3 个本地适配头与 `COPYING`）。
- 许可：Redis 7.2.x 为 BSD-3-Clause。7.4+（RSALv2/SSPL）与 8.0+（AGPLv3）不在允许范围。
  `COPYING` 与源文件版权头逐字保留；`tools/check.sh` 扫描许可边界与 `COPYING` 文本。

### 2.1 文件与取舍

| 文件 | 来源 | 为什么需要 |
| --- | --- | --- |
| `dict.c` / `dict.h` | 上游 `src/` | 表、rehash、safe iterator 本体 |
| `siphash.c` | 上游 `src/` | `dictGenHashFunction` 的默认哈希 |
| `zmalloc.c` / `zmalloc.h` | 上游 `src/` | 上游分配器与 OOM handler；`zmalloc_used_memory()` 供账目测试 |
| `mt19937-64.c` / `mt19937-64.h` | 上游 `src/` | `dict.c` 的随机种子 |
| `fmacros.h` / `atomicvar.h` | 上游 `src/` | `zmalloc.c` 的 include 闭包 |
| `COPYING` | 上游 | 许可原文 |
| `redisassert.h` | 本地适配 | 上游版本依赖 `server.h` / `_serverAssert`；这里提供自包含的打印 + `abort()`，行为与进程终止契约一致 |
| `config.h` | 本地适配 | 空文件，满足 `zmalloc.c` 的 include |
| `solarisfixes.h` | 本地适配 | 空文件，满足 `zmalloc.c` 的 include |

适配头不是上游文件，但同样纳入 sha256 清单；`third_party/README.md` 逐个说明原因与范围。

## 3. include 闭包与构建

`third_party/` 的最小闭包是 9 个上游文件 + 3 个适配头。`dict.c` 只依赖 `dict.h`、
`zmalloc.h`、`redisassert.h`、`fmacros.h`；不引入 `server.h`、`config.h` 的服务器配置。
构建命令：

```sh
ct build -I vendor/dict \
  --cflags '-I vendor/dict/third_party' \
  --ldflags 'vendor/dict/third_party/{dict.c,siphash.c,zmalloc.c,mt19937-64.c} vendor/dict/dict/seed.c' \
  yourapp.ct -o yourapp
```

`seed.c` 必须与上游四个源文件一起链接：哈希种子只能有一个进程级闩锁，把它放进
shim 头会变成每个 TU 一份（B1）。文件名保持短（`seed.c`）：ct 对 `ct.pkg` 的每个
元数据值有 160 字节上限，这条 `ldflags` 已是 159 字节。

`ct.pkg` 的 `cflags` / `ldflags` 使用消费方根目录相对路径（`vendor/dict/third_party/...`），
与 `ct get` 的安装布局一致；`tests/run.sh` 用包内绝对路径，因此包树内也能直接跑。

## 4. 公开面与实现映射

- 4 个 `pub type` 别名：`DictStrPtr`、`DictIntPtr`、`DictStrPtrIter`、`DictIntPtrIter`；
  别名不生成 C typedef，星号由源码手写。
- 5 个 trait：`Lib`（2 个构造器）、`StrPtrDict` / `StrPtrIter`、`IntPtrDict` / `IntPtrIter`
  （各 9 + 5 个方法），共 30 个方法与 30 行 mapping。
- `put` / `clear` / `reserve` 返回接收者，支持链式；`get` / `del` / `len` / `status` 返回
  状态或计数。
- `status()` 按裁决保留：只表达 `DICT_NOT_FOUND` / `DICT_INVALID`，不是 OOM 通道。
  `tools/check.sh` 固定“2 条声明 + 2 条 mapping”，删除即红（§7 M17）。

`DictStrPtr` 的键策略：`put` 先 `dictFind`；命中只 `dictSetVal`（不分配、不复制），新键才
`ct_dict_str_key_dup()`（`zmalloc(strlen + 1)` + `memcpy`）并 `dictAdd`，由
`dictType.keyDestructor` 回收。

`DictIntPtr` 的键槽策略：命中已有键时先 `dictFind`，再 `dictSetVal` 原地改值，只有新键才
`zmalloc(4)` 分配键槽。这样每个存活条目恰好一个键槽。直接复用上游 `dictReplace` 会产生两
个问题（§7 M1 实测）：命中时新分配的键槽被丢弃（每覆盖一次泄漏一个槽），并且
`dictReplace` 的 `dictEntryFree` 会对“旧条目副本”调用 `keyDestructor`，把仍在用的键槽
`zfree` 掉。

## 5. 设计裁决问题（Q1–Q4 已裁决并落地）

四处规格与上游行为的偏差在实现中暴露。裁决结果与落地方式如下，设计文字已同步到
[docs/design.md](design.md)。

### Q1：覆盖已有键时，键指针是否替换？——跟随上游，且键改为表持有副本

- 原设计写"旧键指针与旧值指针直接被替换"；上游 `dictReplace` 只调用 `dictSetVal`，
  **保留第一次插入的键指针**，只有值被替换。
- 裁决：跟随上游。设计改为"覆盖只替换值，键指针保持首次插入的那个"。
- 同时确认所有权：S0 的 shim 借用调用方指针。在"保留旧指针"的覆盖语义下，调用方释放
  或改写缓冲区就会让条目悬垂，因此**已改为复制后持有**：
  - `DictStrPtr`：`ct_dict_str_key_dup()` 用 `zmalloc(strlen + 1)` + `memcpy` 复制；
    `dictType.keyDestructor` 用 `zfree` 回收。`put` 先 `dictFind`，命中只 `dictSetVal`，
    新键才复制并 `dictAdd`；`dictAdd` 失败时立即 `zfree` 副本。
  - `DictIntPtr`：新键写进 4 字节键槽，`keyDestructor` 回收（原实现已如此）。
- 证据：`tests/unit.ct` 断言"同字节、不同地址的两个键是同一个条目"、"迭代器返回的键
  不是调用方缓冲区"、"改写调用方缓冲区后旧字节仍可查"、"改写后的缓冲区成为另一个键"；
  `tests/slots.c` 用短键 / 长键差分证明键确实被复制（§8）。

### Q2：`reserve()` 的失败如何分化？——逻辑拒绝 INVALID，真实分配失败 abort

- 上游事实：`dictTryExpand` 走 `ztrycalloc`，失败返回 `NULL` 而不触发 OOM handler；
  它是全包唯一的非终止分配入口。
- 裁决：按成因分化，不用一个码吞掉两种失败。
- 落地：`ct_dict_reserve_status()` 先自行判断逻辑拒绝条件——
  `dictIsRehashing(d)`、`d->ht_used[0] > buckets`、目标桶数不是更大的 2 的幂、
  `target * sizeof(entry *)` 溢出——命中即返回 `DICT_INVALID`，不触碰分配器。
  条件全部通过后才调用 `dictTryExpand`；此时 `DICT_ERR` 只可能是真正的分配失败，
  由 `ct_dict_abort_oom()`（`zmalloc(SIZE_MAX)` 触发同一 handler，再 `abort()` 兜底）
  终止进程。
- 证据：`tests/oom.c` 四路——armed `put` / armed constructor / armed `reserve` 均
  `SIGABRT` + `CT_DICT_OOM`；未 armed 的重复 `reserve(64)` 返回 `DICT_INVALID` 且进程
  正常退出（退出码 7）。M22 删除 abort 分支后 armed `reserve` 那一路变红（§7）。

### Q3：`dictTryExpand()` 的返回值不可靠，如何判定扩容成功？——保留差分，不改上游字节

- 上游事实（`third_party/dict.c` 第 280–283 行）：

  ```c
  int dictTryExpand(dict *d, unsigned long size) {
      int malloc_failed;
      _dictExpand(d, size, &malloc_failed);
      return malloc_failed? DICT_ERR : DICT_OK;
  }
  ```

  `_dictExpand` 在"桶数落在同一 2 的幂 / `ht_used > size` / 正在 rehash 被拒"时返回
  `DICT_ERR`，但 `dictTryExpand` 丢弃该返回值，只报告 `malloc_failed`。因此它会在没有
  扩容时返回 `DICT_OK`。这是**上游缺陷**，不是本包的封装选择。
- 裁决：保留差分判定，不改上游字节。
- 落地：逻辑拒绝已经由 Q2 的前置判断覆盖；调用后再比较 `dictSlots()` 前后值，桶数确实
  变大才 `DICT_OK`，否则 `DICT_INVALID`。这样既不信任上游返回值，也不破坏
  `third_party/` 的逐字快照与 sha256 门禁。
- 证据：unit 的 `reserve` 断言（同 exponent、低于 `ht_used` 均须 INVALID）；M3 变异
  （恒返回 `DICT_OK`）让 2 条 unit 断言与 `oom` 的拒绝用例一起变红。

### Q4：修改键内容后，"新字节查不到"不是稳定契约——按实测改设计

- 原设计写"修改键内容后条目永久失联"，unit 原断言"新字节必 miss"。
- 上游事实：`dictFind` 用**查询字节**重新计算哈希、定位桶，再用 `dictCompareKeys` 比较
  字节；它不比较条目里冻结的哈希。若新字节与旧字节落在同一个掩码桶，查询新字节会命中。
  初始 4 桶时该概率约 1/4。
- 实测：该断言在变异跑中实际出现过偶发失败（`FAIL mutated key: new bytes miss
  (hash was frozen)`）。因此改写为只断言确定性的方向——**旧字节必不可达**、条目仍在。
- 裁决与落地：设计改为"旧字节必不可达；新字节是否可达取决于当前桶掩码"。Q1 落地后
  公开 API 只暴露表持有的副本，调用方无法制造"原地改写存储键"的状态，所以此条只作为
  内部语义记录，不再出现在对外契约里。
- 证据：修复后连续 10 次 `tests/run.sh` 全绿（§8）。

### Q5（评审修复）：B1 播种、B2 rehash 行为、R1 迭代器 generation

- **B1 哈希种子按 TU 重复播种**：原 shim 头用 `static int ct_dict_seeded` 闩锁，但
  `dictSetHashFunctionSeed()` 写的是 `dict.c` 的进程级全局；每个 `.ct` 是独立 TU，各持
  一份闩锁，第二个 TU 建表就重新随机，先前插入的键静默 `DICT_NOT_FOUND`。修复：播种
  移入 `dict/seed.c`（`void ct_dict_seed_once(void)`，函数级 `static` 闩锁），头文件
  只留声明，`ct.pkg` / `tests/run.sh` / README 构建命令都链接该单元。门禁：`tests/multi_tu_a.c`
  + `multi_tu_b.c` 双 TU，连续 20 次回读；`tools/check.sh` 同时禁止 shim 出现任何非
  `static inline` 的有状态定义（旧的 seed 白名单已删除），并固定“声明在头、定义在 seed 单元”。
- **B2 增量 rehash 零行为覆盖**：原 7 套测试对 `dictRehash(d, 1)` 无感知。修复：
  `tests/slots.c` 增加 5 条断言——加载 1000 条后 `reserve(65536)` 必须进入 rehash
  （`dictIsRehashing` 且 `rehashidx >= 0`）、`rehashidx` 单调不减、足够操作后结束
  （`!dictIsRehashing` 且 `ht_used[1] == 0`）、rehash 进行中 `reserve()` 返回
  `DICT_INVALID`；`tests/rehash.ct` 增加“rehash 中途创建迭代器”场景（1000 条 + reserve，
  再 put 一条后立刻 `iter()`，必须看到 1001 条）。变异 M24 把步进改为 0 后 slots 变红。
- **R1 遍历中 `clear()` / `release()` 的 UAF**：句柄新增引用计数的 generation 计数器；
  `clear()` / `release()` 自增，迭代器记录创建时的值，`next()` / `key()` / `value()` 发现
  不一致即返回 0；`release()` 置 dead，迭代器释放时不再对被释放的 dict 调
  `dictReleaseIterator`。`clear()` 走 `dictEmpty`，它把 `pauserehash` 复位为 0；失效
  迭代器若仍调 `dictReleaseIterator`，`dictResumeRehashing` 会把它减成 -1，而
  `_dictRehashStep` 要求精确等于 0，这张表就永久停在 rehash 中、`reserve()` 永久
  `DICT_INVALID`。修复：`iter_release` 的条件是 `gen->dead || !gen_valid(...)`，
  命中时只 `zfree(it->it)`。语义写入 README §9。门禁：`tests/gen.c` 在 ASan 下 12 条
  断言（含 `pauserehash == 0`、rehash 完成、`reserve()` 恢复）；变异 M26 删除
  generation 比较后 ASan 报 heap-use-after-free，变异 M30 只留 `gen->dead` 判断时
  `pauserehash` 停在 -1、rehash 永不完成、`reserve()` 失败。

## 6. OOM 分支逐条处置

| 分配点 | 上游路径 | 本包处置 |
| --- | --- | --- |
| 包装句柄 `ct_dict_*` | `zmalloc` | 失败即 `zmalloc_default_oom`：打印 + `abort` |
| `dictCreate` 的 `dict` | `zmalloc` | 同上 |
| `dictAdd` 的条目 | `zmalloc` | 同上；`dictAdd` 返回 `DICT_ERR` 只表示重复键 / 逻辑拒绝，不是 OOM |
| `DictStrPtr` 键副本 | `zmalloc(strlen + 1)` | 同上 |
| 自动扩容 / rehash 桶数组 | `zcalloc` | 同上 |
| `iter()` 包装 + `dictGetSafeIterator` | `zmalloc` | 同上 |
| `int` 键槽 | `zmalloc(4)` | 同上 |
| `reserve()` 桶数组 | `ztrycalloc`（`dictTryExpand`） | 逻辑拒绝先行返回 `DICT_INVALID`；真正的分配失败由 `ct_dict_abort_oom()` 触发同一 handler（Q2） |

结论：shim 没有“返回 0 / NULL 让调用方降级”的静默路径。构造器、迭代器、键副本、键槽的
分配失败都会终止进程；`reserve()` 只把逻辑拒绝映射成 `DICT_INVALID`，真实 OOM 仍终止。
M14 变异把构造器契约改成“返回 0”，`oom.c` 立即变红；M22 删除 `reserve` 的 abort 分支，
`reserve OOM aborts` 立即变红（§7）。

## 7. 门禁清单与变异证据

`tools/check.sh` 的静态 / 行为检查（每个门禁类别都至少有一条“注入缺陷 → 变红”记录）：

| # | 检查 | 注入的缺陷 | 实测变红信息 |
| ---: | --- | --- | --- |
| M1 | 分配账目（覆盖不泄漏键槽） | `int put` 改用 `dictReplace` | `FAIL overwriting an int key allocates nothing (no slot leak)` 等 7 条 |
| M2 | shim 必须 `#undef DICT_OK/DICT_ERR` | 删除 `#undef DICT_OK` | `FAIL ... DICT_OK/DICT_ERR must be undefined` |
| M3 | `reserve` 状态必须反映真实结果 | 两个形状恒置 `DICT_OK` | `FAIL reserve to the same exponent is DICT_INVALID`、`reserve below ht_used ...` |
| M4 | 编译警告即失败 | 使 `before` 赋值后未使用 | `FAIL unit: build emitted warnings` + `-Wunused-but-set-variable` |
| M5 | 迭代器必须是 safe iterator | `dictGetSafeIterator` → `dictGetIterator` | `FAIL iterator: must use dictGetSafeIterator and never dictGetIterator`；iterator 套件同时失败 |
| M6 | `int` 键槽必须由 `keyDestructor` 回收 | 删除 `.keyDestructor` 初始化 | slots 6 条：`500 deletes free entry and slot bytes` 等 |
| M7 | README 不得泄漏 C 类型 | 在 README 加 `dictEntry *e;` | `FAIL README C type leak` |
| M8 | README 恰好 10 节且顺序固定 | 追加第 11 节 | `FAIL README sections: expected 10, found 11` + 顺序 diff |
| M9 | `third_party/` 逐字快照 | 向 `dict.c` 追加一行注释 | `third_party/dict.c: FAILED`（sha256） |
| M10 | `dict.c` 只用 `zmalloc` 族 | 加 `(void)malloc(1);` | `FAIL allocator discipline: dict.c calls a raw allocator` |
| M11 | 公开面不得有游离 `pub fn` | 追加 `pub fn bogus();` | `FAIL no free pub fn: every public method must have a type host` |
| M12 | trait 与 mapping 一一对应 | 给 trait 加一个无 mapping 的方法 | `FAIL trait/mapping mismatch: see diff` |
| M13 | 1k vs 10k 差分（每插入 64 B 泄漏） | 对 `key >= 5000` 每次插入 `zmalloc(64)` 不释放 | `FAIL 1k vs 10k differential: per-entry bytes are constant`、`clear() leaves the same residual` |
| M14 | OOM 契约：构造器必须 abort | 构造器改 `ztrymalloc` 并返回 0 | `FAIL constructor OOM aborts: expected signal 6, status 0x0` |
| M15 | OOM 注入必须覆盖 `calloc` | 让 `calloc` 拦截失效 | `FAIL reserve OOM aborts: expected signal 6, status 0x0`（分配成功，子进程正常退出） |
| M16 | `third_party/` 不能被 `vendor/` 替代 | 目录改名为 `vendor/third_party` | `FAIL no vendor/ directory` + `FAIL ct get copy: third_party/ did not survive the install copy` |
| M17 | `status()` 必须保留 | 删除 2 条声明 + 2 条 mapping | `FAIL status() retention: declarations=0 mappings=0, expected 2 and 2` |
| M18 | trait 签名不得出现 C 类型 | 加 `dictEntry *raw();` | `FAIL C type leak: C names in the CT surface` |
| M19 | 术语表必须是三列表格 | 表头删掉第三列 | `FAIL glossary table: header must be | 术语 | 含义 | 本包中的所指 |` |
| M20 | `requires` 下限必须真实 | 改成 `ct >= 0.19.0` | `FAIL requires: must be ct >= 0.20.3` |
| M21 | 包不得依赖 `std` | 追加 `dep = std` | `FAIL dep: this package has no package dependencies` |
| M22 | `reserve()` 的真实分配失败必须 abort | 把 `ct_dict_abort_oom()` 换成 `return CT_DICT_INVALID;` | `FAIL reserve OOM aborts: expected signal 6, status 0x0` |
| M23 | 每套件断言数不得低于实测下限 | 把 `unit.ct` 的 `check()` 掏空（只让前 2 次计数） | `FAIL unit: expected at least 68 assertions, got 2` |
| M24 | 增量 rehash 必须真的步进 | `third_party/dict.c` 里 `dictRehash(d,1)` → `dictRehash(d,0)` | `FAIL reserve starts an incremental rehash`（rehash.ct，passed=15/16）；`FAIL rehash completes once enough operations run`（slots，passed=1535/1536） |
| M25 | 播种必须只有一个进程级闩锁 | 把 `ct_dict_seed_once` 改回 shim 内的 `static inline` + 函数级 `static` 闩锁（种子取各自 TU 的地址，确定性不同） | `FAIL multi_tu: run 1 lost keys across translation units` |
| M26 | 迭代器必须检查 generation | 删除 `iter_next` / `key` / `value` 里的 `ct_dict_gen_valid` 比较 | ASan：`heap-use-after-free ... in dictNext`，`FAIL gen: ASan reported a problem` |
| M27 | README 代码块必须编译且输出正确 | 把 README §3 的 `printf("len: ...")` 改成多打印 1 | `FAIL README block 2 output: see diff`：`len: 3` → `len: 4`、`len: 2` → `len: 3` |
| M28 | 交付物必须被 git 跟踪 | 在干净副本里 `git rm --cached dict/seed.c` | `FAIL git delivery: delivered files are not tracked: dict/seed.c` |
| M29 | shim 不得有有状态定义 | 在 shim 顶部加 `static int ct_dict_seeded = 0;` | `FAIL shim state: non-static-inline definition or function-local static`（`59: static int ct_dict_seeded = 0;`） |
| M30 | 失效迭代器释放不得把 `pauserehash` 减成 -1 | 两处 `iter_release` 的条件改回只判断 `it->gen->dead` | `FAIL clear() + iterator release leaves pauserehash at 0`、`FAIL rehashing still completes after clear()`、`FAIL reserve() works after clear()`（`GEN passed=9 failed=3`） |
| M31 | 制表符不能绕过 shim 状态检查 | 在 shim 顶部加 `static<TAB>int ct_dict_probe_latch = 0;` | `FAIL shim state: ...`（`1: static	int ct_dict_probe_latch = 0;`）+ `FAIL shim symbols: unexpected local data symbols: ct_dict_probe_latch` |
| M32 | `#define` 不得展开出 static 状态 | 加 `#define CT_DICT_PROBE_LATCH static int ct_dict_probe_latch = 0;` | `FAIL shim state: non-static-inline definition, function-local static, or static in a #define` |
| M33 | 干净 clone 必须完整（未跟踪文件也算交付） | 新增未跟踪的 `dict/aux.h`，并把 `#include "aux.h"` 提交进 shim（模拟“本地能编、干净 clone 编不了”） | `FAIL untracked files: a clean clone would miss: dict/aux.h` + `FAIL install-layout build`：`vendor/dict/dict/Dict.shim.h:1:10: fatal error: 'aux.h' file not found` |
| M34 | 文本扫描漏掉的写法必须由 nm 兜底 | 把 `static` 单独一行、下一行写 `int ct_dict_nm_probe = 0;`（逐行正则看不到 `static[[:space:]]`） | `FAIL shim symbols: unexpected local data symbols: ct_dict_nm_probe`（文本扫描通过，nm 变红） |

变异均在 `/tmp/dict-mut` 的整包副本上执行，未改动交付树；每条记录来自对应
`/tmp/m*.log` 的实际输出。M4 的“警告即失败”也解释了为什么 shim 全部使用
`static inline`：未使用的静态函数在 `-Wall` 下会报警。M31/M34 说明只靠文本正则不够：
shim-only TU 用 `-femit-all-decls`（clang 专用，gcc 会拒绝该选项，故先用探针探测）强制
把未引用的静态数据也写进目标文件，再由 `nm -a` 只放行两个 `dictType` 表（以及编译器
自己的 `ltmp*` 临时符号）。

## 8. 实测数据

- 测试计数（连续 10 次 `tests/run.sh`，10/10 全绿）：

  ```
  ok   unit (SUMMARY passed=68 failed=0)
  ok   iterator (SUMMARY passed=343 failed=0)
  ok   rehash (SUMMARY passed=16 failed=0)
  ok   chain (SUMMARY passed=17 failed=0)
  ok   slots (SLOTS passed=1536 failed=0)
  ok   oom (OOM passed=4 failed=0)
  ok   gen (GEN passed=12 failed=0)
  ok   multi_tu (20 runs)
  ok   demo
  9 suites, 0 failure(s)
  ```

  四个 CT 套件合计 444 条 CT 断言；`slots` 的 1536 来自循环内的逐条 `check()`，
  是同一序列下可复现的精确值。`tests/run.sh` 对每个套件分别设了下限
  （68 / 343 / 16 / 17 / 1536 / 4 / 12），低于下限即红（M23）。

- 分配账目（`tests/slots.c`，`zmalloc_used_memory()` 精确值，不是阈值）：

  ```
  SLOTS 1k=48000 B 10k=480000 B reserved=131072 B residual1=0 B residual10=0 B
  SLOTS str short=48000 B long=160000 B delta=112000 B
  ```

  即每条 `int` 条目 48 B：24 B 的 `dictEntry` 经 malloc 取整到 32 B，4 B 的键槽取整到
  16 B，32 + 16 = 48 B（macOS `malloc_size()` 的 usable size；其它平台取整规则不同，
  差分判据不变）。16384 个桶的桶数组 131072 B；清空后残差 0。差分判据要求
  `10k == 10 × 1k` 且两次清空残差相等，能把 64 B/次的小泄漏放大成 320 KB 的差异；
  固定 16 MB 阈值对此不敏感。注意 `reserve=131072 B` 是桶数组的**字节数**，与
  `d.reserve(131072)` 的**桶数**不是同一单位。

  字符串段落：1000 个约 4 B 的短键占 48000 B，1000 个 120 B 的长键占 160000 B，
  差 112000 B（每键约 112 B = 120 B 副本按 usable size 计）。若键是借用的，两者应当相同；
  这个差分就是“键被复制”的直接证据。覆盖同一个键不新增分配，`clear()` 后回到句柄基线。
- OOM 四路：armed `put` abort、armed `reserve(1024)` abort、armed 构造器 abort（均
  `SIGABRT` + `CT_DICT_OOM`），未 armed 的重复 `reserve(64)` 不 abort 且
  `status() == DICT_INVALID`（退出码 7）。
- rehash（`tests/rehash.ct` 的实测输出，供对抗评审复现）：

  ```
  REHASH grow entries=100000 reread_every=1000 reread_ok=1 stride_ok=1 iterated=100000
  REHASH presized reserve=131072 entries=100000 sample_ok=1 iterated=100000
  REHASH equivalent entries=1 iterated=1
  SUMMARY passed=16 failed=0
  ```

  即：10 万条插入全程，每 1000 条回读 `0` / 当前键 / 前一条键均成功；1000 步长的
  100 个采样键全部仍在；迭代器恰好看到 100000 条；预扩容 131072 桶后的同负载结果
  与逐步扩容等价（条目数、迭代数一致）；rehash 中途创建的迭代器看到全部 1001 条。
- demo：`demo/main.ct` 输出与 `demo/expected.out` 逐字 diff 通过。

## 9. 未做与边界

- 未打 tag、未 push、未改 `index/`。
- 未暴露 `dictScan`、进程级 rehash 开关、OOM handler 注册。
- 未新增第三种键值形状；新增需补 `dictType`、哈希/比较、键槽策略与 14 行 mapping。
- Q1–Q4 均已按裁决落地：键复制并归表（覆盖保留首个副本）、`reserve` 按成因分化、
  扩容成功用桶数差分判定、改键可达性按实测限定为内部语义。设计、README 与本文档已同步。
