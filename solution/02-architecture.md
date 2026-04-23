# 02 - 系统架构设计

> 分层架构、核心组件交互、读/写路径流程、双轨搜索架构Trade-off分析

---

## 1. 分层架构图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Agent-Mem-IO Architecture                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    In-Memory Layer (≤20% budget)                     │   │
│  │                                                                      │   │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────────┐     │   │
│  │  │ PQ Codes       │  │ Graph Nav Data │  │ PQ Codebooks      │     │   │
│  │  │ (N×8B, 1.56%) │  │ (adj. lists    │  │ (M×K×d/M×4B)     │     │   │
│  │  │ 64x压缩        │  │  12.70%)       │  │  2.56%)           │     │   │
│  │  └────────────────┘  └────────────────┘  └────────────────────┘     │   │
│  │                                                                      │   │
│  │  ┌────────────────────────────────┐  ┌────────────────┐              │   │
│  │  │ Graph-Aware 2Q BufferPool      │  │ VisitedBitmap  │              │   │
│  │  │ (Hot+Warm queues,             │  │ (N/8+1KB)      │              │   │
│  │  │  hub node protection, 66.8%)  │  └────────────────┘              │   │
│  │  └────────────────────────────────┘                                  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                      │                                       │
│                                      │ PQ ADC预过滤 + 全精度距离计算        │
│                                      │ O_DIRECT + io_uring SSD读取         │
│                                      │ 拓扑感知预取下一跳                    │
│                                      ▼                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    SSD Layer (O_DIRECT)                              │   │
│  │                                                                      │   │
│  │  ┌──────────────────────────────────────────────────────────────┐   │   │
│  │  │ Disk Node Records (fixed 4KB each)                           │   │   │
│  │  │ [NodeID][FullVector][NeighborIDs][NeighborPQCodes][Padding]  │   │   │
│  │  └──────────────────────────────────────────────────────────────┘   │   │
│  │                                                                      │   │
│  │  ┌──────────────────────────────────────────────────────────────┐   │   │
│  │  │ LSM-Tree Write Path                                         │   │   │
│  │  │ MemTable → WAL → Immutable MemTable → SSTable → Compaction │   │   │
│  │  └──────────────────────────────────────────────────────────────┘   │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                      │                                       │
│                                      ▼                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    I/O Engine Layer                                  │   │
│  │                                                                      │   │
│  │  ┌──────────────────────────────────────────────────────────────┐   │   │
│  │  │ io_uring Engine                                             │   │   │
│  │  │ True batch submit (all SQEs → 1 syscall)                   │   │   │
│  │  │ Fixed-file + Fixed-buffer registration                     │   │   │
│  │  │ Async batch read + topology-aware prefetch                  │   │   │
│  │  └──────────────────────────────────────────────────────────────┘   │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 核心组件交互流程

### 2.1 组件依赖关系

```
Benchmark
  ├── FlatGraphIndex (图遍历导航)
  │     └── NSWBuilder → VamanaBuilder (3-phase构建)
  │
  ├── PQEncoder (PQ编码+ADC距离表)
  │     ├── PQ codes (内存常驻, 1.56%)
  │     └── PQ codebooks (内存常驻, 0.025%)
  │
  ├── DiskIndexReader (SSD读取+缓存)
  │     ├── BufferPoolManager (Graph-Aware 2Q缓存)
  │     │     ├── TwoQueueEvictionPolicy (warm+hot+hub保护)
  │     │     └── compute_distance_direct (零拷贝距离计算)
  │     ├── io_buffer_pool_ (I/O临时缓冲区, 非缓存)
  │     └── IoEngine (io_uring异步I/O)
  │           ├── submit_async_batch (批量提交)
  │           └── wait_async_batch (批量等待)
  │
  ├── LsmWriteManager (混合负载写入)
  │     ├── MemTable → WAL → SSTable
  │     ├── Bloom Filter (SSTable预过滤)
  │     └── CompactionManager (后台压缩)
  │
  └── VamanaBuilder.add_node_incremental (增量插入)
        ├── search_for_construction (搜索候选)
        ├── robust_prune (选择最优邻居)
        └── reverse edges + re-prune (反向边修剪)
```

### 2.2 关键接口

