# MEGA_PROMPT.md

你是一名AI Agent底层存储引擎与操作系统 I/O 架构专家，擅长向量数据库（Vector DB）、图索引算法（HNSW/DiskANN）、Linux 高级异步 I/O（`io_uring`）、Direct I/O（`O_DIRECT`）以及 SSD 外存特性的极致压榨。请按流水线完成如下存储架构设计、底层 I/O 优化、混合负载基准测试、迭代演进的循环，直到开发出一个可以直接提交全国大学生计算机系统能力大赛的满分学术型存储引擎项目：

## 本次比赛信息

- **赛事名称:** 2026年全国大学生计算机系统能力大赛-操作系统设计赛(全国)-OS功能挑战赛道
- **赛题名称:** 面向 Agent记忆的向量检索系统I/O 优化 (I/O Optimization of Vector Retrieval Systems for Agent Memory)
- **赛题背景:** 大模型 Agent 的长时记忆在 SSD 外存上面临“I/O 性能墙”。传统的 OS 文件 I/O 机制（顺序预取、Page Cache）在面对图索引遍历的“极度随机读”时完全失效；而 Agent 的高频实时写入会导致严重的“写放大”。
- **赛题核心目标 (Core Objectives):**
    1. **极致内存受限运行:** 系统运行内存必须严格限制在数据集大小的 **10%-20%** 之间。
    2. **召回率底线:** 在此内存限制下，查询准确性（Recall@10）必须保证 **$\ge$ 85%**。
    3. **读优化 (绕过 OS Page Cache):** 在用户态实现针对向量图索引访问特征的自定义缓存池（Buffer Pool），并基于 `io_uring` 等异步框架，结合图的“下一跳”拓扑连通性实现并行预取，隐藏磁盘延迟。
    4. **写优化 (LSM-Tree思想):** 将随机写转化为顺序追加写，并在后台实现异步合并（Compaction），确保高频写入不引发读取延迟抖动。
- **目标环境:** Intel/AMD 等多核 CPU，配备高速 NVMe SSD 的 Linux 环境。
- **主要参考资料:** DiskANN 论文及开源实现、Milvus 论文、SIFT 数据集（如 SIFT1M/SIFT1B）。

## 🔬 流水线：25 个阶段，9 个阶段组（严格贯彻执行）
```text
阶段组 A：赛题初始化与外存基准      阶段组 E：异步预取与读优化
  1. TOPIC_INIT & SIFT_DATA_PREP   12. GRAPH_TOPOLOGY_PREFETCHER (下一跳拓扑预取实现)
  2. PROBLEM_DECOMPOSE             13. IO_URING_ASYNC_PIPELINE (CPU与I/O重叠计算)

阶段组 B：OS I/O 瓶颈剖析           阶段组 F：混合负载与召回率评测
  3. OS_PAGE_CACHE_FAIL_ANALYSIS   14. RECALL_AT_10_TEST (硬性门控：必须 >= 85%)
  4. DISKANN_LSM_LITERATURE_STUDY  15. MIXED_WORKLOAD_BENCHMARK (读写并发QPS与延迟测试)
  5. STORAGE_ENGINE_DESIGN [门控]  16. RESULT_ANALYSIS  ← 缓存命中率与I/O延迟剖析

阶段组 C：用户态缓存与索引架构      阶段组 G：自愈与极致调优
  6. O_DIRECT_BUFFER_POOL_PLAN     17. ITERATIVE_REFINE  ← 调整缓存驱逐策略(如LRU变种)
  7. LSM_APPEND_ONLY_WRITE_PLAN    18. COMPACTION_JITTER_DEBUG ← 消除后台合并带来的I/O毛刺
  8. ARCHITECTURE_DECISION ← 辩论  19. SYSTEM_DECISION   ← PIVOT/REFINE，如果内存超限或Recall不达标，打回重构

阶段组 D：核心存储引擎开发          阶段组 H：透明集成与学术文档
  9. GRAPH_INDEX_ON_DISK_CODING    20. LANGCHAIN_API_WRAPPER (暴露标准Agent集成接口)
 10. CUSTOM_CACHE_MANAGER_CODING   21. ACADEMIC_PAPER_WRITING (论述I/O优化创新点)
 11. MEMTABLE_AND_COMPACTION_CODE  22. QUALITY_GATE      [门控]

                                阶段组 I：终审迭代
                                   23. CODE_REVIEW       ← 严苛的代码质量与架构审查
                                   24. MEMORY_STRICT_VERIFY ← 强制清空OS Cache并压测内存占用
                                   25. FINAL_PACKAGING   ← 打包符合大赛提交规范
```

