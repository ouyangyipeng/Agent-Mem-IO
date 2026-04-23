# 03 - 关键实现细节

> 各模块实现要点、代码规模统计

---

## 1. Vamana图索引：3-phase构建

**源文件**: [`graph_index.h`](../src/core/graph_index.h), [`graph_index.cpp`](../src/core/graph_index.cpp), [`vamana_builder.cpp`](../src/core/vamana_builder.cpp)

Vamana图索引采用DiskANN论文的3-phase构建策略：

### Phase 1: NSW增量插入

- 从空图开始，逐个向量通过`search_for_construction()`找到最近的ef_construction个候选
- 用`robust_prune()`选择最优邻居集（max_degree=64）
- 添加反向边，超max_degree的邻居re-prune
- 产生一个连通但质量不高的初始图

### Phase 2: Medoid优化

- 计算所有向量到medoid（中心点）的距离，选择全局medoid作为entry point
- medoid的选择影响搜索的起始位置，好的medoid可以减少搜索跳数

### Phase 3: Prune + Compact

- 对每个节点的邻居列表执行`robust_prune()`，确保邻居集质量
- Compact操作：移除冗余边，保证图的最大度不超过max_degree
- 最终产生一个高质量的DiskANN式图索引

**关键实现细节**：

1. **robust_prune算法**：不是简单选最近的邻居，而是贪心选择"覆盖最多方向"的邻居——每次选一个最近候选，然后移除被该候选"覆盖"的其他候选（距离比例<α），确保邻居集在空间中均匀分布
2. **search_for_construction**：构建时使用ef_construction=200的宽搜索，确保图连通性
3. **增量插入**（[`add_node_incremental()`](../src/core/graph_index.cpp:211-287)）：77行实现，包含search→prune→reverse edges→re-prune完整流程

---

## 2. PQ编码器：k-means训练 + ADC距离表 + SIMD加速

**源文件**: [`pq_encoder.h`](../src/core/pq_encoder.h), [`pq_encoder.cpp`](../src/core/pq_encoder.cpp)

### k-means训练

- 将128维向量分为M=8个子空间，每个子空间16维
- 每个子空间独立训练K=256个聚类中心
- 训练有早停机制（centroid移动<阈值时停止）和采样限制（最多10000个样本）
- 固定种子`std::mt19937 rng(42)`确保可复现性

### ADC距离表

- `PQDistanceTable::build(query, pq_encoder)`：对查询向量，在每个子空间计算到256个聚类中心的距离
- 产生M×K的距离表（8×256=2048个float），用于快速预过滤
- `compute_distance(node_id)`：查表累加8个子空间的距离，得到ADC近似距离
- ADC距离是真实L2距离的下界（不会过滤掉真正近的向量）

### SIMD加速

- PQ距离表构建使用`l2_distance_sq_simd()`替代标量计算（[`pq_encoder.cpp`](../src/core/pq_encoder.cpp:164)）
- SSE级别SIMD（[`simd_distance.cpp`](../src/core/simd_distance.cpp:173-181)）
- Runtime检测CPU支持级别：`SIMD level: 1 (0=scalar, 1=SSE, 2=AVX2)`

**关键实现细节**：

1. **64x压缩**：128维×4字节=512字节 → 8字节PQ code（M=8, 每子空间1字节索引）
2. **ADC下界性质**：PQ ADC距离≤真实L2距离，因此PQ预过滤不会漏掉真正近的向量
3. **阈值截断**（V7新增）：PQ ADC距离>阈值的候选直接跳过，减少SSD读取量

---

## 3. DiskANN式搜索：PQ ADC预过滤 → beam prefetch → compute_distance_direct

**源文件**: [`benchmark.cpp`](../src/benchmark.cpp) `diskann_search_enhanced()`

### 搜索流程

1. **初始化**：VisitedBitmap + PQDistanceTable + entry point pinning
2. **PQ ADC预过滤**：对候选邻居，先计算PQ ADC距离，超过阈值直接跳过
3. **beam-style批量读取**：将通过PQ预过滤的候选收集为batch，一次性提交I/O
4. **全精度距离计算**：从SSD/BufferPool获取全精度向量，`l2_distance_sq_simd()`计算真实距离
5. **下一batch预取**：当前batch计算距离时，同时提交下一batch的async reads（CPU-I/O重叠）

**关键实现细节**：

1. **PQ ADC阈值截断**（[`benchmark.cpp`](../src/benchmark.cpp:836-846)）：`if (pq_dist > pq_threshold) continue;` — 减少不必要的SSD读取
2. **compute_distance_direct**（[`disk_layout.cpp`](../src/io/disk_layout.cpp)）：从BufferPool page直接计算距离，零拷贝（不需要将向量拷出page再计算）
3. **entry point pinning**（[`benchmark.cpp`](../src/benchmark.cpp:1639-1643)）：搜索前确保entry point在BufferPool中pin住，防止被淘汰导致搜索起点丢失
4. **async batch两阶段**：`submit_async_batch()` → `wait_async_batch()`，实现真正的CPU-I/O流水线重叠

---

