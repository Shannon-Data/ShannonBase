# Rapid 发布 review 源码复核

本次 R01–R09 指用户提供的发布 review 编号。`REVIEW_FIX_STATUS.md` 已不存在，原 review 的相关引用不能作为当前证据。以下调用链来自源码静态复核，不是实际崩溃栈；回归结果另行记录。

| Issue | 判定 | 依据及修复/验收要求 |
| --- | --- | --- |
| R01 | 原始缺陷已修正，压力资格待验证 | populate/log_dml_notification.cpp 的 TransactionManager::erase_if_complete_locked 在全部登记变更和物理 finalizer 完成后调用 observe_commit_scn，而不是 source COMMIT callback 直接推进。worker 使用 get_gc_safe_scn 回收；InnoDB 活跃 read view 会保守阻止回收。shannon_review_capture_gc 已覆盖仅在 InnoDB 建立的 RR 快照保护旧版本、释放后 GC 继续推进；持续写入内存稳定性仍需验证。 |
| R02 | 具体异常逃逸路径成立 | 自动维护调度没有 catch；worker 重试入队和 scheduler ready 入队在任务 catch 外；binlog catalog 复制和异常处理中的 quarantine 也可能逃逸。本次保护维护 pass、两处队列分配和 consumer 外层异常，并回滚失败任务入队留下的登记及去重键，允许后续维护重新调度。外层失败设置全局 fault，查询拒绝 offload，全部 unload 后 reload 恢复。该兜底不等于 durable quarantine，也不代表所有线程异常已闭环。 |
| R03 | “窗口没有统一计费”不成立 | window_iterator.cpp 构造时调用 reserve_query_memory，运行时容器使用预算内 PMR；SpillRecord 有单算子及全局 window 临时字节限制；Init/Read 处理预算耗尽。仍需并发、低预算、取消及磁盘故障验证，MySQL 表达式求值和扫描等不因此获得 RSS 约束。 |
| R04 | 实现描述过时，暂停及进展风险仍在 | table_persistence.cpp::checkpoint 捕获边界后释放全体 mutation locks，逐 IMCU 流式写文件，最后验证版本。该路径没有全表快照字节同时驻留的证据；初始/最终全体锁获取、逐 IMCU 写入阻塞和持续变更导致 checkpoint 重试仍需量化。本次修正文档。 |
| R05 | 预算边界判断基本正确 | res_mgmt.cpp::reserve_query_memory 接通 operation/query/query-total 限额。应逐项列举接入算子，不能推导整个 mysqld RSS 上限。 |
| R06 | batch head-of-line 等待成立，查询已有独立 deadline | consume 在所有 touched table barrier 完成后发布 consumer.visible。源 TABLE/MDL 与 enqueue resource scope 在 barrier 等待前释放，因此不能仅由该等待断言 OLTP 被同步阻塞。consumer barrier 的等待 slice 不是总等待 deadline；prepare_query、CAPTURE 扫描及 COUNT 使用 rapid_query_sync_timeout_ms 限制查询等待。BROKEN/GONE/quarantine 不消除健康表对后续 batch 的长期阻塞。不能跳过表后发布跨表可见状态；需保留原子可见性，以压力结果确定调度或超时策略。 |
| R07 | 运行时及持久化撤销已有实现，故障闭环仍需资格验证 | 当前 recovery/README.md 明确 native checkpoint + committed MySQL binlog，没有 Rapid 独立 capture WAL。恢复在未发布表上完成验证/replay，失败 reload。QuarantinePropagationTable 先拒绝读取，再调用 revoke_fast_recovery；后者禁用 notification tracker、调用 store invalidate 并写 taint marker（表目录失败时使用独立目录）。provider 抛异常/返回失败的单测只证明当前进程的 recovery fence，不能证明所有持久化失败后的重启行为。原 review 对已删除文档的引用不能作为当前证据。 |
| R08 | 缺少 SLA 资格证据的结论合理 | barrier/重试/配额是机制，不是 p99 结果。需先定义数据量、并发、写入率、事务规模、磁盘条件、freshness/p99 目标和观察时长，再测量。 |
| R09 | 验收要求，尚非具体 bug | 未指出哪个参数失效或哪个计数器失真。逐项改变参数、触发真实分配/回收/等待/失败，以行为和计数增量验收；缺少证明不能直接等价于实现错误。 |