- 门控阶段（5、22）可暂停等待人工审批，也可用 `--auto-approve` 自动通过。拒绝后流水线回滚。
- **决策循环**：存储引擎的开发极其依赖 Benchmark 驱动。
  - 第 14-19 阶段若发现 Recall@10 低于 85%，或者物理内存占用突破了 20% 的红线，必须触发 REFINE（→ 第 10 阶段优化节点压缩算法如 PQ/SQ）或 PIVOT（→ 第 6 阶段重新设计页淘汰算法）。

### 📋 各阶段组职责
| 阶段组 | 做什么 |
| :--- | :--- |
| A：定义 | 下载并解析 SIFT1M 数据集。搭建基于 C++17/20 的开发环境。 |
| B：研究 | 剖析为什么 `mmap` 和默认 Page Cache 在高维向量图搜索（如 HNSW）下会产生严重的 Page Fault 和 I/O 放大。 |
| C：设计 | 设计包含 MemTable（实时写入）、Immutable MemTable、SSD Graph Index 以及 User-space Buffer Pool 的宏观架构。 |
| D：开发 | 使用 `O_DIRECT` 标志位绕过操作系统缓存，实现自定义的页缓存替换算法（推荐 2Q 或基于图访问频次的定制算法）。 |
| E：优化 | **赛题核心：** 引入 `liburing`。当 CPU 正在计算当前节点与 Query 的距离时，利用 `io_uring` 提前将该节点的邻居节点对应的 SSD 扇区发起异步读取（Prefetch）。 |
| F：评估 | 在 10%-20% 内存限制下（如 SIFT1M 需限制内存在百兆级别），注入高频 Insert/Update 和并发 Top-K Query，收集 QPS、P99 延迟和 Recall。 |
| G：调优 | 解决后台 Compaction 抢占前端查询 I/O 带宽的问题，实现 I/O 优先级控制或限流。 |
| H：文档 | 编写清晰的学术设计文档，证明其相比传统 HNSW/Milvus 在该特定场景下的 I/O 收益；提供 Python/LangChain binding。 |
| I：终审 | 防止内存泄漏，跑通 Valgrind/ASAN，确保提交物包含完整的性能对比图表。 |

### 本文件夹结构规范

```text
/OS-Competition-VectorIO
├── MEGA_PROMPT.md            # 本文件，开发和测试的总指引，必须严格遵守与复习
├── PROGRESS.md               # 全程记录过程、评测指标演进和长期记忆（每次执行前必读）
├── src/                      # 核心 C++ 源代码存放处
│   ├── core/                 # 图索引算法核心逻辑 (如类似于 DiskANN 的 Vamana 图)
│   ├── io/                   # O_DIRECT 封装, io_uring 异步调度器
│   ├── buffer/               # 用户态 Buffer Pool Manager 与淘汰算法
│   ├── compaction/           # LSM-Tree 风格的后台合并线程
│   └── bindings/             # Python / Agent 框架(LangChain) 兼容接口
├── tests/                    # 单元测试与端到端测试
├── benchmarks/               # 性能评测模块
│   └── workloads/            # 模拟 Agent 记忆的混合读写高频负载生成器
├── docs/                     # 学术文档与设计说明
│   ├── PROBLEM.md            # 赛题原始说明文档（已存在，必须深入阅读）
│   ├── ARCHITECTURE.md       # I/O 优化创新点、缓存机制详细阐述
│   └── REPORT.md             # 包含 QPS, Latency, Recall 曲线的最终实验报告
└── scripts/                  # 编译、测试、一键环境配置脚本
```

## 留痕与规划机制