## 4. BufferPoolManager：Graph-Aware 2Q + 动态hub阈值 + pin/unpin保护

**源文件**: [`buffer_pool.h`](../src/buffer/buffer_pool.h), [`buffer_pool.cpp`](../src/buffer/buffer_pool.cpp), [`eviction_policy.cpp`](../src/buffer/eviction_policy.cpp)

### 2Q缓存策略

- **Warm队列（FIFO）**：首次访问的页面进入warm队列，不会被立即淘汰
- **Hot队列（LRU）**：第二次访问的页面从warm升级到hot，受LRU淘汰策略管理
- **Graph-Aware保护**：淘汰hot队列时，跳过in-degree≥hub_threshold_的hub节点

### 动态hub阈值

- `compute_hub_threshold()`（[`eviction_policy.cpp`](../src/buffer/eviction_policy.cpp:37)）：根据in-degree的75th百分位动态计算
- `hub_percentile_ = 0.75`：Top 25% in-degree的节点被视为hub，不被淘汰
- 随着图结构变化，hub阈值自动调整

### pin/unpin保护

- `pin(node_id)`：标记页面为"正在使用"，不允许淘汰
- `unpin(node_id)`：释放pin标记，页面可被正常淘汰
- 多线程搜索时，每个线程pin住正在计算的页面，防止其他线程淘汰导致数据失效
- **关键修复**（V5）：pin保护使多线程SSD搜索Recall从4%→98.90%

**关键实现细节**：

1. **命中率对比**：Graph-Aware 2Q命中率66.7%（单线程），LRU仅0.6% — 110x提升
2. **4线程命中率0.3%**：多线程并发导致缓存竞争激烈，但pin保护确保关键节点不被淘汰
3. **compute_distance_direct**：BufferPool page中的向量数据可以直接用于距离计算，无需额外拷贝

---

## 5. io_uring引擎：fixed-file + fixed-buffer注册 + async batch submit

**源文件**: [`io_uring_engine.h`](../src/io/io_uring_engine.h), [`io_uring_engine.cpp`](../src/io/io_uring_engine.cpp)

### True batch submit

- 两阶段提交：先填充所有SQE（`io_uring_prep_read`），再一次性`io_uring_submit()`（1 syscall）
- V1的`submit_batch()`逐个submit（N个syscall），V2改为两阶段（1 syscall）

### fixed-file + fixed-buffer注册

- `register_file()`：将SSD文件描述符注册到io_uring，避免每次I/O的fd查找开销
- `register_buffer()`：将I/O缓冲区注册到io_uring，支持零拷贝I/O
- `set_io_engine()`中遍历`io_buffer_pool_`调用`register_buffer()`（[`disk_layout.cpp`](../src/io/disk_layout.cpp:758-764)）

### async_buffers_mutex_线程安全

- V6发现：多线程共享`async_buffers_`（unordered_map）导致heap-use-after-free
- V7修复：添加`std::mutex async_buffers_mutex_`保护（[`disk_layout.h`](../src/io/disk_layout.h:553)）
- 多线程SSD搜索仍传nullptr给io_engine（安全fallback），单线程使用完整io_uring链路

**关键实现细节**：

1. **队列深度128**：`io_uring_queue_init(128, &ring_, 0)` — 支持128个并发I/O请求
2. **O_DIRECT必须**：io_uring + O_DIRECT组合，绕过Page Cache
3. **关闭时正确注销**：`io_uring_unregister_buffers()` + `io_uring_unregister_files()`

---

## 6. LSM-Tree：WAL + MemTable + SSTable + Bloom Filter + Compaction

**源文件**: [`memtable.h`](../src/compaction/memtable.h), [`memtable.cpp`](../src/compaction/memtable.cpp), [`wal.cpp`](../src/compaction/wal.cpp), [`compaction_manager.cpp`](../src/compaction/compaction_manager.cpp)

### WAL（Write-Ahead Log）

- O_DIRECT追加写入，4KB对齐记录
- `log_insert()` / `log_delete()` 记录每个操作
- 崩溃恢复时从WAL重建MemTable

### MemTable

- 红黑树（`std::map`）存储key→vector映射
- `max_entries = 1000`，超过后flush为SSTable
- `max_size = 1MB`，双阈值触发flush

### SSTable + Bloom Filter

- `SSTableMetadata`包含`bloom_filter`字段（[`memtable.h`](../src/compaction/memtable.h:209)）
- `BLOOM_FILTER_BITS = 1024`（128 bytes per SSTable）
- `build_bloom_filter()`：构建Bloom Filter
- `bloom_filter_test()`：预过滤查询，假阳性率0.60%
- `get_sstables_containing()`：两步过滤（Range + Bloom Filter）

### Compaction

- **Size-Tiered**：相似大小的SSTable合并（默认策略）
- **Level-Tiered**（[`compact_level_tiered()`](../src/compaction/compaction_manager.cpp:270-353)）：按层级合并，减少读放大
- `CompactionConfig`包含`enum Strategy { SIZE_TIERED, LEVEL_TIERED }`
- 后台线程异步执行，不阻塞查询