发布判断：可继续开发及在明确能力范围内受控试点；本次复核不能支持通用生产 GA 放行。代码缺陷、回归结果和生产资格须分别记录，不能将 R01–R09 全部描述为仍存在的代码 bug。

本次新增 secondary_engine.shannon_committed_binlog_thread_exception：catalog 构建前注入 bad_alloc，验证 mysqld 存活、FORCED 查询失败、ON 查询回退保留源提交、全部 unload/reload 后 FORCED 查询再次成功。


## 本次代码修复的调用链及边界

### R02：后台分配异常

- 问题：维护调度、任务重试/ready 入队及 binlog catalog 复制可在原有任务级 catch 外抛出；异常处理中的 quarantine 也可能再次抛出。
- 根因：异常保护范围没有覆盖调度及 catalog 构建；任务提交失败后可能残留去重键。catalog 构建前缺少 MySQL 线程状态，使该位置的 DBUG 注入不生效。
- 调用链：`auto_maintenance_thread → schedule_gc/schedule_compact/schedule_stats_update → submit_unique → schedule_locked`；`worker_loop → handle_task_result_locked → schedule_retry_locked`；`scheduler_loop → enqueue_ready_locked`；`CommittedBinlog::attach → consume → catalog copy / quarantine`。
- 修复状态：维护 pass 失败后下次重试；重试/ready 队列失败后将任务记为失败并释放登记/去重键；提交失败回滚登记/去重键。consumer 每轮初始化线程状态并以 scope guard 清理，外层 catch 设置全局 fault 并唤醒查询，拒绝继续 offload，直到所有表 unload 后 reload。
- 最终判定：这些具体异常路径已补保护；未据此宣称全部后台线程、线程创建、系统互斥锁错误和极端 OOM 已完全闭环。consumer 全局 fault 是运行时 fail-closed，不等于持久化 quarantine。

### COPY_INFO 的来源与模式分层

- 问题：原 `Source::COMMITTED_BINLOG` 与 `Source::COPY_INFO` 都由同一个行镜像 parser 消费，来源枚举混入了传播模式；“recovery-only” 注释也与在线 binlog consumer 不符。
- 根因：记录格式与生产模式使用同一标签。
- 调用链：notification hooks / `BinlogParser::read_batch → EnqueueRowChange → PopulatorImpl::write_impl → table_worker_func → CopyInfoParser::apply_change`；恢复另由 `BinlogRecovery::replay → CopyInfoParser::apply_change` 消费。
- 修复状态：`Source` 仅保留 UN_KNOWN/REDO_LOG/COPY_INFO；COPY_INFO 记录通过 `CopyInfoMode::NOTIFICATION/COMMITTED_BINLOG` 保存生产模式，在线解码与恢复回放都设置该模式。队列 dispatch 统一走 COPY_INFO；事务身份验证、outcome 回调和队列满重试继续按记录模式区分，不读取全局配置猜测已入队记录。
- 最终判定：保留 notification 的主库事务身份和非阻塞失败隔离；保留 binlog producer 的已提交系统版本及后台 backpressure。SQL 参数 `CAPTURE/COMMITTED_BINLOG` 保持兼容。

### R04：checkpoint 文档纠正

- 问题：原文称全量 native snapshot bytes 在全体锁内固定后再写文件，与现实现不符。
- 根因：文档没有随 optimistic streaming checkpoint 更新。
- 调用链：`TablePersistenceManager::checkpoint → capture boundary/versions → serialize_imcu → validate versions/boundary → sync_file → persist_manifest`。
- 修复状态：README 已明确逐 IMCU 写文件、最终一致性验证、写入持续发生时可丢弃 generation；初始和最终阶段仍需获取全体 mutation locks。
- 最终判定：纠正旧实现描述；没有承诺最大写入暂停或 checkpoint 完成时间。

