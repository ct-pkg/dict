# dict

Redis 7.2.x `dict` 的 CT 包封装：**只存指针、增量 rehash、safe iterator**。
与 `hashmap`、`std::map` 并列但语义不同：本包**不拷贝**键值，扩容没有全表停顿，
并且支持遍历中删除当前元素。

> **状态：S0（需求与骨架），尚不可编译、不可安装。**
> `ct.pkg` 已登记，`version` 是声明的首个版本 `0.1.0`，但远端还没有 `v0.1.0` tag；
> `third_party/`、`map` 块与测试在 S2 落地。
> 需求与契约以 [docs/design.md](docs/design.md) 为准。

## 1. 定位与边界

做三件事：

- 只存指针的哈希表：调用方持有键与值的生命周期，表只做索引；
- 增量 rehash：每次操作搬少量桶，十万级以上也不会出现一次性全表停顿；
- safe iterator：`next()` 预取下一项，遍历中可以 `removeCurrent()`。

不做的事：

- **不是 `hashmap` 的替代**：本包不拷贝键值，键必须在表的整个生命周期内保持有效且内容不变；
- **不是 `std::map` 的替代**：本包会分配、会增长，内存不可预先计算；
- 不统一三者接口；不提供自动析构；不是线程安全的；
- **OOM 不做软失败**：内存不足时直接 `abort`（Redis `zmalloc` 的默认行为），与 `hashmap` 同一契约。

场景判据见 [docs/design.md](docs/design.md) §1，选型总表见 `ct/docs/design/map-options.md`。

## 2. 依赖与安装

| 项 | 值 |
| --- | --- |
| 包名 / 版本 | `dict` / `0.1.0`（未打 tag） |
| 许可 | BSD-3-Clause（Redis 7.2.x），见 [LICENSE](LICENSE) |
| 上游 | redis/redis，基线 tag `7.2.4`（`src/`） |
| 编译器 | `requires = ct >= 0.20.3`（依赖 `pub type`，S2 实测确认） |
| 包依赖 | 无 |
| 链接 | `ldflags` 编译 `dict.c` / `siphash.c` / `zmalloc.c` / `mt19937-64.c` |

**未发布状态**：本仓库没有 `v*` tag。`index/index` 无本包行；`index/packages.txt` 中的行
保持注释（`refresh-index.sh` 遇到无 tag 仓库会整体失败）；`tools/repos.txt` 的 `tag` 列写 `-`。
首个 tag 推送时取消 `index/packages.txt` 的注释。

**许可边界**：只允许 Redis **7.2 及更早**（BSD-3-Clause）。7.4+ 是 RSALv2/SSPL，8.0+ 是
AGPLv3，不得引入。`COPYING` 与源文件版权头必须逐字保留；本包不是 Redis 官方发行版。

安装（S2 之后）：

```sh
ct get dict
ct install
```

## 3. 快速开始

目标形态（**尚未编译，S2 落地后替换为实测文件与输出**）：

- 建表：`dict::Lib.newStrPtr()`；
- 写入：`d.put("alice", &alice).put("bob", &bob)`（键值都不拷贝）；
- 读取：`d.get("alice", &p)`，返回 `DICT_OK` / `DICT_NOT_FOUND`；
- 遍历：`DictStrPtrIter *it = d.iter()`，循环 `it.next()`，用 `it.key()` / `it.value()`；
- 遍历中删除：`it.removeCurrent()`，之后继续 `next()`；
- 收尾：`it.release()`、`d.release()`。

键的缓冲区必须活得比条目久，插入后不能再改内容；这是与 `hashmap` 最本质的差别。

## 4. API 全表

`namespace dict` 只放构造器；两个形状各自成宿主，方法同名不冲突。

### 4.1 `namespace dict`（`Lib`）

| 方法 | 签名 | 语义 |
| --- | --- | --- |
| `newStrPtr` | `DictStrPtr *newStrPtr()` | 建 `char *` → `void *` 表 |
| `newIntPtr` | `DictIntPtr *newIntPtr()` | 建 `int` → `void *` 表 |

