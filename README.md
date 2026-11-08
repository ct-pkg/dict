# dict

Redis 7.2.4 `dict` 的 CT 包封装：**键复制托管、值借用、增量 rehash、safe iterator**。
与 `hashmap`、`std::map` 并列但语义不同：本包只拷贝键（值仍由调用方持有），
扩容没有全表停顿，并且支持遍历中删除当前元素。

```ct
#include <stdio.h>

import dict::DictTypes;
import dict::Dict;

int main(void)
{
    DictStrPtr *d;
    void *out = 0;
    int one = 1;
    int two = 2;

    d = dict::Lib.newStrPtr();
    d.put("one", &one).put("two", &two);
    printf("%d\n", d.get("two", &out));   /* prints 0: DICT_OK */
    printf("%d\n", *(int *)out);          /* prints 2 */
    d.release();
    return 0;
}
```

README 里的每个 `ct` 块都是完整程序：`tools/check.sh` 会逐个抽取、`ct build` 并运行，
再把 stdout 与预期输出逐字比对。

> **状态：0.1.0 已实现，未发布。** 包可编译、可安装、有测试与静态门禁；远端还没有
> `v0.1.0` tag，`index/index` 无本包行。需求与契约以 [docs/design.md](docs/design.md)
> 为准，实现细节与门禁变异证据见 [docs/implementation.md](docs/implementation.md)。

## 1. 定位与边界

做三件事：

- 只借用值的哈希表：`put` 复制键并由表持有，值只保存指针，生命周期归调用方；
- 增量 rehash：每次操作搬少量桶，十万级以上也不会出现一次性全表停顿；
- safe iterator：`next()` 预取下一项，遍历中可以 `removeCurrent()`。

不做的事：

- **不是 `hashmap` 的替代**：本包不拷贝值，值对象必须活得比条目久（键不需要）；
- **不是 `std::map` 的替代**：本包会分配、会增长，内存不可预先计算；
- 不统一三者接口；不提供自动析构；不是线程安全的；
- **OOM 不做软失败**：写入与建表路径在内存不足时直接 `abort`，与 `hashmap` 同一契约；
  `reserve()` 按成因分化（§5.5）：逻辑拒绝返回错误码，真正的分配失败同样终止进程。

场景判据见 [docs/design.md](docs/design.md) §1，选型总表见 `ct/docs/design/map-options.md`。

## 2. 依赖与安装

| 项 | 值 |
| --- | --- |
| 包名 / 版本 | `dict` / `0.1.0`（未打 tag） |
| 许可 | BSD-3-Clause（Redis 7.2.x），见 [LICENSE](LICENSE) |
| 上游 | redis/redis，基线 tag `7.2.4`（`src/`） |
| 编译器 | `requires = ct >= 0.20.3`（`pub type` 与 OO 映射按 ct v0.20.3 实测） |
| 包依赖 | 无 |
| 链接 | `ldflags` 编译 `dict.c` / `siphash.c` / `zmalloc.c` / `mt19937-64.c` / `dict/Dict.seed.c` |

**未发布状态**：本仓库没有 `v*` tag。`index/index` 无本包行；`index/packages.txt` 中的行
保持注释（`refresh-index.sh` 遇到无 tag 仓库会整体失败）；`tools/repos.txt` 的 `tag` 列写 `-`。
首个 tag 推送时取消 `index/packages.txt` 的注释。

**许可边界**：只允许 Redis **7.2 及更早**（BSD-3-Clause）。7.4+ 是 RSALv2/SSPL，8.0+ 是
AGPLv3，不得引入。`COPYING` 与源文件版权头必须逐字保留；本包不是 Redis 官方发行版。
第三方源码全部在 `third_party/`，不能改名 `vendor/`：`ct get` 的目录拷贝会跳过 `vendor/`，
安装后就会缺文件。

安装（发布前用包目录的绝对路径）：

```sh
ct get /path/to/dict
ct install
```

`ct install` 打印的构建命令等价于：

```sh
ct build -I vendor/dict \
    --cflags '-I vendor/dict/third_party' \
    --ldflags 'vendor/dict/third_party/dict.c vendor/dict/third_party/siphash.c vendor/dict/third_party/zmalloc.c vendor/dict/third_party/mt19937-64.c vendor/dict/dict/Dict.seed.c' \
    yourapp.ct -o yourapp
```