## COUNT 专用 helper 的作用域

`ha_rapid::records` 的 `prepare_count_scan`、`wait_for_query_watermark`、`barrier_error` 均改为函数内 lambda，`WaitOutcome` 改为局部类型。六条既有消息集中到 `RapidErrMsg` 数组，通过枚举索引访问并校验数量；错误文本及等待/事务语义保持一致。

## 验证记录

本次验证结果：

- Debug `mysqld` 编译通过；`git diff --check` 通过。
- 7 个既有 MTR 用例通过：`shannon_committed_binlog`、`shannon_committed_binlog_apply_progress`、`shannon_committed_binlog_restart`、`shannon_recovery_pending_commit`、`shannon_review_capture_gc`、`shannon_review_window_budget`、`shannon_count_star_wait_races`。
- 新增 `shannon_committed_binlog_thread_exception` 通过，验证 catalog 前 `bad_alloc` 触发外层 catch、mysqld 存活、FORCED 查询拒绝、ON 回退保留源提交、全部 unload/reload 后 FORCED 查询恢复。首次注入未触发，补线程状态初始化后业务断言通过；测试还需恢复原始 GLOBAL debug 才能通过 MTR 状态清理检查，修正后独立重跑通过。
- 以上合计 8 个目标用例通过，来自最终代码的两次运行；不把 shutdown_report 计为业务回归，不把历史记录计入本次结果。
- 维护队列的分配失败保护本次完成源码检查及编译，未进行每个队列分配点的故障注入。以上测试也不证明 p99、freshness SLA、真实 RSS 上限或长期生产稳定性。

运行日志：`/tmp/shannon-review-fault-build.log`、`/tmp/shannon-review-mtr.log`（7 个既有用例通过，新增用例当时因状态清理失败）、`/tmp/shannon-review-fault-mtr.log`（修正后的新增用例通过）。

## 2026-10-13 续审

复核基线为 `5416fe792` 加本次未提交修复；当前工作树和本机 Debug 二进制的结果不能等同于干净检出的发布包验收。未运行完整 secondary_engine suite，也未执行生产负载或 SLA 资格测试。

### 本次发现与修复

1. **源码交付缺文件**：已跟踪的 `populate/log_populate.cpp` 引用了尚未跟踪的 `utils/try_operation.h`。该文件不是 CMake 生成文件，发布提交/源码包必须包含它；本地构建成功不能解除此交付阻断。复核文档本身也尚未跟踪，需要随交付材料纳入版本管理。
2. **超大事务错误原因回归**：`BinlogParser::read_batch` 的表参与者发现阶段先按原始行镜像字节检查事务预算，但失败时没有覆盖默认的“missing, corrupt or unsupported”错误。因此 `shannon_committed_binlog_oversize` 的安全拒绝、ON 回退及 reload 恢复均正常，准确错误信息断言失败。本次让发现阶段和解码阶段复用预算错误返回，保留原有预算及 fail-closed 行为，未修改测试预期。
3. **历史全套结果不可放行**：`/tmp/shannon-secondary-release-mtr.log` 记录 oversize 失败和 suite timeout，只执行到部分用例；不能将其中的通过项解读为完整 suite 通过。本次重跑没有复现历史超时，但这不足以解释历史超时根因。

### 发布前仍需验收

