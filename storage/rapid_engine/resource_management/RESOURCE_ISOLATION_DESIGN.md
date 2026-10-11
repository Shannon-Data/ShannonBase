<!-- Copyright (c) 2026, Shannon Data AI and/or its affiliates. -->

# ShannonBase TP/AP 资源隔离 V1

关联：[Issues #932](https://github.com/Shannon-Data/ShannonBase/issues/932)、[#34](https://github.com/Shannon-Data/ShannonBase/issues/34)。
调研日期：2026-10-11。测量基线：`3ff82b4b1`；已 rebase 到最新 main `c3dfc1756`（两者源码树完全一致）；设计分支：`feature/rapid-resource-isolation-932`。
阶段 A/B 的运行代码已实现并完成下述验证；阶段 C/D 保留为后续设计。以下第 18 节说明首版实际边界与部署方式。

## 1. 建议采用的路线

推荐采用“资源组与分类 + 操作准入/预算 + CPU 隔离后端”的组合：

1. 将 TP、AP、SYNC（binlog reader/apply）、MAINT（load/reload/GC/compaction/stats）分开计量和调度。
2. 优先复用 MySQL 8.4 Resource Groups 管理线程 CPU affinity，在 Rapid 中补充查询准入、后台并发、内存和 I/O 预算。
3. Linux 上增加可选的 cgroup v2 CPU quota/weight 后端；严格 CPU 模式采用不重叠的物理核集合，不能只调整 nice 值或限制线程数。
4. 保持源端 DML/commit 不等待资源令牌；同步背压发生在后台，AP 的一致性等待不突破 SYNC 上限。
5. 对 shared-cache、内存带宽、InnoDB buffer pool、设备 I/O 等剩余干扰明确计量。若要求更强隔离，另行评估进程/节点拆分。

第一阶段建议交付 CPU 分区、AP 准入、SYNC/MAINT 并发与可观测性；它是可验证的起点，不是 #932 全部能力的完成声明。

## 2. OceanBase 调研结论

| 能力 | 官方资料/公开代码确认的机制 | ShannonBase 的借鉴方式 |
| --- | --- | --- |
| 分层资源管理 | 租户资源单元与租户内 consumer group/resource plan | 当前没有对应租户模型，先采用服务器级资源组，不直接复制租户体系 |
| 工作分类 | 支持用户、SQL、后台功能到资源组的映射 | 初版按 TP/AP 引擎路径与后台任务类别自动分类，支持明确的 AP 会话/用户绑定 |
| CPU | cgroup 分配权重、上限；独立后台 CPU 配额 | 保留后台总预算，SYNC 与 MAINT 再分配；复用 MySQL affinity，可选 cgroup CPU 后端 |
| I/O | MIN_IOPS/MAX_IOPS/WEIGHT_IOPS 管理保留、上限与竞争下分配 | 设计带类别标记的 Rapid I/O 入口与字节/操作速率预算，不能把 CPU 控制当成 I/O 隔离 |

上述资料分别来自 OceanBase [租户内资源隔离（MySQL 模式）](https://en.oceanbase.com/docs/common-oceanbase-database-10000000003683721)、[后台独立 CPU 配额](https://en.oceanbase.com/docs/common-oceanbase-database-10000000001719934)。不同版本的能力不能混用：文档以链接所列版本为准。

另外检查了 OceanBase 公开 `master` 提交 `5e8a63d29dae9fde5e962b8fcc7b167d438fc343`：

- [ob_cgroup_ctrl.cpp](https://github.com/oceanbase/oceanbase/blob/5e8a63d29dae9fde5e962b8fcc7b167d438fc343/src/share/resource_manager/ob_cgroup_ctrl.cpp) 区分 cgroup v1/v2，包含线程入组、v2 threaded 层级以及 quota/weight 接口。
- [ob_resource_plan_manager.cpp](https://github.com/oceanbase/oceanbase/blob/5e8a63d29dae9fde5e962b8fcc7b167d438fc343/src/share/resource_manager/ob_resource_plan_manager.cpp) 处理独立后台 CPU 配额和 I/O directives 的归一化。
- [ob_resource_mapping_rule_manager.cpp](https://github.com/oceanbase/oceanbase/blob/5e8a63d29dae9fde5e962b8fcc7b167d438fc343/src/share/resource_manager/ob_resource_mapping_rule_manager.cpp) 管理资源组映射。

这是公开源码观察，不保证 `master` 对应某个已发布版本。借鉴其分类、保留与上限的结构，不照搬 Oracle 风格的存储过程 API。

## 3. ShannonBase 当前代码与缺口

| 当前落点 | 已有能力 | 本方案需要补充 |
| --- | --- | --- |
| `resource_management/res_mgmt.h`、`utils/utils.cpp` | 全局/查询内存 reservation，算子有界 allocator，读取可用内存/cgroup 余量 | 工作类别、AP admission、预算快照、后台资源计量；不能将已有机制称为 RSS 限制 |
| `include/rapid_config.h` | 查询、算子、列存及 propagation 参数 | 汇总资源隔离配置，避免多套互相冲突的额度 |
| `handler/ha_shannon_rapid.cc`、`include/rapid_context.h` | secondary prepare、执行上下文、失败原因、query SCN | 准入生命周期、线程资源组切换、fallback/错误时恢复 |
| `populate/log_binlog_parser.cpp` | 单 reader，完整事务解码，queue 背压，跨表事务可见性屏障 | 有界 read/decode 调度、SYNC 预算、等待前释放源 TABLE/MDL 的阶段拆分 |
| `populate/log_populate.cpp` | 按 table_id 分派，按表顺序 apply，每活跃表有 worker | 全局公平并发上限，后续有界共享 worker pool；保持表内 FIFO 与错误屏障 |
| `imcs/worker.cpp`、`autopilot/loader.cpp` | GC/compaction/stats 调度、load/reload | MAINT 总预算及 SYNC 与必要回收的进度保留 |
| `imcs/imcs.cpp` | 并行 load，部分线程数依赖硬件线程总数 | 继承 MAINT 类别与预算；不能按全机 CPU 数无条件扩张 |
| `executor/iterators/*` | 当前 AP executor 主要在连接线程单线程执行，已有向量化与 spill | 必须管住实际 THD 线程；只限制一个并不存在的 AP worker pool 无效 |

MySQL 原生 Resource Groups 已提供 USER/SYSTEM 线程 CPU affinity 和优先级，不提供本方案所需的完整内存/I/O 管理；平台和 thread-pool 配置也有限制。见 [MySQL 8.4 Resource Groups](https://dev.mysql.com/doc/refman/8.4/en/resource-groups.html)，以及本地 `sql/resourcegroups/resource_group_mgr.*`。

## 4. 工作分类与资源归属

| 类别 | 包含工作 | 预算策略 |
| --- | --- | --- |
| TP | 用户 DML、主引擎短查询、commit/binlog 持久化，以及支撑源端的必要 InnoDB 系统线程 | 独立 CPU 集合；不向 Rapid governor 申请逐行令牌，不因 AP 额度阻塞 commit |
| AP | 已准入的 Rapid 查询；明确绑定为 AP 的会话中的分析查询，不含已退出 Rapid 的主引擎 fallback | CPU 集合 + active query 上限 + 查询/算子 memory + spill 预算 |
| SYNC | committed reader、propagation coordinator、row apply | 最低进度保留、最大 CPU/并发/读速率；后台 queue 饱和时保留数据并等待 |
| MAINT | load/reload、压缩、统计、非紧急 GC 等 | 总预算内低优先级、公平调度，不能占用 SYNC 的保留额度 |

GC 是混合情况：长时间不给必要回收进度会造成内存反压。为必要回收预留 MAINT 内的最小进度，仍计入后台总预算；不能以“紧急”为由无限绕过上限。

初版不引入多租户、任意 SQL AST 匹配和新的 SQL parser。USER/SYSTEM 是 MySQL 线程类型，不等同 TP/AP 类别：实际 PFS 注册类型必须与目标 Resource Group 匹配。

## 5. CPU：先隔开运行位置，再考虑借用

推荐严格 CPU 模式使用静态、互不重叠的 CPU 集合，默认不借用 TP/AP 保留核。按物理核及 SMT siblings 验证，避免把同一核的两个超线程分别称为“独立 TP/AP 核”。NUMA/LLC 共享仍需单独观测。

配置示意，假设可用 CPU 0–15 且示例没有拆开 SMT siblings：

| 资源 | CPU 示例 | 说明 |
| --- | --- | --- |
| TP USER 与必要 TP SYSTEM | 0–7 | 两种 MySQL group type 使用相同 TP CPU 集合 |
| AP USER | 8–11 | 管理实际执行查询的连接线程 |
| SYNC SYSTEM | 12–13 | reader、coordinator、apply 共用此预算 |
| MAINT SYSTEM | 14–15 | load、reload、GC、stats 等 |

这是部署示例，不作为默认核数或推荐生产比例。核数不足时拒绝严格模式配置；宽松模式可以重叠，但必须显示较弱的保证。

AP 入口在获得 admission 后进入作用域资源组，成功、失败、KILL、fallback、prepare retry 都要恢复原会话归属。`Rapid_execution_context` 持有 token 与兜底清理，但不能仅依赖 LEX 最终析构才恢复：fallback 可能在同一 statement 内执行。

优先使用 MySQL 的 group 管理、MDL、线程登记与恢复接口，不同时维护另一套会话 affinity。只有 Rapid worker 确认登记为合适的 PFS thread type 后才入组。既有 TP/System 线程需启动时扫描并登记，新线程需事件式继承；没有覆盖到的线程数量要可见。仅绑定 Rapid 后台而让 `USR_default`/`SYS_default` 继续使用全部 CPU，不属于完整 CPU 隔离。

CPU 集合和后端切换初版要求隔离工作排空/重启，不在运行查询中更改亲和性。内核 CPU 热插拔、容器 cpuset 收缩和 group drop/disable 必须重新验证有效配置，不能静默宣称仍在严格模式。

## 6. cgroup：作为可选 CPU 后端

Linux 可选后端使用管理员预先委派的 cgroup v2 threaded 子树；服务只操作该子树，不修改宿主机任意 cgroup。启用前探测层级、权限、控制器和实际可用 CPU。

- `cpu.max` 提供上限，`cpu.weight` 提供竞争下的相对份额；weight 不是硬核数保留，quota 也不自动给 TP 预留某些 CPU。
- affinity/cpuset 是运行位置限制，quota 是运行量限制；组合时只允许一致且经过验证的配置。
- 按线程迁移存在 syscall 成本，适合永久后台 worker；混合 TP/AP 会话逐 statement 迁移需专项性能验证，初版优先 affinity 作用域或专用 AP 会话。
- 同一 mysqld 的线程共享资源 domain，不能用 threaded CPU 分组声称完成独立内存、I/O 硬隔离。Linux 当前 threaded controller 列表不包含 memory/io。见 [Linux cgroup v2 Threads](https://cdn.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html#threads)。

管理员已有的 MySQL Resource Group 配置与 Rapid 计划冲突时拒绝激活，不覆盖 DBA 的配置。能力不满足时，`STRICT_CPU` 拒绝启用，`SOFT` 只报告软件预算能力，不能悄悄降级。

## 7. 软件 governor 与准入

在现有 `ResMgmt` 内扩展 `ResourceGovernor`，统一维护不可变配置快照、类别计量、公平运行令牌和 cancellation。接口按操作边界调用，不进入源端逐行 commit 热路径：

- AP `try_admit(query)`：查询数量、现有 memory reservation 和 CPU backend 状态符合条件才准入。
- SYNC/MAINT `try_acquire_work(class, cost)`：按有界 batch/任务分配执行机会，完成后归还并计量实际 CPU 时间、字节和等待。
- `ResourceGroupScope`：负责当前线程资源归属的切换和恢复。
- `MemoryReservation`：延用既有 RAII，补充类别归属和 ownership transfer。

初版 AP 超额使用非阻塞拒绝，不在 secondary prepare 阶段加入长等待。此时 MySQL 可能已持有源表 MDL；新增 admission 等待容易延长 DDL 阻塞。需要等待队列时，应在持表锁前准入或显式关闭/重开表并重新验证，这是后续独立改动。

后台调度采用有界 batch 与公平轮转，不能简单创建“令牌睡眠”，也不能持有 queue mutex、源端 binlog index lock、行锁或 MDL 长时间等待资源。CPU 不用按墙钟时间粗略睡眠估计占用：软调度记录线程 CPU 时间并在安全边界让出，硬上限交给可用的 CPU 后端。

## 8. 按表 apply 的处理

第一阶段保留现有按表 worker，增加**全局 active apply 并发**上限、公平令牌与 CPU 分区，令牌等待前释放调度锁。注意：这限制运行并发，尚不限制所有 per-table thread 的数量和栈占用，不能作为完整线程预算。

第二阶段将逻辑 `table_id -> FIFO/error/generation state` 与物理执行线程分离，使用有界共享 apply pool：

1. 一张表同时最多一个 in-flight task，按 change_id 顺序取有界 batch。
2. batch 完成/预算耗尽后重新进入公平 ready queue，不能让热点表一直占有 worker。
3. TABLE/THD 与事务状态归物理 worker 管理；跨 worker 交接时不转移绑定旧 THD 的 TABLE 指针。
4. 同一表失败阻断后续记录，其他表仍可推进；unload/KILL/shutdown 能唤醒正在等待的逻辑队列。
5. 全局 worker 数独立于 loaded table 数，pending 与 inflight 字节都有预算。

跨表事务仍在所有相关表 apply 后统一发布 SCN。初版继续顺序推进事务；不得因为增加线程池而提前发布某个表的新版本。事务间流水线及有序 publication frontier 不是本轮资源隔离的前置功能。

## 9. reader 的资源等待与一致性

将 reader 的“打开源 TABLE/解码”和“enqueue/apply 等待”拆成独立作用域：先获取必要预算，再短时间打开 metadata、读取完整有界事务并生成拥有字节的记录；释放源 TABLE 外部锁和 MDL 后，后台才等待 queue/执行预算。

等待之后用 loaded generation/取消状态重新验证目标，不持源 TABLE 指针到下一阶段。binlog index lock 只覆盖有界定位/读操作；任何长时间 IO 令牌等待应发生在它外面。source cursor 与 purge pin 在未安全消费时仍必须受到保护，不能以释放锁为由跳过数据。

AP query-demand 可以把 SYNC 提升到其自身额度内的优先级，但不能无限借用 TP/AP CPU 或绕过后台总上限。不要让等待 freshness 的 AP 查询占着有限的 active-execution slot，从而耗尽运行额度；区分 admitted/pending-freshness/active，转入 active 前重新检查预算。

预算造成 lag 不等于数据损坏。禁止因此丢记录、推进 assembled/visible 到未处理位置、提前 ACK、无条件 quarantine 或对同一 mutation 重试造成重复 apply。既有 oversize/源日志失效等真实不支持条件仍按现有规则隔离。

## 10. 内存：先明确可计量范围

复用现有 query/global/operator memory grants，不另建重复的查询额度。现有查询参数为 READONLY 且预算单例容量固定，初版继续启动时配置；不能只改 sysvar 显示值而使已有 reservation 仍按旧容量运行。

新增预算区分：AP 临时算子/spill 缓冲、SYNC 解码/拥有的 row image/queue/inflight、MAINT 临时 load/compression 缓冲、常驻 Rapid 列存。相同 row image 在 reader -> queue -> worker 转移时转移额度 ownership，不重复计费，不出现无主的额度间隙。

类预算保证不超过配置上限；它不是 CPU 内存带宽、整个进程 RSS、InnoDB buffer pool 或 libc allocator 碎片的硬隔离。要为 InnoDB、MySQL、未计量路径和操作系统保留 headroom，并暴露未覆盖的内存范围。关闭/缩小计划不能使未释放 reservation 失效。

COMMITTED_BINLOG 额度不足时由 reader 在不持源端锁的状态下等待。CAPTURE 在源端有必要的 row-image 工作；不能在那里等待后台额度，应维持明确的源端成功/副本失效策略和原因。这两种 producer 的预算耗尽语义不可混同。

## 11. I/O：软件分类，不承诺同进程硬隔离

OceanBase 的组 I/O 管理依赖其资源计划及内部 I/O 管理。ShannonBase 没有现成的统一等价入口，因此需要逐步为 Rapid binlog read、spill、load/reload、checkpoint 添加类别和计量。

初版每类使用有界 outstanding 操作数与 byte rate/token bucket，后续再考虑保留/上限/权重的共享 I/O 调度器。必须同时量化 IOPS 与 bytes，不能把顺序 64 MiB 读和 4 KiB 随机读算成等价资源。

最大合法 event/事务必须能取得预算：允许有界 burst/debt 或分块读取并偿还，不能因一笔操作大于 token bucket 容量而永久饿死。解压后的内存另外计费；源日志读速率额度不能代替解码内存上限。

主引擎 binlog/redo fsync、group commit 等核心耐久化路径不经 Rapid I/O 限速器。后台读仍共享设备/page cache，load/reload 的 InnoDB 读取和回退到主引擎的分析读取需要专门覆盖，不能只限制 spill 就宣称已实现 I/O 隔离。更强保证需独立设备或进程/节点配合。

## 12. 超额与 fallback 策略（按用户确认更新）

首版允许 fallback 使用系统资源。隔离约束作用于 Rapid 执行和后台工作；主引擎 fallback 不再占用 Rapid AP 配额。因此，发生 fallback 的负载可能与 TP 共享 CPU、内存和 I/O，验收时必须单独记录，不能将其计入严格隔离保证。

| 情况 | V1 行为 |
| --- | --- |
| AP admission/CPU 计划不足，`ON` | 释放已取得的 Rapid 配额，恢复进入 Rapid 前的资源组，按 MySQL 既有规则 fallback 主引擎 |
| AP admission/CPU 计划不足，`FORCED` | 返回明确的资源不足原因；释放配额并恢复资源组，不绕过 FORCED 强制 offload 语义 |
| 表不可用/同步超时，`ON` | 清理 Rapid 执行状态，恢复原资源组后 fallback |
| 表不可用/同步超时，`FORCED` | 保留具体 offload 错误及原因，并完成清理 |
| KILL、unload、shutdown | 中断可取消的预算等待，唤醒等待者，释放配额及线程绑定；禁止 shutdown/unload 引发新查询重试 |
| 后台被限速 | 保留数据、等待预算、暴露 lag；不阻塞源端 commit |

“无组资源”对应退出 Rapid 临时绑定并恢复原会话资源组：原先未显式指定资源组时恢复 `USR_default`/`SYS_default`；原先由 DBA 显式绑定资源组时保留其绑定，避免破坏用户配置。不创建名为“无组”的资源组，也不把线程留在 Rapid AP 组执行主引擎 fallback。

需验证 MySQL secondary-to-primary retry 的错误分类、执行上下文销毁和资源组恢复时机。恢复必须发生在主引擎 retry 之前，不能只依赖语句最终析构。优先复用已有生命周期接口，只有证明 engine-local hook 不足时才加入最小服务器桥接。

测试矩阵覆盖资源不足、fallback、KILL、unload 和 shutdown：检查活动/等待配额归零、原组恢复、没有遗留等待、无新任务准入、无重复释放；正常 fallback 还需核对主引擎结果。`ON` 和 `FORCED` 分别验证，KILL 后不能通过 fallback 继续执行已取消查询。

## 13. 配置与能力契约

建议初版提供固定类别，不复制 OceanBase 整套存储过程：

- `rapid_resource_isolation=OFF|OBSERVE|SOFT|STRICT_CPU`：默认 OFF；OBSERVE 只计量，SOFT 使用软件预算并可选 native CPU 组，STRICT_CPU 要求完整 CPU 分类覆盖。
- 使用 MySQL 原生 Resource Groups 的 CPU 配置；Rapid 配置绑定 TP USER/TP SYSTEM/AP/SYNC/MAINT group 名称，校验类型、有效核集合、启用状态与权限。
- AP active query、SYNC active apply、MAINT active task、后台总并发；这些运行上限可以动态缩小，但不终止在途任务，超过新上限时只阻止新增。
- 复用现有 query/operator/column-store 内存参数；新增 propagation/maintenance transient 预算与类别 I/O rate/burst，启动时设置容量。
- optional CPU quota/weight/cgroup path 作为 CPU backend 的配置，不给首版引入另一套与 Resource Group 矛盾的 affinity 参数。

计划以 generation 标记。验证失败保留旧计划。关闭后停止接纳新隔离任务，排空已接纳的工作，恢复线程归属与清理旧 generation；启动未完成或后台新线程无法入组时，严格模式拒绝新的 AP/load 工作并报告 degraded，不停止源端 TP。

## 14. 实现落点与改动范围

首选将通用接口放在 `resource_management/res_mgmt.h`，新增一个 `res_mgmt.cpp` 放实现；配置归 `rapid_config.h`，统计接入既有 Rapid status/Performance Schema。细分 CPU backend 时再按实际规模决定是否拆文件。

主要接入点：`handler/ha_shannon_rapid.cc`、`include/rapid_context.h`、`populate/log_binlog_parser.cpp`、`populate/log_populate.cpp`、`imcs/worker.cpp`、`imcs/imcs.cpp`、`autopilot/loader.cpp`，以及 query allocator/spill 工具。AP worker/并行 load/模型运行产生的额外线程也要继承预算；不能只给调用线程入组就忽略其子线程。

变更优先位于 `storage/rapid_engine`；新文件带版权声明，按已有要求只格式化修改的 Rapid C++ 文件。是否需要最小 server lifecycle/retry bridge，在第一阶段 API 验证后给出具体 diff 再审阅。

## 15. 可观测性

每类暴露：配置/有效 CPU backend、计划 generation、已登记和未分类线程、active/queued、CPU time、quota/令牌等待、reserved/used/peak memory、read/write bytes、I/O 等待，以及 admission 拒绝原因。

SYNC 单独暴露 committed/assembled/visible positions、backlog bytes、事务数、等待 freshness 的 AP 数、SCN/barrier 状态。不同 binlog 文件的 offset 不能直接相减当 lag bytes，需从文件序列/大小计算或标记估计值。

区分 RESOURCE_BUSY、SYNC_BUDGET_WAIT、CPU_BACKEND_UNAVAILABLE、PLAN_CONFLICT 与已有源日志/传播错误。不要把限速造成的等待记录为 corruption，也不把整机 TPS 差异归类为通知成本。

## 16. 分阶段交付与验收

| 阶段 | 交付 | 完成条件 |
| --- | --- | --- |
| A：基线与 API 验证 | Release 重复基线、分类覆盖、PFS/原生 group 生命周期验证、fallback 策略验证 | 每个实际执行线程可追踪；不支持的后端明确拒绝；能恢复会话资源组 |
| B：推荐第一版 | native CPU 分区、AP try-admission、后台公平 active 并发、OBSERVE/SOFT/STRICT_CPU、指标 | TP/AP CPU 核集合与配置一致；query-demand 不绕过额度；KILL/unload/shutdown 无死锁 |
| C：完整进程内预算 | 有界 apply pool、SYNC/MAINT memory 与 I/O、可选 CPU cgroup 后端 | 线程数/队列内存有界、无热点表饥饿、合法大事件能推进、I/O 覆盖边界明确 |
| D：增强隔离 | NUMA/设备部署、剩余共享资源分析；如需要，设计跨进程/节点 | 对尚不能消除的 TP/AP 双向干扰有测量和具体部署保证 |

测试包括：混合会话 AP->TP 资源组恢复；ON primary retry 前释放 Rapid 配额并恢复原组；FORCED 资源忙原因；活跃查询修改/禁用/drop group；容器 CPU 收缩和权限缺失；同表顺序与跨表原子可见性；预算不足/大 event/压缩 payload；等待时 unload/DDL/KILL/shutdown；CAPTURE 不引入等待；worker 故障、热表公平、低额度下必要 GC。

性能至少覆盖 TP only、hint-only、SYNC active、TP+AP+SYNC、load/reload/maintenance 压力，使用相同耐久化设置、数据规模、暖机和随机/平衡顺序。Release 下每组建议至少 10 次、每次稳定区间至少 60 秒；这是实验设计建议，不是已经完成的测量。

CAPTURE 与 COMMITTED_BINLOG 必须分别完成相同矩阵，禁止用一种模式的结果代表另一种：

| 配置 | 无后台消费的前台对照 | 正常 SYNC | TP+AP+SYNC | AP 资源不足后主引擎 fallback |
| --- | --- | --- | --- | --- |
| 隔离关闭 | 两种模式各测；暂停点需验证，测后恢复并核对结果 | 两种模式各测 | 两种模式各测 | ON/FORCED 分别验证 |
| 隔离开启 | 相同 TP CPU 集合与持久化配置 | 两种模式各测 | 固定 AP 负载及预算，两种模式各测 | 验证回到系统资源后的 TPS 与尾延迟，单独报告 |

暂停消费者的实验必须确保队列不溢出、没有故障隔离和隐藏 fallback，并验证恢复后所有变更一致；CAPTURE 的前台行镜像构造和缓冲入队属于其传播成本。正常 SYNC 的整机差异包含后台解析/apply 和共享资源消耗，不能标为纯 commit 路径损耗。另分别记录 KILL/unload/shutdown 完成时间和预算清零，避免只报告 TPS 而遗漏生命周期失败。

同时测 TP TPS/commit p95/p99、AP latency/throughput、freshness/lag 与实际 CPU/memory/I/O。TP 隔离开启后的对照基线应在同一个 TP 预留 CPU 集合上运行；AP 也使用同一 AP 保留容量，避免把减少核数的差异误算为干扰。

已有 #932 的 24 轮 Debug 测量只作对照，不能推导固定回归比例或已经零开销。建议验收目标由你在 Release 基线后确认：TP 吞吐与尾延迟的最大容许变化、AP 的最低服务能力，以及允许的最大同步 lag。阶段 B 不能对内存带宽/设备争用承诺绝对互不影响。

## 17. V1 范围与当前状态

按已确认路线合并阶段 A/B：复用 MySQL 原生 CPU 资源组，加入 Rapid 查询准入和 SYNC/MAINT 活动并发预算，提供分类、配额、拒绝原因，并复用已有传播状态观测；先验证生命周期和 Release 基线，再验证资源不足、fallback、KILL、unload 与 shutdown。

采用上一节的系统资源 fallback 策略。cgroup CPU 后端、有界物理 apply 线程池、独立 I/O 和更强进程/节点隔离属于后续范围。当前分支已接入 governor、native group 作用域、binlog reader/入队/apply 和维护入口；构建、功能回归与重复性能测量已完成，具体记录见第 19 节。

## 18. 首版运行接口与边界

- `rapid_resource_isolation`：OFF（不限制、不切组）、OBSERVE（仅统计）、SOFT（独立并发预算，可选 native CPU 组）、STRICT_CPU（预算 + 校验后的 native CPU 分区）。模式切换要求所有 Rapid 表 unloaded，且没有 active/waiting 工作；limits 和组名称是启动参数。
- `rapid_resource_ap_limit=4`、`rapid_resource_sync_limit=2`、`rapid_resource_maint_limit=1`：限制活动操作；FIFO 排队发生在后台，AP/foreground load 非阻塞准入。保留现有查询内存/算子/spill 预算，不把并发限制称为 RSS/I/O 隔离。
- `rapid_resource_tp_group`、`rapid_resource_ap_group`：USER 组；`rapid_resource_sync_group`、`rapid_resource_maint_group`：SYSTEM 组；`rapid_resource_maint_user_group`：前台 load/reload 使用的 USER 组。STRICT_CPU 下两个 MAINT 组须使用同一 CPU 集合。
- `SHOW GLOBAL STATUS LIKE 'rapid_resource_status'`：每类 active/waiting/peak、AP rejected、后台 cancelled、CPU 恢复失败数。peak 是进程生命周期峰值，不能直接作为后续模式切换后的限额违规证据。

DBA 先用 MySQL 原生 CREATE RESOURCE GROUP 建组，再通过启动参数指定名称。STRICT_CPU 不覆盖 DBA 现有线程绑定：部署前须将全部普通源端线程（含未被 PFS 登记的系统线程）限制到 TP 组定义的 CPU 分区。Linux 可通过 `taskset -apc <TP CPU list> <mysqld PID>` 配置现有线程，并保证新普通线程继承该分区。启用时和申请 CPU 作用域时审计 `/proc/self/task`；拓扑在显式验证计划时刷新缓存，每次作用域仍校验组定义和实际 affinity。新 worker 登记与审计发生竞争时，释放审计锁并限时重试完整校验，不放行持续未分类的线程；未覆盖、物理核/SMT 重叠、组缺失/类型错误或实际 affinity 不匹配均报具体原因，禁止声称已隔离而悄悄使用全机 CPU。显式修改部署 CPU/cpuset 或第三方线程绑定需重新验证计划。

后台 scope 的原始 affinity 连同原资源组一起恢复，避免 native default 组恢复时把外部配置的 TP CPU 限制意外扩大。fallback 使用进入 Rapid 前的系统/会话资源，不保留 Rapid AP 配额；它可以再次造成 TP/AP 争用，需单独测量。外部 SET RESOURCE GROUP 若已覆盖作用域绑定，则保留管理员的新设置。组与原组通过非阻塞 MDL pin 防止执行中 ALTER/DROP；原绑定恢复失败计数并终止对应 session，避免后续 SQL 继续使用错误绑定。

SYNC 包括 coordinator 的提交状态整理/队列分派、committed reader 的 decode，以及每表 apply。apply 每批最多 256 条或约 1 MiB（单条大记录仍能推进），保留表内 FIFO，释放 grant 后让其他表公平推进。reader 在打开源表前取得 decode grant，关闭源 TABLE/MDL 后释放它；随后取得独立的 SYNC 入队 grant，缓冲区满时先释放 grant 再等待空间，所有入队结束后释放 grant 才等待 apply，保证 SYNC=1 可以推进；每个有界解析批次保留事务顺序和独立 SCN，入队后只等待各表的最后屏障，所有表完成后才一起发布该批次的可见位置，避免逐事务等待造成反复准入和 CPU 切组；generation 校验与 enqueue 配对，发布屏障持有原 buffer，unload/reload 不会误投旧批次或释放新 buffer 的屏障。

维护入口覆盖 load/partition load 与 GC/compaction/stats 工作池。SOFT/STRICT_CPU 中 load 使用现有串行扫描，宽表 AP 使用当前已分类 THD 上的同步扫描；避免内部共享池或嵌套并行绕过预算。OFF/OBSERVE 保留既有并行行为。有界物理 apply 线程池、更细粒度的并行预算、I/O/NUMA/cgroup 后端和其他 ML/监控任务的专项预算仍属后续范围。

MySQL 普通语句已有 KILL/fatal 后禁止 fallback 的检查；prepared statement retry 补齐同样条件。KILL 不得清除取消错误并转为主引擎执行。shutdown 先关闭 governor 并唤醒等待者，再按 producer→consumer 顺序销毁引擎对象。

## 19. 实现验证记录

Debug 功能构建和 169 项相关单元测试通过，其中 22 项 resource tests 包括新增的 FIFO、取消、停止、移动 lease 与独立类别配额测试。secondary_engine 完整运行覆盖 242 项检查：239 项通过，3 项因新增配置/状态名称造成快照差异；修正后，包含这些用例、全部 8 个新增隔离 MTR、large_data_index/vectorized 和 sys_vars.all_vars 的 15 项定向检查全部通过。STRICT_CPU 的 SYNC=2 用例与 main.mysqld--help-notwin 又分别连续通过 5 次。枚举配置、批次等待优化和入队 SYNC 分类后的 Debug/RelWithDebInfo 构建完成，169 项单元测试重新通过；全部 8 个资源隔离 MTR 与 9 个 committed binlog 回归，加上 shutdown_report，共 18 项检查全部通过。

新增 MTR 核对 ON primary fallback、FORCED 具体拒绝原因、原资源组恢复与 MDL pin、普通/prepared KILL、SYNC=1 跨表提交可见性、capture/committed-binlog 等待配额时 unload，以及同步进行中的 shutdown/restart。shutdown 验证发现并修正恢复会话的重复 external unlock 和未关闭 TABLE 引用；后台源表使用者现在在 pre_dd_shutdown 阶段停止并 join。

性能矩阵使用同一组 TP 预留 CPU：TP 0–3、AP 4–5、SYNC 6–7、MAINT 8–9，压测客户端使用 10–11。分别测试 unloaded baseline、两种传播模式的 OFF/STRICT_CPU 正常同步，以及固定一条 FORCED AP 会话的混合负载。每个测量组交替顺序重复 3 次，预热 5 秒、计量 30 秒；逐轮核对源端和 Rapid 的 COUNT/SUM。该主机向 Linux 暴露 12 个 vCPU，未控制 hypervisor 的实际物理核映射，因此这些数据不能证明物理机器的缓存/NUMA 隔离效果。

首轮优化构建的 42 组对照均核对一致，但 committed binlog 严格隔离的混合组 AP 中位吞吐仅 0.38 QPS、p95 中位 6135.91 ms（OFF 为 99.85 QPS、13.95 ms）。此轮没有实际独立预热：本机 sysbench 接受未实现的 `--warmup-time` Lua 参数而没有执行预热，故仅保留为定位对照。发现 reader 逐事务等待 apply 屏障造成频繁准入/CPU 切组，已改为有界解析批次末尾统一等待并发布，入队整理也纳入 SYNC grant，空间不足先释放 grant 再等待。新增 600 个跨表事务覆盖多个解析批次的 SYNC=1 回归；新版构建和独立 5 秒预热的正式测量已完成。并发编译期间的 Debug 短跑仅验证测试部署和一致性，不作为 TPS 数据。正常同步和混合负载的吞吐变化包含后台工作，不能当作纯 commit 路径开销；本轮不声称实现 RSS、I/O 或跨节点隔离。

正式测量共 42 组，全部源端/Rapid COUNT/SUM 一致，`cpu_restore_failures=0`。每组独立运行 5 秒预热，再计量 30 秒，随机种子 932，重复 3 次。TP 使用 10,000 行表的随机主键 autocommit UPDATE，混合组增加一条 FORCED 会话，对另一张 100,000 行静态表执行 COUNT/SUM；AP 无主引擎 fallback。以下 p95 为完整 SQL 事件耗时，不是仅 commit 阶段的耗时。

| 正常后台同步配置 | TP 线程 | TPS 中位数 | TPS 范围 | TP p95 中位数 (ms) |
| --- | ---: | ---: | --- | ---: |
| BASELINE | 1 | 474.90 | 469.14–485.76 | 2.81 |
| CAPTURE_OFF | 1 | 449.19 | 349.99–461.77 | 3.19 |
| BINLOG_OFF | 1 | 462.78 | 399.09–476.25 | 3.02 |
| CAPTURE_STRICT | 1 | 473.20 | 313.76–482.49 | 2.86 |
| BINLOG_STRICT | 1 | 460.42 | 449.35–469.42 | 3.02 |
| BASELINE | 4 | 1262.99 | 1248.06–1333.75 | 4.65 |
| CAPTURE_OFF | 4 | 1292.20 | 1286.51–1310.37 | 4.33 |
| BINLOG_OFF | 4 | 1223.77 | 1191.45–1264.93 | 4.91 |
| CAPTURE_STRICT | 4 | 1294.85 | 1237.27–1315.46 | 4.33 |
| BINLOG_STRICT | 4 | 1120.89 | 1070.69–1131.27 | 5.67 |

| TP+AP 混合配置（4 TP 线程） | TP TPS 中位数 | TPS 范围 | TP p95 (ms) | AP QPS 中位数 | AP p95 (ms) |
| --- | ---: | --- | ---: | ---: | ---: |
| CAPTURE_OFF | 1037.98 | 1034.98–1071.78 | 6.09 | 143.32 | 8.43 |
| BINLOG_OFF | 1016.93 | 1004.93–1077.27 | 6.21 | 113.50 | 11.65 |
| CAPTURE_STRICT | 1055.53 | 1048.66–1098.65 | 5.77 | 123.07 | 10.84 |
| BINLOG_STRICT | 1008.89 | 1005.34–1018.04 | 5.99 | 81.49 | 16.71 |

4 线程下 STRICT_CPU 相对同传播模式 OFF：正常后台同步 capture TPS +0.21%、committed binlog −8.41%；混合负载 capture +1.69%、committed binlog −0.79%。单线程样本波动较大，capture 各组最低样本降到约 314–350 TPS，不能据 3 次重复推导固定开销或因果结论。批次优化后的 committed binlog 严格隔离 AP p95 三次为 16.71、16.71、15.55 ms，未再出现首轮的秒级积压。

CPU 分区并不隔离共享设备 I/O、内存带宽、缓存和源端元数据路径；严格准入审计及 native group 切换也有后台成本。剩余 TPS/AP 差异保留为 #932 后续优化依据，需在受控物理核环境中进一步 profiling，再推进阶段 C/D 的有界工作池、内存/I/O 预算与更强隔离；本轮没有证明任何一种模式实现零开销。

更新 origin/upstream main 并 rebase 后，最终源码树与已测试版本完全一致；main.mysqld--help-notwin、sys_vars.all_vars、shannon_params、shannon_parallel_load、STRICT_CPU、600 事务跨批次同步及 shutdown_report 共 7 项复核全部通过。