## 3. 快速开始

写、读、遍历、遍历中删除、链式收尾（完整程序，输出见块内注释）：

```ct
#include <stdio.h>

import dict::DictTypes;
import dict::Dict;

int main(void)
{
    DictStrPtr *d;
    DictStrPtrIter *it;
    void *out = 0;
    int one = 1;
    int two = 2;
    int three = 3;

    d = dict::Lib.newStrPtr();
    d.put("one", &one).put("two", &two).put("three", &three);
    printf("len: %d\n", d.len());          /* 3 */

    d.get("two", &out);
    printf("two: %d\n", *(int *)out);      /* 2 */

    it = d.iter();
    while (it.next()) {
        if (*(int *)it.value() == 2) {
            it.removeCurrent();
        }
    }
    it.release();

    printf("len: %d\n", d.len());          /* 2 */
    printf("clear returns receiver: %d\n", d.clear() == d); /* 1 */
    d.release();
    return 0;
}
```

`int` 键形状只需把构造器换成 `dict::Lib.newIntPtr()`，`put(1, &v)` / `get(1, &out)` /
迭代器的 `key()` 返回 `int`。完整可运行版本是 [demo/main.ct](demo/main.ct)，
输出固定在 [demo/expected.out](demo/expected.out)，由 `tests/run.sh` 逐字比对。

键在 `put` 时被复制：调用方插入后可以立即释放或改写自己的缓冲区。值不会被复制，
值对象必须活得比条目久；这是与 `hashmap` 最本质的差别。

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
| `put` | `DictStrPtr *put(const char *key, void *value)` | 复制键；插入或覆盖（覆盖只换值）；返回接收者 |
| `get` | `int get(const char *key, void **out)` | `out` 可为 0，只判存在性 |
| `del` | `int del(const char *key)` | 删除条目并释放键副本，返回状态 |
| `len` | `int len()` | 条目数，上限 2^31-1 |
| `clear` | `DictStrPtr *clear()` | 删条目并释放键副本，不释放值；返回接收者 |
| `reserve` | `DictStrPtr *reserve(int buckets)` | 预扩容；逻辑拒绝 → `DICT_INVALID`；真实分配失败 → abort（§5.5） |
| `status` | `int status()` | 最近一次数据 / 扩容操作的状态 |
| `iter` | `DictStrPtrIter *iter()` | 新建 safe iterator |
| `release` | `void release()` | 释放表与键副本，不释放值 |

### 4.3 `DictStrPtrIter *`

| 方法 | 签名 | 语义 |
| --- | --- | --- |
| `next` | `int next()` | 1 = 有当前条目，0 = 结束 |
| `key` | `char *key()` | 借用当前条目的键副本 |
| `value` | `void *value()` | 借用当前值 |
| `removeCurrent` | `int removeCurrent()` | 删除当前条目；之后仍可 `next()` |
| `release` | `void release()` | 释放迭代器 |

### 4.4 `DictIntPtr *` / `DictIntPtrIter *`

与 §4.2 / §4.3 同构，签名差异：

- `put(int key, void *value)`；
- `get(int key, void **out)`；
- `del(int key)`；
- `IntPtrIter.key()` 返回 `int`。

`int` 键由 shim 放进一个 4 字节键槽（每次新插入一次分配），由表在删除 / 清空 / 释放时回收。

### 4.5 状态码

| 常量 | 值 | 含义 |
| --- | ---: | --- |
| `DICT_OK` | 0 | 成功 |
| `DICT_NOT_FOUND` | 1 | 键不存在 |
| `DICT_INVALID` | 2 | 空键 / 空句柄 / 参数非法 / 扩容被逻辑拒绝 |

## 5. 教程正文

### 5.1 何时选本包

值已经由调用方长期持有，表只是索引；或者表很大、不能接受一次性扩容停顿；
或者需要在遍历中删除。满足任一条选本包。要"键值都托管、写完就放手"选 `hashmap`；
要定容、`put` 不分配选 `std::map`。

### 5.2 键复制、值借用意味着什么