| 项目 | 现有证据 | 剩余验收 |
| --- | --- | --- |
| R01 GC | 仅 InnoDB 建立的 RR 快照保护旧版本，COMMIT 后回收恢复的目标 MTR | 持续写入和长快照混合压力下的版本量、回收速率、内存趋势；活跃主库 read view 会保守停止回收，应量化影响 |
| R02 后台异常 | consumer catalog、notification、transaction 异常及 OOM reload 目标用例；provider 异常/失败单测 | 维护队列、线程创建和共享 apply 重调度失败逐点注入，确认存活、隔离、登记清理及 unload/shutdown 完成 |
| R03/R05 预算 | window/sys_pool 目标用例及资源管理单测 | 并发竞争、取消、临时磁盘写满/写失败与清理；不能从预算推出整个 mysqld RSS 上限 |
| R04 checkpoint | 逐 IMCU 流式序列化与版本验证的源码证据 | 持续变更下 checkpoint 成功率、重试率、写入暂停及峰值内存 |
| R06 可见性/等待 | 查询 deadline 源码；CAPTURE scan/COUNT 超时及无关表锁用例 | apply 长期停滞、跨表大事务下的整体进度与 freshness；consumer barrier 尚无总 deadline |
| R07 恢复 | store invalidate、双目录 taint marker、两处恢复入口检查的源码；provider 失败时运行时 fence 单测 | store invalidate 失败、两个 marker 路径不可写/不可读、崩溃重启的组合故障注入，确认旧 checkpoint 不被错误发布 |
| R08/R09 资格/可观测性 | 机制与部分计数器断言 | 明确规模、并发、写入率、事务规模、磁盘条件、目标 p99/freshness 和持续时间，逐项检验参数实际行为和计数变化 |

发布判断维持：本次证据不支持通用生产 GA；先补齐源码交付、最终回归和上述资格证据。既有修复有效与未取得生产资格是不同结论。

### 本次最终验证

- Debug `mysqld` 增量编译通过；`git diff --check` 通过。
- 修复后的新二进制独立重跑 **10 个目标 MTR 全部通过**：`shannon_committed_binlog_oversize`、`shannon_committed_binlog_thread_exception`、`shannon_propagation_oom_reload`、`shannon_notification_exception`、`shannon_transaction_exception`、`shannon_review_capture_gc`、`shannon_review_capture_timeout`、`shannon_review_binlog_table_lock`、`shannon_review_window_budget`、`shannon_sys_pool`。MTR 汇总的 11 项包括 `shutdown_report`，业务回归数为 10。
- 现有 `rapid_resources-t` **36/36**、`rapid_recovery_protocol-t` **10/10** 通过。使用的是本机已有单测二进制；本次源码改动仅涉及在线 binlog parser，不宣称重新构建了单测目标。
- 首轮结果为 9 个业务用例通过、oversize 错误文本失败；最终结果来自新二进制的单次完整目标运行，不混合首轮结果。
- 日志：`/tmp/shannon-release-reassessment-20261013-build.log`、`/tmp/shannon-release-reassessment-20261013-mtr.log`（首轮）、`/tmp/shannon-release-reassessment-20261013-final-mtr.log`（最终）、`/tmp/shannon-release-reassessment-20261013-resources-gunit.log`、`/tmp/shannon-release-reassessment-20261013-recovery-gunit.log`。

## COMMITTED_BINLOG 返回值失败处理

本次进一步修改 `log_binlog_parser.cpp`，移除 consumer 的 batch catch、线程外层 catch 及主动 throw。`BinlogParser::read_batch` 和 `EnqueueRowChange` 对外提供不抛异常的返回值接口；STL/MySQL/provider 的抛异常调用通过底层 `Utils::try_operation` / `try_bool_operation` 转为失败状态。后者保留操作本身返回 false 的语义，不把“没有抛异常”误认为业务成功。

目录复制、source metadata/decode 和事务 notification/fence 准备分别提供返回值函数，consumer 根据失败结果选择全局 fault 或逐表隔离。事务准备只有在全部登记和 buffer 分配成功后才递增 fence；中途失败不入队、不推进可见水位，并撤销当前 catalog 的恢复资格。隔离失败升级为全局 fault，cursor 复制失败也设置 fault。新增单表隔离接口避免构造临时 vector。

`rapid_committed_catalog_bad_alloc` 现在直接注入失败返回，原有 `shannon_committed_binlog_thread_exception` 继续验证 consumer fault 和 reload 恢复，但不再宣称该用例实际抛出了 bad_alloc。实际 provider 抛异常及资源析构由 `RapidOperationStatus.ProviderExceptionUnwindsResourcesAndReturnsFailure` 单测验证。新增 `shannon_committed_binlog_prepare_failure` 覆盖 notification 已登记但 fence 尚未建立时失败、源提交保留及 reload 恢复。