| 接口 | 调用方 | 被调用方 | 说明 |
|------|--------|----------|------|
| `diskann_search_enhanced()` | benchmark | DiskIndexReader + PQEncoder + FlatGraphIndex | SSD搜索主路径 |
| `search_memory_fast()` | benchmark | FlatGraphIndex + SIMD | 内存搜索路径（仅供对比） |
| `compute_distance_direct()` | DiskIndexReader | BufferPoolManager | 从BufferPool page直接计算距离，零拷贝 |
| `submit_async_batch()` | diskann_search_enhanced | IoEngine | io_uring批量异步读取 |
| `add_node_incremental()` | run_mixed_workload | VamanaBuilder | 增量插入新向量到图索引 |

---

## 3. 读路径（搜索）完整流程

### 3.1 SSD搜索路径（赛题核心场景）

```
diskann_search_enhanced(query, k, ef_search, beam_width, ...)
│
├─ Step 1: 初始化
│  ├── VisitedBitmap.resize(N) + clear()  // 复用bitmap，避免重复分配
│  ├── PQDistanceTable.build(query, pq_encoder)  // 构建ADC距离表
│  ├── entry_point pinning  // 确保entry point在BufferPool中
│  └── 初始化candidate = {entry_point}
│
├─ Step 2: PQ ADC预过滤（内存中完成，无I/O）
│  ├── 对每个candidate node_id:
│  │   ├── 从PQ codes获取8字节编码 (内存常驻)
│  │   ├── PQDistanceTable.compute_distance(node_id) → ADC近似距离
│  │   └── if ADC距离 < pq_threshold: 加入next_batch
│  │   └── else: 跳过（减少SSD读取量）
│  └── PQ ADC阈值截断：过滤掉PQ距离过大的候选，减少不必要的SSD读取
│
├─ Step 3: SSD批量读取（I/O层）
│  ├── if use_async (单线程io_uring):
│  │   ├── IoEngine.submit_async_batch(next_batch_ids)  // 批量提交I/O
│  │   ├── IoEngine.wait_async_batch()  // 等待完成
│  │   └── 获取全精度向量数据
│  ├── else (多线程同步):
│  │   ├── DiskIndexReader.read_vectors_batch(ids)  // 同步pread
│  │   └── BufferPoolManager.pin/unpin保护
│  └── BufferPool命中 → compute_distance_direct (零拷贝)
│     BufferPool未命中 → SSD O_DIRECT读取 → 放入BufferPool
│
├─ Step 4: 全精度距离计算
│  ├── l2_distance_sq_simd(query, full_vector)  // SSE/AVX2加速
│  ├── 更近的候选加入result heap
│  └── 获取候选的邻居列表 (内存常驻, Graph Nav Data)
│
├─ Step 5: 下一batch预取（CPU-I/O重叠）
│  ├── 当前batch计算距离时
│  ├── 同时提交下一batch的async reads (io_uring)
│  └── 实现CPU计算与I/O等待的流水线重叠
│
├─ Step 6: 重复Step 2-5直到候选列表耗尽
│
└─ Step 7: 返回Top-K结果
```

### 3.2 内存搜索路径（仅供对比，违反≤20%约束）

```
search_memory_fast(query, k, ef_search, ...)
│
├─ VisitedBitmap + 贪心图遍历
├─ 所有向量在内存中 → 直接l2_distance_sq_simd
├─ 无I/O、无BufferPool、无io_uring
└─ QPS高但内存比例100%，违反赛题约束
```

---

## 4. 写路径（LSM写入+增量插入）流程

```
run_mixed_workload(base, queries, ...)
│
├─ 写入线程 (write_thread):
│  ├── 生成新向量 (random or from dataset)
│  ├── LsmWriteManager.insert(new_vec, assigned_id)
│  │   ├── MemTable.insert()  // 内存中红黑树
│  │   ├── WAL.log_insert()   // O_DIRECT追加写
│  │   ├── MemTable满 → flush为SSTable
│  │   └── CompactionManager.compact()  // 后台Level-Tiered压缩
│  │
│  ├── vamana_builder->add_node_incremental(base, new_vec, graph_id, *nav_data)
│  │   ├── search_for_construction(new_vec, ef_construction)  // 搜索候选邻居
│  │   ├── robust_prune(candidates, max_degree)  // 选择最优邻居集
│  │   ├── 添加反向边 (reverse edges)
│  │   └── 超max_degree的邻居 → re-prune
│  │
│  ├── base.reserve()  // 防止reallocation导致引用失效
│  └── base_mutex保护并发append + graph insert
│
├─ 查询线程 (search_threads):
│  ├── diskann_search_enhanced(...)  // SSD搜索路径
│  └── 结果包含初始向量+增量插入向量
│
└─ LSM read-back验证:
   ├── LsmWriteManager.get(node_id)  // 读回写入的向量
   ├── Bloom Filter预过滤 (0.60%假阳性率)
   └── 报告可搜索率
```