- `put` 复制键（`char *` 用 `strlen + 1` 字节，`int` 用 4 字节键槽），值只保存指针。
- 键副本归表所有：`del` / `clear` / `release` 各释放一次。调用方插入后可以立即释放或
  改写自己的键缓冲区，条目不受影响。
- 值永远不归包所有；`clear()` / `release()` 不会释放值指向的对象。
- **覆盖已有键时只替换值，键保持首次插入的那份副本**（跟随上游 `dictReplace`）。
  因此再次 `put` 传入内容相同的另一个缓冲区不会改变 `key()` 的返回，也不会释放旧值
  指向的对象。`tests/unit.ct` 先记录首次插入后 `key()` 返回的地址，再用同字节的新缓冲区
  覆盖，并断言该地址不变——"保留首个副本"与"换成第二个副本"只有指针身份能区分（Q1）。

### 5.3 增量 rehash

扩容不是一次性搬全表，而是每次 `put` / `get` / `del` 顺手搬少量桶。代价是单次操作仍有
摊销，但不会出现"第 65536 条插入卡住 50ms"这种台阶。`reserve()` 可以提前给够桶数；
它只在桶数确实变大时报告 `DICT_OK`。

上游 7.2.4 的扩容返回值有个坑：非终止入口丢弃了内部扩容函数的结果，没有真正扩容也报
成功。本包不信任返回值，改用调用前后桶数的差值判定（Q3）。

### 5.4 safe iterator

`iter()` 返回的就是 safe iterator：它在交出当前条目前已经预取下一项，所以
`removeCurrent()` 之后继续 `next()` 是安全的。`key()` / `value()` 是借用指针，
`next()` / `removeCurrent()` 之后立刻失效。

如果遍历途中对表调用 `clear()` 或 `release()`，迭代器随即失效：`next()` 返回 0，
不会去读已经释放的条目（generation 守卫，见 §9）。

### 5.5 失败语义（对外契约）

**写入与建表路径 OOM = 进程 abort，不返回错误码。** 这是 Redis 分配器的默认行为，
也是本包的正式契约：`newStrPtr()` / `newIntPtr()`、`put`、`get` 触发的扩容、迭代器创建
在 OOM 时都不会返回可处理的失败；调用方不要把 `status()` 当作 OOM 通知。

**`reserve()` 按成因分化（Q2）：**

- **逻辑拒绝**——正在 rehash、请求桶数落在同一个 2 的幂、小于已有条目数、尺寸溢出——
  不触碰分配器，直接返回 `DICT_INVALID`，进程继续；
- **真正的分配失败**——逻辑条件都已排除后扩容仍失败——走与其它路径相同的 OOM handler
  终止进程，不返回错误码。

这条分化由 `tests/oom.c` 的两路子进程分别锁定：armed `reserve` 必须 `SIGABRT`，
逻辑拒绝必须不 abort 且 `status()` 为 `DICT_INVALID`。删除 abort 分支会让前一路变红
（实现说明 M22）。

返回值只表达逻辑失败：键不存在（`DICT_NOT_FOUND`）、参数非法（`DICT_INVALID`）。
需要 OOM 可恢复语义时只能选 `std::map`（`put` 返回 -1）；`hashmap` 与本包一样终止。

## 6. 常见配方

- **遍历中删除**：`for (it = d.iter(); it.next(); ) { if (pred(it.value())) it.removeCurrent() }`。
- **预扩容**：十万级批量插入前 `d.reserve(131072)`，减少 rehash 次数；这里的 `131072`
  是**桶数**，不是字节数（§10 的 `reserve=131072 B` 才是字节）。
- **键来自临时缓冲区**：直接 `put(buf, v)`，表会复制键；随后改写或释放 `buf` 都不影响条目。
- **值是指针但需要区分"不存在"与"值为 0"**：用 `get(key, 0)` 的返回状态判断，
  不要用 `get(key, &p)` 后 `p == 0` 判断。
- **临时键查询**：`get` 只读取调用方传入的字节并逐字节比较，不会保存该指针。
- **覆盖时想换键副本**：`put` 单独不会改键副本；需要换就先 `del(oldKey)` 再 `put(newKey, v)`。