本次交付包含此前缺失的 `utils/try_operation.h`；源码缺文件的历史阻断在该文件随提交纳入版本管理后解除。完整 suite、持续压力和持久化组合故障重启资格仍未取得。

本轮验证：最终 Debug `mysqld` 构建通过；重新构建的 `rapid_recovery_protocol-t` 共 12 个单测通过（10 个恢复协议、2 个状态适配器）；最终二进制的 16 个目标 MTR 全部通过，覆盖 COMMITTED_BINLOG 基本读写（含 Blob）、apply progress、commit boundary、corruption、oversize、catalog failure、prepare failure、restart、snapshot、mode concurrent、COUNT wait races、pending commit recovery、apply pool、CAPTURE GC、notification exception 和 transaction exception。`shutdown_report` 不计入业务用例数量；`git diff --check` 通过。

日志：`/tmp/shannon-binlog-status-final-build.log`（mysqld 与单测目标）、`/tmp/shannon-binlog-status-leaf-build.log`（Blob 分配保护下移后的最终 mysqld）、`/tmp/shannon-binlog-status-gunit.log`、`/tmp/shannon-binlog-status-final-mtr.log`。最终 MTR 曾因磁盘不足在复制测试数据时退出，未执行业务用例；清理本次生成的临时测试目录后独立重跑通过。早一轮的 16 个目标测试通过记录保留在 `/tmp/shannon-binlog-status-mtr.log`，不混入最终结果。

## 持久化失败重启与 consumer 停滞续修

本节更新此前 R06/R07 判定；历史验证记录保留。基线为 `e7dd9bf98`。磁盘清理仅删除生成的测试数据库/临时目录，保留日志、模型、源码和构建产物，释放约 11 GB；清理清单 `/tmp/shannon-space-cleanup-20261013.log`。

- R07：store invalidate 失败/抛异常不会再跳过本地 marker。主 marker 写失败继续尝试备用目录；检查错误默认拒绝快恢复；provider restore 前检查 marker，避免 restore 擦除失效证据。新单测验证 provider 失败后 marker 保留并由新 manager 拒绝恢复；新 MTR 验证 provider 与主 marker 同时失败时备用 marker 在 crash 后触发主库 reload，以及 marker 检查失败时 reload。健康 binlog checkpoint crash 快恢复用例仍通过。
- R06：后台 reader 重试、资源准入、队列满及 apply barrier 共用无进展期限。期限复用 `rapid_query_sync_timeout_ms`（默认 30 秒），不是单次 wait slice；catalog 重试不能重置期限。超时隔离当前 catalog 中加载表，不推进可见水位/游标，ON 回退、FORCED 拒绝，全部 unload/reload 后恢复。期限是协作检查，不能打断阻塞 syscall。现有配置现在同时影响查询同步等待和后台隔离，应根据负载设置，过短会降低 AP 可用性。
- TP 优先：未增加每笔源 DML 的 DD 更新、前台 flush 或 AP apply 等待。测试中的 2 秒提交断言仅证明注入停滞时 TP 没有等待 apply，不是延迟 SLA。既有故障路径的同步撤销 I/O 仍存在；不能将其无证明地异步化。
- 未解决：无可靠 binlog、provider 和两处 marker 全部无法持久化之后崩溃，仍缺少主库事务内的 durable source witness。没有宣称此组合故障已闭环，没有采用所有崩溃强制 reload 或拒绝 TP 提交。GC/checkpoint/SLA 仍需测量，未以缺少资格证据推断实现有缺陷。

详细主库事务资格设计、HeatWave PDF 与 MySQL 官方 commit message/公开补丁证据见 [FAILURE_RECOVERY_DESIGN.md](recovery/FAILURE_RECOVERY_DESIGN.md)。DD secondary_load 是恢复意图；after-commit GTID 排除集是已提交 binlog 恢复线索，二者都不足以证明未日志化 DML 的 checkpoint 资格原子性。