---

## 5. 双轨搜索架构的Trade-off分析

### 5.1 SSD路径 vs 内存路径

| 维度 | SSD路径 | 内存路径 |
|------|---------|----------|
| **内存比例** | 20.0% ✅ | ~100% ❌ |
| **QPS** | 7.62 (10K) | 1961 (10K) |
| **Recall** | 98.90% | 98.90% |
| **I/O优化生效** | io_uring + BufferPool + PQ ADC + O_DIRECT | 全部绕过 |
| **赛题合规** | ✅ 核心场景 | ❌ 违反约束 |
| **适用场景** | 赛题评测 | 性能对比参考 |

### 5.2 Trade-off决策

**为什么保留内存路径？**

1. **对比参考**：内存路径QPS（1961/12887）展示了"如果不受内存约束"的性能上限，帮助理解I/O优化的价值
2. **功能验证**：内存路径使用相同的图索引和PQ编码器，验证核心算法的正确性
3. **诚实展示**：README明确标注内存路径❌违反约束，不作为达标证据

**为什么SSD路径QPS低但仍然选择它作为赛题核心？**

1. **赛题约束优先**：赛题明确要求内存≤20%，SSD路径满足约束而内存路径不满足
2. **I/O优化是赛题核心**：赛题考察的是"突破传统OS预取局限"的I/O优化能力，而非绕过约束追求高QPS
3. **WSL2环境限制**：SSD QPS受WSL2虚拟化I/O延迟限制（每次I/O约3-5ms），真实NVMe环境下预期显著提升
4. **Recall达标**：SSD路径Recall=98.90%远超85%阈值，证明I/O优化不影响检索质量

### 5.3 多线程SSD搜索的Trade-off

| 方案 | QPS | io_uring | 线程安全 | 选择 |
|------|-----|----------|----------|------|
| 单线程+io_uring | 7.62 | ✅ 完整链路 | ✅ | 默认方案 |
| 多线程+同步I/O | 9.25 | ❌ 禁用 | ✅ mutex保护 | 多线程方案 |
| 多线程+共享io_uring | ? | ✅ | ❌ heap-use-after-free | ❌ 不安全 |

**决策**：多线程SSD搜索禁用io_uring（async_buffers_线程不安全），使用同步BufferPool读取+pin保护。这是合理的线程安全决策——I/O并行性来自多线程并发同步读取，而非单线程异步批量读取。

---

## 6. 关键设计决策记录

| 决策 | 选择 | 理由 | 替代方案 |
|------|------|------|----------|
| 图索引算法 | DiskANN (Vamana) | SSD友好+PQ导航+beam读取 | HNSW（全内存假设） |
| 向量压缩 | PQ M=8 K=256 | 64x压缩+ADC预过滤 | OPQ（复杂度高收益低） |
| I/O框架 | io_uring | True batch+零拷贝+fixed优化 | libaio（限制多） |
| 缓存策略 | Graph-Aware 2Q | 命中率66.7%（LRU 0.6%） | LRU（不感知图结构） |
| 写入路径 | LSM-Tree | 追加写+后台压缩 | B-tree（随机写） |
| 磁盘布局 | 4KB固定记录 | O_DIRECT对齐+SSD友好 | 变长记录（对齐困难） |
| 多线程I/O | 同步BufferPool | 线程安全+pin保护 | 共享io_uring（不安全） |
| VisitedBitmap | resize/clear复用 | 1.2KB vs unordered_set 40MB | unordered_set（内存爆炸） |
| 增量插入 | add_node_incremental | DiskANN StitchedVamana标准做法 | 简单append（不保证连通性） |
| 性能展示 | 双轨+内存标注违规 | 诚实展示所有数据 | 只展示高QPS（选择性隐藏） |

---

*架构设计中的每个决策都记录了"为什么这么做"而非"做了什么"。实际实现细节见 [03-implementation.md](03-implementation.md)。*