## 7. 术语表（Glossary）

| 术语 | 含义 | 本包中的所指 |
| --- | --- | --- |
| `borrow`（借用） / 所有权（ownership） | 借用是只保存或返回指针、不取得所有权、由调用方负责释放的引用方式；所有权指"谁负责在何时释放对象"。 | 值（两种形状的 `void *`）是借用：`clear()` / `release()` 不释放值指向的对象。键是例外：`DictStrPtr` 的 `char *` 键被复制，`DictIntPtr` 的 `int` 键写进 4 字节键槽，两者都由表持有并在删除 / 清空 / 释放时回收。 |
| 键副本 / 键槽（key copy / key slot） | 表为保存键而自己分配的内存。 | `DictStrPtr` 每个新键复制一份 `strlen + 1` 字节的副本；`DictIntPtr` 每个新键分配一个 4 字节键槽。二者各自由表释放一次；覆盖已有键不会新建。 |
| `handle`（句柄） | 指向包内部对象的不透明标识；调用方只能通过包的方法操作它，不能解引用或自行分配。 | `DictStrPtr *`、`DictIntPtr *`、`DictStrPtrIter *`、`DictIntPtrIter *` 四个句柄。 |
| `host`（宿主） | CT 映射表里方法挂靠的类型；方法按（宿主, 方法名）寻址，同一宿主上不能有两个同名方法。 | 上述四个类型各自是宿主；`namespace dict`（trait `Lib`）只挂 `newStrPtr()` / `newIntPtr()`。 |
| `mapping`（映射） | 把 CT 方法转发到 C 实现的 shim 注册行，一个公开方法对应一行。 | 每个形状 14 行（表 9 + 迭代器 5），两个构造器另计；`tools/check.sh` 校验 trait 与 mapping 一一对应。 |
| 链式返回自身 | 配置 / 写入方法返回接收者本身，以便把多次调用连成一条表达式；返回值不携带成功 / 失败信息。 | `put` / `clear` / `reserve` 返回 `DictStrPtr *` 或 `DictIntPtr *` 本身；`get` / `del` / `status` / `len` 返回状态码或计数，不参与链式。 |
| `pub type` | CT 的公开类型别名声明；本包用它把 C 侧包装结构暴露成 CT 句柄，公开面不出现 C 类型。 | `Dict.m.ct` 里的四个 `pub type`：`DictStrPtr = struct ct_dict_strptr`、`DictIntPtr = struct ct_dict_intptr`，以及对应的两个迭代器类型。`pub type` 本身不生成 C typedef，源码里的星号是手写的。 |
| `shim` | C 侧适配层：把 CT trait 方法桥接到上游 C API 的静态内联函数与注册代码。 | `dict/Dict.shim.h` 里的 `static inline` 函数、两种形状的 `dictType`、`int` 键槽与 `removeCurrent` 的桥接。 |
| `DictStrPtr` / `DictIntPtr`（两种形状） | 本包两个具体表类型，靠键类型区分，各自独立成宿主。 | `DictStrPtr *`：`char *` 键（表持有副本）→ 借用 `void *` 值；`DictIntPtr *`：`int` 键（表持有键槽）→ 借用 `void *` 值。构造器为 `newStrPtr()` / `newIntPtr()`。 |
| `rehash`（增量 rehash） | 桶数变化后把条目重新分布到新桶数组的过程；增量指把搬迁摊到多次操作，而不是一次搬完整表。 | 本包是增量 rehash：每次 `put` / `get` / `del` 搬少量桶，没有一次性全表停顿。`hashmap` 相反，是一次性搬全表。 |
| `safe iterator`（安全迭代器） | 交出当前条目前已预取下一项的迭代器，因此删除当前元素后仍可继续遍历。 | `iter()` 返回 `DictStrPtrIter *` / `DictIntPtrIter *`；`removeCurrent()` 之后继续 `next()` 是安全的。`hashmap` 的 `HashStrIntIter *` / `HashIntPtrIter *` 是只读游标，没有这个保证。 |
| `status()` | 读取最近一次数据 / 扩容操作留下的状态码，只表达逻辑失败。 | `DictStrPtr` 与 `DictIntPtr` 上各有一个 `status()`，返回 `DICT_OK` / `DICT_NOT_FOUND` / `DICT_INVALID`；**它不是 OOM 通知**——OOM 路径上进程已经 `abort()`（§5.5）。 |
| `DICT_OK` / `DICT_NOT_FOUND` / `DICT_INVALID` | 本包三个状态码：成功、键不存在、参数非法。 | 值为 0 / 1 / 2，定义在 `DictTypes.h.ct`；`DICT_NOT_FOUND` 由 `get` / `del` / `removeCurrent` 未命中触发，`DICT_INVALID` 由空键、空句柄、非正桶数或 `reserve()` 被逻辑拒绝触发。 |
| `dictType` | Redis 中描述一种表的哈希、比较与键值析构回调的常量集合。 | 每个形状一个，只存在于 shim；两者都只设置 `keyDestructor`（键副本 / 键槽回收），`valDestructor` 为 `NULL`。 |
| `zmalloc` / `zfree` | Redis 的分配器包装，分配失败时走默认 handler 打印并 `abort()`。 | 本包的分配（表、条目、键副本、`int` 键槽）都经过它们；这也是 §5.5 的 OOM 契约在上游的落点。`reserve()` 用同族的非终止入口分配，失败后由 shim 主动触发同一 handler。 |
| `OOM` | out of memory，内存分配失败。 | 本包直接进程终止：写入、建表与 `reserve()` 的真实分配失败都不返回可处理的失败；`reserve()` 只有逻辑拒绝会返回 `DICT_INVALID`（§5.5、Q2）。 |
| `third_party/` / `vendor/` | 存放 vendored 上游源码的目录约定。 | Redis `7.2.4` 的源文件与本地 `redisassert.h` 适配头都放 `third_party/`；不能改名 `vendor/`，`ct get` 会跳过该目录。 |
| `S0` / `S2` | 本包落地切片的编号。 | S0 = 需求与骨架；S2 = 上游抽取、实现、测试与安装态验收，见 `docs/design.md` §10。当前实现覆盖 S2.0–S2.5。 |