最终 Debug `mysqld` 与 `rapid_recovery_protocol-t` 构建通过；13 个单测通过（11 个恢复、2 个状态适配器）；独立非 record 运行的 12 个目标 MTR 全部通过（另有 shutdown_report）：4 个新增故障用例，以及 storage_layer_binlog/unlogged、committed_binlog_restart/snapshot/prepare_failure/thread_exception、review_binlog_table_lock、recovery_pending_commit。新增用例先 record，再由最终二进制独立比对。完整 suite、持续压力和通用 GA 资格仍未取得。

日志：`/tmp/shannon-recovery-stall-final-build.log`、`/tmp/shannon-recovery-stall-gunit-final.log`、`/tmp/shannon-recovery-stall-record.log`、`/tmp/shannon-recovery-stall-final-mtr.log`。

## 资格失效时停止自动恢复，等待手工 SECONDARY_LOAD

用户明确 TP 优先、失败表手工 load。本轮对 `recovery_required`、taint 或检查错误在源 MDL 获取前 warning 并返回失败，保留未发布状态；self-load 同样拒绝这些表，避免自动重建绕过用户决定。主库提交语义未改变；未修改普通无 checkpoint/日志已清理的 fallback，健康 fast recovery 继续读取 Rapid 原生 checkpoint 并 replay committed binlog。旧 checkpoint 与主库最新状态之间缺少 durable witness 的不可识别窗口仍未修复，不能把未出现 marker 当作新增资格证明。

手工 load 一致性：SQL 层持有排他 MDL，覆盖扫描、AP 注册与传播 attach，源写入在这个窗口等待；新并发 MTR 验证加载中表没有发布，等待写入在加载结束后成功，并由传播路径反映到 AP。显式 load 因此不是零 TP 阻塞的在线加载，本轮未降低锁强度或改变这一协议。

官方 HeatWave commit message 与本地 `hw` PDF 章节已补入 FAILURE_RECOVERY_DESIGN：原生持久化 change packet、DD load 意图、GTID/binlog iterator 恢复，以及手册 SNAPSHOT_SCN/PERSISTED_SCN。MySQL 普通复制位置系统表与 InnoDB undo 到 gtid_executed 的 durable handoff 是相关参考，未将其当作 HeatWave 私有协议或直接修改主库实现。

最终 Debug mysqld 构建通过；7 个新增/调整用例 record 后，独立非 record 的 **11 个目标 MTR 全部通过**（另有 shutdown_report），包含资格撤销/检查失败、provider 不可用、未日志化/弱持久化、uncertain commit 的手工 load，新增并发 load 一致性，以及健康 checkpoint/binlog/pending commit 和 self-load restart recovery。`git diff --check` 通过。未新增 gunit 或执行完整 suite/长期压力。

日志：`/tmp/shannon-manual-recovery-complete-build.log`、`/tmp/shannon-manual-recovery-record3.log`、`/tmp/shannon-manual-recovery-final.log`。前两轮发现测试错误码/预期 warning 未豁免，以及线程 DBUG 初始化位置问题；修正后结果来自最终代码的独立运行。

## 原生 storage fast recovery 与持久化水位

基线 `d507a70bf`。保留 Rapid 原生列式 checkpoint 恢复及 committed binlog 回放，避免重新扫描 TP 和转换格式。manifest v3 保存 SNAPSHOT_SCN/PERSISTED_SCN；v2 兼容读取，缺失水位保持未知值 0。checkpoint 捕获稳定数据边界，文件、manifest 和 provider 发布成功后才推进展示水位；provider 返回失败或抛异常按返回值处理并清理候选 generation。新 load 重置旧持久化水位；快恢复恢复 manifest 水位并校准 Rapid SCN 时钟。

新增本地发布失败和 provider 发布失败用例：较新源变更已在 AP 可见，发布失败后仍从旧原生 checkpoint 恢复并回放，水位不虚增。新增 3 个 manifest 单测覆盖不同快照/持久化 SCN、v2 未知水位和非法水位。Debug mysqld 与测试目标构建通过；16 个 gunit 全通过；record 后独立运行的 15 个目标 MTR 全通过（另有 shutdown_report），覆盖上述新增用例、普通日志缺失/损坏回退、资格撤销后手工加载、并发加载一致性、pending commit、自加载恢复及 consumer 停滞。Rapid 目录按用户指定的两条 find/xargs clang-format 命令完成格式化。