### 4.2 `DictStrPtr *`

| 方法 | 签名 | 语义 |
| --- | --- | --- |
| `put` | `DictStrPtr *put(const char *key, void *value)` | 插入或覆盖；不拷贝；返回接收者 |
| `get` | `int get(const char *key, void **out)` | `out` 可为 0，只判存在性 |
| `del` | `int del(const char *key)` | 删除并返回状态 |
| `len` | `int len()` | 条目数，上限 2^31-1 |
| `clear` | `DictStrPtr *clear()` | 删条目，不释放键值；返回接收者 |
| `reserve` | `DictStrPtr *reserve(int buckets)` | 预扩容；被拒时 `status()` 为 `DICT_INVALID` |
| `status` | `int status()` | 最近一次数据 / 扩容操作的状态 |
| `iter` | `DictStrPtrIter *iter()` | 新建 safe iterator |
| `release` | `void release()` | 释放表，不释放键值 |

### 4.3 `DictStrPtrIter *`

| 方法 | 签名 | 语义 |
| --- | --- | --- |
| `next` | `int next()` | 1 = 有当前条目，0 = 结束 |
| `key` | `char *key()` | 借用当前键 |
| `value` | `void *value()` | 借用当前值 |
| `removeCurrent` | `int removeCurrent()` | 删除当前条目；之后仍可 `next()` |
| `release` | `void release()` | 释放迭代器 |

### 4.4 `DictIntPtr *` / `DictIntPtrIter *`

与 §4.2 / §4.3 同构，签名差异：

- `put(int key, void *value)`；
- `get(int key, void **out)`；
- `del(int key)`；
- `IntPtrIter.key()` 返回 `int`。

`int` 键由 shim 放进一个 4 字节键槽（每次插入一次分配），由表在删除/清空/释放时回收。

### 4.5 状态码

| 常量 | 值 | 含义 |
| --- | ---: | --- |
| `DICT_OK` | 0 | 成功 |
| `DICT_NOT_FOUND` | 1 | 键不存在 |
| `DICT_INVALID` | 2 | 空键 / 空句柄 / 扩容被拒 |

## 5. 教程正文

### 5.1 何时选本包

键值已经由调用方长期持有，表只是索引；或者表很大、不能接受一次性扩容停顿；
或者需要在遍历中删除。满足任一条选本包。要"写完就放手"选 `hashmap`；
要定容、`put` 不分配选 `std::map`。

### 5.2 "只存指针"意味着什么

- `put` 不复制键，也不复制值；条目里只有两个指针。
- 键缓冲区必须活得比条目久；插入后**不能修改内容**，否则哈希对不上，条目永久失联，
  `release()` 也无法正确定位。
- `clear()` / `release()` 不会释放键值指向的对象；需要托管时调用方自己清理。

### 5.3 增量 rehash

扩容不是一次性搬全表，而是每次 `put` / `get` / `del` 顺手搬少量桶。代价是单次操作仍有
摊销，但不会出现"第 65536 条插入卡住 50ms"这种台阶。`reserve()` 可以提前给够桶数。

### 5.4 safe iterator

`iter()` 返回的就是 safe iterator：它在交出当前条目前已经预取下一项，所以
`removeCurrent()` 之后继续 `next()` 是安全的。`key()` / `value()` 是借用指针，
`next()` / `removeCurrent()` 之后立刻失效。

### 5.5 失败语义（对外契约）

**内存不足 = 进程 abort，不返回错误码。** 这是 Redis `zmalloc` 的默认行为，也是本包的
正式契约：`put` / `get` / `del` / `clear` / `reserve` **都不会**在 OOM 时返回可处理的失败；
调用方不要把 `status()` 当作 OOM 通知。

这是两个包（`dict` / `hashmap`）**共同的契约**：`hashmap` 虽有 `hashmap_oom()` 可检，但
链式 `put` 没有错误通道，检出后同样终止（见其 README §5.5 与设计文档 §6）。因此
"写失败怎么办"在这两个包之间切换时答案不变：不办，因为进程已经不在了。