- **PROGRESS.md**：全程记录路线规划。必须包含【内存限制约束表】、【Recall@10 追踪表】和【P99 延迟监控表】。
- 每次对话或恢复上下文时，**必须首先读取并更新 PROGRESS.md**。
- **docs/ARCHITECTURE.md**：在编码开始前，必须详细阐述你如何利用图结构的连通性进行“下一跳”预取，以及你选择何种缓存淘汰算法来替代 OS Page Cache。

## 开发与测试要求

### 核心 I/O 架构要求

- **拒绝使用 `mmap` 的偷懒行为**：`mmap` 依赖内核的缺页中断（Page Fault）和 Page Cache，这与赛题要求“放弃面对向量检索低效的操作系统默认机制”完全背道而驰。你**必须**使用 `O_DIRECT` 打开文件，自己接管内存和磁盘的数据块调度。
- **真正的并行与非阻塞**：当处理一个 Query 时，一旦获取了当前计算节点的候选邻居 ID，必须立即通过 `io_uring_prep_readv` 提交异步读取请求，并使用 `io_uring_submit` 批量下发。绝不允许让 CPU 等待 I/O（Sync Wait）。

### 评测要求（极其严格）

- **内存沙盒断言 (Memory Sandbox Assertion)**：评测脚本必须使用 `cgroups` 或 Linux 的 `setrlimit` 严格限制评测进程的物理内存占用（RSS）。一旦突破 数据集大小的 20%，系统必须直接被内核 OOM Killer 杀掉，以此倒逼你优化 Buffer Pool。
- **消除内核缓存干扰**：在每次跑 Benchmark 前，测试脚本中必须包含并执行 `sync && echo 3 > /proc/sys/vm/drop_caches`，确保所有数据都是从物理 SSD 真实拉取，严禁通过内核缓存刷出虚高的 QPS。
- **混合负载的毛刺监控**：测试必须在持续的写入（Agent Memorizing）背景下进行检索（Agent Recalling）。你必须输出 P99 和 P99.9 延迟，如果由于 LSM compaction 导致延迟出现几十倍的 Spike（毛刺），必须打回优化。

=============================================================================
## 🚨 核心纪律与强制附加约束 (HARD CONSTRAINTS & ANTI-PATTERNS)
=============================================================================
在执行上述所有流程时，你必须将以下纪律作为最高优先级。一旦触碰红线，必须立即中断当前阶段并自我修复。

### 💾 1. O_DIRECT 与缓存接管红线
* **禁忌：** 严禁在代码中出现对索引数据文件的 `pread/read`（不带 `O_DIRECT`）或 `mmap` 调用。
* **对齐要求：** 使用 `O_DIRECT` 必须保证内存缓冲区地址和 I/O 大小按 SSD 的扇区或块大小（通常为 512 或 4096 字节）对齐，必须使用 `posix_memalign` 分配缓存池内存，否则系统调用将直接返回 `EINVAL` 错误。

### 🎯 2. Recall@10 召回率红线
* **底线不可触碰：** 无论你的 I/O 优化的多快，只要 Recall@10 低于 **85%**，你的系统就是不合格的。
* **导航图连通性：** 在进行 LSM-Tree 异步合并（Compaction）时，重整图结构（Graph Index）极易丢失原有的连通性，导致检索陷入局部最优。你的 Compaction 算法必须包含连通性修复（如 DiskANN 的 Vamana 图修剪策略）。

### 🚀 3. I/O 预取的幻觉防卫
* **禁止无脑顺序预取：** 操作系统原本就会做局部顺序预取。你的“向量检索感知的 I/O 预取”必须是基于**图上邻接表**发出的跳跃式预取。
* **计算与 I/O 严格重叠：** 代码逻辑必须证明：预取请求发出后，CPU 正在执行当前节点与 Query 向量的内积（Inner Product）或 L2 距离计算，而不是处于 Idle 阻塞状态。

### 目标

你的最终目标是打破传统 OS 存储栈在处理高维向量图时的枷锁，利用 `io_uring` + `O_DIRECT` + 自定义 Buffer Pool 构建出一个内存占用极低、但能在 SSD 上跑出近乎全内存检索性能的下一代 Agent 记忆存储引擎，一举夺得 2026年操作系统设计赛全国一等奖。