## 8. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 插入过的键查不到 | 键里含内嵌 `0x00`（按 C 字符串截断），或句柄已经 `release()` | 键是 C 字符串；句柄释放后不再使用 |
| 遍历时崩溃 | 用 `del()` 删了当前键，再读 `key()` / `value()` | 删当前条目用 `removeCurrent()` |
| 进程在大量插入时直接退出 | OOM，写入 / 建表 / `reserve()` 的真实分配失败都走默认 `abort` | 本包与 `hashmap` 都不做软失败；降低内存压力，或改用定容的 `std::map` |
| `reserve()` 返回 `DICT_INVALID` | 正在 rehash / 请求桶数落在同一 2 的幂 / 小于已有条目数 / 尺寸溢出 | 逻辑拒绝，进程不会终止；调整桶数后重试（§5.5） |
| 第二次 `put` 同内容新键后 `key()` 仍是首次的副本 | 上游覆盖只替换值，保留首个键副本 | 预期行为；要换键先 `del`（§5.2） |
| 安装后编译报缺 `zmalloc.h` | 第三方目录被放进了 `vendor/` | 必须是 `third_party/`；`ct get` 会跳过 `vendor` |
| 误用了 7.4+/8.x 源码 | 许可不同（RSALv2/SSPL、AGPLv3） | 只用 7.2.x；见 LICENSE |

## 9. 限制与未支持

- **没有自动析构**：只提供 `release()`，`Drop` 由调用方写。
- **遍历中 `clear()` / `release()` 会作废迭代器**（generation 守卫）：句柄维护一个
  代数计数，`clear()` / `release()` 自增；迭代器记住创建时的计数，`next()` / `key()` /
  `value()` 发现不一致即返回 0。这两个操作因此不会造成 use-after-free，但迭代器也
  不会继续给出条目。`tests/gen.c` 在 ASan 下锁定该语义。
- **值仍借用**：键由表复制持有，值不复制、不释放；值对象必须活得比条目久。
- **覆盖不释放旧值**：`put` 命中已有键时只替换值指针，旧值需调用方自行回收；
  键副本保留第一次插入的那份（§5.2）。