返回值只表达逻辑失败：键不存在（`DICT_NOT_FOUND`）、参数非法（`DICT_INVALID`）。
`reserve()` 返回 `DICT_INVALID` 表示扩容被内部状态拒绝，不是 OOM。
需要 OOM 可恢复语义时只能选 `std::map`（`put` 返回 -1）；`hashmap` 与本包一样终止。
软失败若要做，是独立切片，不在 S0 范围。

## 6. 常见配方

- **遍历中删除**：`for (it = d.iter(); it.next(); ) { if (pred(it.value())) it.removeCurrent() }`。
- **预扩容**：十万级批量插入前 `d.reserve(131072)`，减少 rehash 次数。
- **键来自堆缓冲区**：把键的所有权与释放责任留在调用方，表只存指针；不要 `free` 后再查。
- **值是指针但需要区分"不存在"与"值为 0"**：用 `get(key, 0)` 的返回状态判断，
  不要用 `get(key, &p)` 后 `p == 0` 判断。
- **临时键**：查询时可以用栈上缓冲区，`get` 只读指针内容，不保存它。

## 7. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 插入过的键查不到 | 键缓冲区被改过内容，或已被 `free` | 键不可变、不可提前释放；需要托管用 `hashmap` |
| 遍历时崩溃 | 用 `del()` 删了当前键，再读 `key()` / `value()` | 删当前条目用 `removeCurrent()` |
| 进程在大量插入时直接退出 | OOM，Redis `zmalloc` 的默认行为是 `abort` | 本包与 `hashmap` 都不做软失败；降低内存压力，或改用定容的 `std::map` |
| `reserve()` 返回 `DICT_INVALID` | 扩容被内部状态拒绝（正在 rehash / 扩容被禁用） | 稍后重试；不当作 OOM |
| 安装后编译报缺 `zmalloc.h` | 第三方目录被放进了 `vendor/` | 必须是 `third_party/`；`ct get` 会跳过 `vendor` |
| 误用了 7.4+/8.x 源码 | 许可不同（RSALv2/SSPL、AGPLv3） | 只用 7.2.x；见 LICENSE |

## 8. 限制与未支持

- **没有自动析构**：只提供 `release()`，`Drop` 由调用方写。
- **不拷贝键值**：键必须长期有效且不可变；值永远不归包所有。
- **覆盖不释放旧键值**：`put` 命中已有键时只替换指针，旧键/旧值需调用方自行回收。
- **键是 C 字符串**，按 `strlen` / `strcmp` 处理，不允许内嵌 `0x00`。
- **不承诺遍历中用 `del()` 删除其它键**；只支持 `removeCurrent()`。
- **不承诺遍历中新增的键可见**；只保证不崩、不漏掉遍历开始前已存在的键。
- **OOM = 进程终止**（与 `hashmap` 相同），没有错误码可降级。
- **不暴露 `dictScan`**（CT 无闭包）、不暴露进程级 `dictSetResizeEnabled` / rehash 暂停开关。
- **`len()` 是 `int`**，超过 2^31-1 条不在支持范围。
- **非线程安全**；并发由调用方加锁。
- **需要 9 个上游文件 + 1 个本地断言适配头**，比 `hashmap` 重；不追求单文件。
- **不做泛型形状**：新增键值形状需新增 `dictType`、哈希/比较与 14 行 mapping。

## 9. 版本与变更记录

### 0.1.0（2026-11-08，未发布）

- S0：需求与骨架。`ct.pkg`、9 节 README、契约 `.m.ct` / `.h.ct`、`tests/` 占位。
- OOM 语义定为进程终止，并与 `hashmap` 统一为两个包共同的契约（§5.5）。
- 未写实现、未拷第三方源码、未打 tag、未 push。
- 首个 tag 为 `v0.1.0`，在此之前完成设计文档 §10 的 S2.0–S2.5。