官方恢复页面的原生存储优先、日志缺失等情况下源端 fallback 是本轮对照边界；未实现 OCI/S3 provider 或分布式 HA，也未公开推断 HeatWave 私有原子协议。AP 持久化水位不是未日志化源提交的 durable witness；所有撤销证据无法落盘的不可识别窗口仍未闭环。完整回归、长期压力和 SLA 资格未由这些定向测试替代。

构建日志：`/tmp/shannon-heatwave-boundary-verified-build.log`；gunit：`/tmp/shannon-heatwave-boundary-gunit.log`；MTR 日志目录：`/tmp/shannon-heatwave-boundary-final/log`。

## 按表恢复正确性、发布清理与幂等性续审

分支 `fix/rapid-recovery-lost-revocation`，基线 `185dd3644`。保留现有原生
checkpoint 校验、绑定 binlog 边界验证、完整 committed transaction replay 和
发布前 FULL 扩展接口；不实现 TP 全表比对、不用 GTID 与 Rapid SCN 做数值差减。

本轮修正：同表 recovery job 使用独立 mutex 串行化，并在加锁后复查加载状态；
源 MDL 获取后再次检查撤销，避免等待期间的源变更被误转为自动 primary reload。
原生恢复、普通表 reload 和分区表 reload 共用注册路径：先 attach 传播，再发布
share；失败撤销 consumer 登记、share 和部分 IMCS 对象，并要求显式加载。
重复成功任务跳过实际 restore/replay，不重复计数；重复失败任务保持未发布。

已加载表的 SECONDARY_LOAD 原本就由 check_loadable 拒绝。本轮保留该行为，
新增回归断言；仍加载的失败表必须先 SECONDARY_UNLOAD 再 SECONDARY_LOAD。
没有把最初遗漏该检查所产生的“重复手工 load 会追加数据”推测当作既有缺陷。

最终 Debug mysqld 构建通过；恢复协议单测 33/33 通过；最终代码的独立非 record
MTR 验证共 **17 个业务用例通过**：16 个恢复/加载用例的完整目标运行，加上
随后独立运行的 1 个分区恢复用例。shutdown_report 不计入业务数量。新增的
8 个用例覆盖混合表 GTID 恢复、发布失败清理、成功/失败任务重复执行、等待期间
撤销、重复加载报错、SIGABRT、断电文件模拟与分区重复恢复；既有用例覆盖
SIGKILL、未提交事务、日志回放、文件损坏、未日志化提交和手工加载一致性。

SIGABRT 用例向独立 MTR mysqld 发信号并关闭 core dump；验证 TP 已提交变更
保留、未提交变更排除，AP 与 TP 一致。断电模拟是在停止服务器后留下较新的
破损 manifest 和孤立临时文件，验证完整旧 generation 加 replay。它不等于
宿主机/虚拟机断电，未验证 OS/设备控制器缓存丢失或真实存储掉电行为。

测试开发中曾发现注入请求在 session 销毁后不生效、checkpoint-1 已被正常 GC
淘汰、模拟候选编号与已发布 generation 冲突，以及预期 SIGABRT 回溯日志未
豁免；修正后由最终二进制独立比对，不将这些失败运行或 record 运行计为放行结果。

日志：`/tmp/shannon-recovery-idempotence-debug-build.log`（最终 mysqld），
`/tmp/shannon-recovery-idempotence-gunit.log`，
`/tmp/shannon-recovery-idempotence-final-mtr.log`（16 个业务用例），
`/tmp/shannon-partition-recovery-final.log`（1 个分区业务用例）。
`git diff --check` 通过。

边界仍然存在：未日志化 TP 提交成功且全部撤销证据丢失，当前 FAST 无法自动
识别；TP 比对只有接口。本轮不宣称该窗口、真实断电资格、完整 suite、长时间
并发压力或通用生产 GA 已闭环。