- **键是 C 字符串**，按 `strlen` / `strcmp` 处理，不允许内嵌 `0x00`。
- **不承诺遍历中用 `del()` 删除其它键**；只支持 `removeCurrent()`。
- **不承诺遍历中新增的键可见**；只保证不崩、不漏掉遍历开始前已存在的键。
- **OOM 一律终止进程**；`reserve()` 只把逻辑拒绝映射成 `DICT_INVALID`（§5.5、Q2）。
- **不暴露 `dictScan`**（CT 无闭包）、不暴露进程级 `dictSetResizeEnabled` / rehash 暂停开关。
- **`len()` 是 `int`**，超过 2^31-1 条不在支持范围。
- **非线程安全**；并发由调用方加锁。
- **需要 9 个上游文件 + 3 个本地适配头**，比 `hashmap` 重；不追求单文件。
- **不做泛型形状**：新增键值形状需新增一份表类型描述、哈希/比较回调与 14 行 mapping。

## 10. 版本与变更记录

### 0.1.0（2026-11-08，未发布）

- 实现：Redis `7.2.4` 的 `dict` 抽取到 `third_party/`，shim + 4 个 `pub type` + 5 个 trait
  全部落地；`ct build` 可编译，`ct get` / `ct install` 的安装布局可构建。
- 键语义（Q1）：`put` 复制键并由表持有，覆盖只替换值、保留首个键副本；值始终借用。
- 扩容语义（Q2/Q3）：`reserve()` 逻辑拒绝返回 `DICT_INVALID`，真实分配失败 abort；
  扩容成败用桶数前后差判定，不信任上游非终止入口的返回值。
- 哈希种子（B1）：播种移入独立编译单元 `dict/Dict.seed.c`（函数级 `static` 闩锁），
  shim 头只留声明；同一进程内无论多少个调用方 TU，都只播种一次。`tests/multi_tu_a.c` /
  `multi_tu_b.c` 组成双 TU 门禁，连续 20 次"第二张表建完后回读第一张表"必须全中。
- 迭代器失效（R1）：generation 守卫——`clear()` / `release()` 自增句柄代数，迭代器
  `next()` / `key()` / `value()` 发现代数不符即返回 0；ASan 门禁 `tests/gen.c`（8 条）。
- 测试：`tests/unit.ct`（68）、`iterator.ct`（343）、`rehash.ct`（16）、`chain.ct`（17）
  共 444 条断言；`tests/slots.c` 做分配账目差分（含 5 条增量 rehash 行为断言），
  `tests/oom.c` 四路验证（put / constructor / reserve 真实 OOM 均 abort，逻辑拒绝不 abort），
  `tests/gen.c` 在 ASan 下验证 generation 守卫；demo 输出与 `demo/expected.out` 逐字一致。
- 分配账目（macOS `malloc_size` 的 usable size；其它平台的具体字节数会不同，差分判据不变）：
  1k 条 `int` 条目 `48000 B`、10k 条 `480000 B`、16384 桶的桶数组 `131072 B`、清空后残差
  `0 B`；字符串短键 `48000 B` / 长键 `160000 B`，差 `112000 B` 证明键被复制。注意
  `reserve=131072 B` 是**字节**（16384 个桶指针），与 `d.reserve(131072)` 的**桶数**不是
  同一单位。
- 门禁：`tests/run.sh` 对每个套件设断言数下限（68 / 343 / 16 / 17 / 1536 / 4 / 8），
  另有 `multi_tu`（20 次）与 demo；低于下限即红，掏空 `check()` 无法蒙混。`tools/check.sh`
  覆盖交付文件集与 `git ls-files` 跟踪、`ct.pkg` 字段、第三方 sha256、许可边界、OO 公开面、
  shim 约束（禁止任何非 `static inline` 的有状态定义）、分配器纪律、README 结构与
  `ct` 块逐个编译运行、安装布局与 `tests/run.sh`；每个门禁类别都有变异测试记录
  （[docs/implementation.md](docs/implementation.md) §7）。
- 文档：新增 `docs/implementation.md`；README 术语表位于「限制」之前（§7）。
- 未打 tag、未 push。