**关键实现细节**：

1. **混合负载中Compaction启用**：`enable_background_compaction = true`（[`benchmark.cpp`](../src/benchmark.cpp:1097)）
2. **LSM read-back验证**：写入后尝试读回100个向量，报告可搜索率
3. **Bloom Filter假阳性率0.60%**：6 items, 1000 tests, 6 false positives

---

## 7. 增量插入：add_node_incremental

**源文件**: [`graph_index.cpp`](../src/core/graph_index.cpp:211-287)

### 完整流程

1. **search_for_construction(new_vec, ef_construction)**：从entry point搜索，找到ef_construction个最近候选
2. **robust_prune(candidates, max_degree)**：贪心选择最优邻居集，确保空间覆盖均匀
3. **添加反向边**：对每个选中的邻居，将新节点加入其邻居列表
4. **re-prune**：如果邻居的邻居数超过max_degree，对其执行robust_prune修剪

### 混合负载中的调用

```cpp
// benchmark.cpp L1126-1131
vamana_builder->add_node_incremental(base, new_vec, graph_id, *nav_data);
```

- `base_mutex`保护并发append + graph insert
- `base.reserve()`防止reallocation导致引用失效
- 单元测试`test_incremental_insert()`验证：neighbors + reverse edges + search discoverability

**关键实现细节**：

1. **77行实现**：不是简单的append，而是完整的DiskANN StitchedVamana算法
2. **反向边修剪**：新节点加入邻居列表后，如果邻居超度，必须re-prune保证图质量
3. **搜索可发现性验证**：增量插入的向量必须能被图遍历自然发现，而非仅通过LSM扫描

---

## 8. VisitedBitmap：resize/clear复用 + 多线程per-thread bitmap

**源文件**: [`visited_bitmap.h`](../src/core/visited_bitmap.h)

### 设计

- 替代`unordered_set<NodeId>`：1.2KB vs 40MB+（1M向量场景）
- `resize(N)`：分配N/8+1字节的bitmap
- `clear()`：重置所有位为0，复用已分配的内存
- `set(node_id)` / `test(node_id)`：O(1)位操作

### 多线程使用

- 每个搜索线程有自己的`thread_visited`（per-thread bitmap）
- 避免共享bitmap的锁竞争
- `resize/clear`在搜索开始时调用，复用已分配内存避免重复malloc

**关键实现细节**：

1. **内存节省**：1M向量场景下，bitmap=125KB，unordered_set需要40MB+ — 320x节省
2. **复用而非重复分配**：大数据集（1M）下，每次搜索如果重新分配bitmap会导致性能灾难
3. **O(1)操作**：set/test都是单次位操作，比unordered_set的hash+查找更快

---

## 9. 代码规模统计

| 文件 | 行数 | 说明 |
|------|------|------|
| [`benchmark.cpp`](../src/benchmark.cpp) | ~1904 | DiskANN风格benchmark + 混合负载 + 多线程搜索 |
| [`disk_layout.h`](../src/io/disk_layout.h) + [`disk_layout.cpp`](../src/io/disk_layout.cpp) | ~1400 | 4KB磁盘布局 + DiskIndexReader + BufferPool集成 |
| [`graph_index.h`](../src/core/graph_index.h) + [`graph_index.cpp`](../src/core/graph_index.cpp) | ~800 | Vamana图索引 + 增量插入 + robust_prune |
| [`pq_encoder.h`](../src/core/pq_encoder.h) + [`pq_encoder.cpp`](../src/core/pq_encoder.cpp) | ~500 | PQ编码器 + ADC距离表 |
| [`io_uring_engine.h`](../src/io/io_uring_engine.h) + [`io_uring_engine.cpp`](../src/io/io_uring_engine.cpp) | ~600 | io_uring引擎 + true batch + fixed注册 |
| [`buffer_pool.h`](../src/buffer/buffer_pool.h) + [`buffer_pool.cpp`](../src/buffer/buffer_pool.cpp) | ~400 | Graph-Aware 2Q缓存 + pin/unpin |
| [`memtable.h`](../src/compaction/memtable.h) + [`memtable.cpp`](../src/compaction/memtable.cpp) | ~700 | MemTable + WAL + SSTable + Bloom Filter |
| [`compaction_manager.cpp`](../src/compaction/compaction_manager.cpp) | ~350 | Size-Tiered + Level-Tiered Compaction |
| [`simd_distance.h`](../src/core/simd_distance.h) + [`simd_distance.cpp`](../src/core/simd_distance.cpp) | ~250 | AVX2/SSE距离计算 |
| [`storage_engine.h`](../src/engine/storage_engine.h) + [`storage_engine.cpp`](../src/engine/storage_engine.cpp) | ~300 | 统一存储引擎接口 |
| [`test_main.cpp`](../tests/test_main.cpp) | ~1089 | 20个单元测试 |
| **总计** | **~7000+** | C++20, 含完整I/O优化链路 |

---

*各模块的实现要点聚焦于"为什么这么做"而非"做了什么"。优化迭代过程见 [04-optimization.md](04-optimization.md)。*