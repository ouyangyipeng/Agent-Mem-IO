# Agent-Mem-IO 性能优化方案

> 基于 DiskANN、Starling、PipeSearch、Turbocharging Vector DBs 等文献研究制定的优化计划

---

## 一、当前性能瓶颈分析

### 1.1 内存问题（最关键）

当前内存比例为 **125%**，目标为 **10%-20%**。根本原因：

| 问题 | 原因 | 影响 |
|------|------|------|
| 全量向量常驻内存 | [`benchmark.cpp`](src/benchmark.cpp:280) 将所有向量加载到 `std::vector<Vector>` | 1M×128×4 = 512MB，远超限制 |
| 图邻接表全量加载 | [`benchmark.cpp`](src/benchmark.cpp:253) `std::vector<std::vector<NodeId>>` 全量常驻 | 每节点16邻居×2×4B = 128MB |
| 无PQ压缩 | 没有实现Product Quantization | 无法在有限内存中存储足够的导航信息 |

**SIFT1M 内存预算计算：**
- 数据集大小：1M × 128 × 4B = 512MB
- 10%限制 = 51.2MB，20%限制 = 102.4MB
- **DiskANN方案**：PQ压缩向量在内存（8MB）+ 图导航数据（~50MB）= **约58MB（~11%）**

### 1.2 QPS问题

当前 QPS = **331**，目标 > **1000**。原因：

| 瓶颈 | 原因 | 优化方向 |
|------|------|---------|
| 距离计算慢 | [`benchmark.cpp`](src/benchmark.cpp:39) 朴素L2循环计算，无SIMD | AVX2/SSE加速 |
| visited集合慢 | [`benchmark.cpp`](src/benchmark.cpp:112) `unordered_set<NodeId>` hash查找 | 改用bitmap |
| 无真正的异步I/O | benchmark纯内存运行，未走io_uring路径 | 实现CPU-I/O重叠 |
| 无批量处理 | 单query串行搜索 | 多query并行 + batch I/O |

---

## 二、优化方案（按优先级排序）

### 优先级1：Product Quantization（PQ）压缩 — 内存优化核心

**这是最关键的优化。没有PQ，无法满足10%-20%内存限制。**

DiskANN的核心策略：**内存中只存PQ压缩向量，SSD上存全精度向量+图索引**。

#### PQ原理

```
原始向量: 128维 × float32 = 512 bytes
PQ压缩:  分割为 m=8 个子空间，每子空间16维
          每子空间 k=256 个centroid，索引=1字节
          压缩后: 8 × 1字节 = 8 bytes (64x压缩!)

ADC(非对称距离计算):
  查询向量不压缩，对每个子空间预计算距离查找表
  与数据库向量距离 = 查表+累加，O(m)操作
  比全精度距离快得多，精度损失小
```

#### 内存预算（SIFT1M）

| 组件 | 大小 | 是否在内存 |
|------|------|-----------|
| PQ codes (1M×8B) | **8MB** | ✅ 内存 |
| PQ codebooks (8×256×16×4B) | **128KB** | ✅ 内存 |
| 图邻接表 (1M×max_degree×4B) | ~32-64MB | ✅ 内存 |
| 入口点索引 | ~1MB | ✅ 内存 |
| **内存总计** | **~41-73MB** | **8%-14%** ✅ |
| 全精度向量 (1M×512B) | 512MB | ❌ SSD |
| 图+向量合并记录 | ~576MB | ❌ SSD |

#### 实现步骤

1. **创建 `src/core/pq_encoder.h/cpp`**：
   - `PQEncoder` 类：训练codebook（k-means per subspace），编码向量
   - `PQDecoder` 类：ADC距离计算（查找表+累加）
   - 参数：`m=8, k=256`（SIFT1M标准配置）

2. **创建 `src/core/pq_distance.h`**：
   - `compute_adc_distance()`：非对称距离计算
   - `build_distance_table()`：为查询向量预计算查找表
   - 查找表大小：8×256 = 2048个float值 = 8KB，可常驻L1 cache

3. **修改搜索流程**：
   ```
   原流程: 全精度向量 → L2距离 → 排序
   新流程: PQ近似距离(ADC) → 过滤候选 → 
           io_uring异步读SSD全精度向量 → 重排序 → 最终结果
   ```

---

### 优先级2：Disk Layout优化 — 数据局部性

遵循 **Starling/MARGO** 论文的核心洞察：

> DiskANN中94%的4KB页面数据被浪费，因为每个I/O只访问一个节点（几百字节），但最小I/O单位是4KB。

#### DiskANN节点布局（4KB固定大小记录）

```
每个节点记录 = 固定4KB:
  [NodeID: 4B][Vector: 512B][NeighborIDs: R×4B][PQ_codes_of_neighbors: R×8B][Padding]
  
  R=32时: 4 + 512 + 128 + 256 = 900B → 填充到4KB
  R=64时: 4 + 512 + 256 + 512 = 1284B → 填充到4KB
  
关键优势: 一次磁盘读取获得:
  - 当前节点全精度向量
  - 邻居ID列表（用于后续遍历）
  - 邻居PQ codes（用于ADC距离计算，无需额外I/O！）
```

#### 实现步骤

1. **创建 `src/io/disk_layout.h/cpp`**：
   - `DiskNodeRecord` 结构：固定大小4KB记录
   - `write_node_record()`：将节点数据打包写入SSD
   - `read_node_record()`：从SSD读取并解析节点记录

2. **Beam Search策略**：
   - `beam_width=4`：每次预取4个候选节点的完整记录
   - 利用io_uring批量提交4个异步读请求
   - SSD并行处理多个读请求，延迟接近单次读

---

### 优先级3：io_uring Pipeline优化 — CPU-I/O重叠

遵循 **PipeSearch** 论文的核心设计：

> SSD延迟(~100μs) > 计算延迟(~10μs)，所以必须用I/O pipeline width(W>1)来重叠计算和I/O。

#### Coroutine风格异步搜索

```
搜索流程（PipeSearch风格）:

1. 初始化候选列表，从入口点开始
2. 循环:
   a. 从候选列表取top-W个未访问节点
   b. 通过io_uring批量预取这W个节点的SSD记录（非阻塞）
   c. 对已缓存的候选节点，用PQ ADC计算距离（计算与I/O重叠！）
   d. 等待io_uring完成，获取全精度向量
   e. 用全精度距离重排序，更新候选列表
3. 返回top-K结果
```

#### 实现步骤

1. **修改 `src/io/io_uring_engine.cpp`**：
   - 实现batch read：一次submit多个read请求
   - ring-per-thread设计
   - 注册固定缓冲区（`IORING_REGISTER_BUFFERS`）减少内核开销

2. **修改 `src/engine/prefetcher.cpp`**：
   - 实现 beam-style prefetch：预取top-W候选
   - 搜索中CPU计算PQ距离的同时，io_uring拉取SSD数据
   - 严格保证：CPU不阻塞等待I/O

---

### 优先级4：SIMD距离计算 — QPS提升

#### 当前问题

[`benchmark.cpp`](src/benchmark.cpp:39) 的 `l2_dist()` 是朴素循环：

```cpp
float s = 0;
for (Size i = 0; i < a.size(); ++i) {
    float d = a[i] - b[i];
    s += d * d;
}
```

#### AVX2优化

```cpp
// AVX2: 8个float并行处理，128维只需16次迭代
__m256 diff = _mm256_sub_ps(_mm256_loadu_ps(a+i), _mm256_loadu_ps(b+i));
__m256 sq = _mm256_mul_ps(diff, diff);
sum = _mm256_add_ps(sum, sq);
// 最终horizontal reduction
```

预期加速：**4-8x**（128维从128次迭代降到16次）

#### 实现步骤

1. **创建 `src/core/simd_distance.h/cpp`**：
   - `l2_distance_avx2()`：AVX2加速L2距离
   - `l2_distance_sse()`：SSE fallback
   - `l2_distance_fallback()`：朴素循环（非x86平台）
   - 编译时检测CPU特性自动选择

2. **PQ ADC距离也要优化**：
   - 查找表是连续内存，8×256 floats
   - 累加操作可用SIMD加速

---

### 优先级5：搜索算法优化

#### visited集合优化

```cpp
// 当前: unordered_set<NodeId> - hash查找O(1)但有内存分配开销
// 优化: 固定大小bitmap - 1bit per node, 纯数组访问

// 1M节点只需 1M/8 = 128KB bitmap
class VisitedBitmap {
    std::vector<uint8_t> bitmap_;  // 1M nodes = 128KB
    bool test(NodeId id) const { return bitmap_[id >> 3] & (1 << (id & 7)); }
    void set(NodeId id) { bitmap_[id >> 3] |= (1 << (id & 7)); }
};
```

#### 其他优化
- 预分配priority_queue内存（reserve）
- 避免search中不必要的内存分配
- 多query并行处理（线程池）

---

## 三、优化后的系统架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Agent-Mem-IO v2.0 Architecture                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    In-Memory Components (~10-15% budget)             │   │
│  │                                                                      │   │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────────┐     │   │
│  │  │ PQ Codes       │  │ Graph Nav Data │  │ PQ Codebooks      │     │   │
│  │  │ (1M×8B=8MB)    │  │ (adj. lists    │  │ (8×256×16×4B      │     │   │
│  │  │                │  │  ~32-64MB)     │  │  =128KB)          │     │   │
│  │  └────────────────┘  └────────────────┘  └────────────────────┘     │   │
│  │                                                                      │   │
│  │  ┌────────────────┐  ┌────────────────────────────────────────┐     │   │
│  │  │ Entry Points   │  │ Visited Bitmap (128KB per search)     │     │   │
│  │  │ (~1MB)         │  │ + Distance Lookup Tables (8KB/query)  │     │   │
│  │  └────────────────┘  └────────────────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                      │                                       │
│                                      │ PQ ADC距离过滤                        │
│                                      │ io_uring异步预取                      │
│                                      ▼                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    SSD-Resident Components (O_DIRECT + io_uring)     │   │
│  │                                                                      │   │
│  │  ┌──────────────────────────────────────────────────────────────┐   │   │
│  │  │ Disk Node Records (fixed 4KB each)                           │   │   │
│  │  │ [NodeID][FullVector][NeighborIDs][NeighborPQCodes][Padding]  │   │   │
│  │  │                                                              │   │   │
│  │  │ 一次I/O获得: 当前向量 + 邻居ID + 那居PQ码                    │   │   │
│  │  │ Beam Search: 预取W=4个候选节点记录                           │   │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    LSM-Tree Write Path (unchanged)                   │   │
│  │  MemTable → Immutable MemTable → SSTable → Compaction               │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 四、搜索流程对比

### 当前流程（全内存）

```
query → 加载全量向量到内存 → NSW greedy search → L2距离(朴素) → top-K
内存: 512MB(向量) + 128MB(图) = 640MB (125%)
QPS: 331 (单线程，无SIMD)
```

### 优化后流程（DiskANN风格）

```
query → 
  1. 构建ADC查找表(8KB) →
  2. 从内存PQ codes + 图邻接表开始搜索 →
  3. 用ADC距离遍历图(无需SSD I/O!) →
  4. 收集top-W候选 →
  5. io_uring batch预取W个候选的SSD记录(非阻塞) →
  6. CPU同时计算下一轮PQ ADC距离(重叠!) →
  7. 等I/O完成后，用全精度距离重排序 →
  8. 重复2-7直到收敛 → top-K结果

内存: 8MB(PQ) + 50MB(图) = 58MB (11%) ✅
QPS: 预估 >1000 (SIMD + CPU-IO重叠 + 批量处理)
```

**关键创新点**: 步骤3使用PQ ADC距离在内存中遍历图，**大部分图遍历不需要SSD I/O**！只有在需要全精度重排序时才访问SSD。这与DiskANN论文完全一致。

---

## 五、实施计划

### Step 1: PQ编码器实现

| 文件 | 内容 | 依赖 |
|------|------|------|
| `src/core/pq_encoder.h` | PQ编码器类定义 | types.h |
| `src/core/pq_encoder.cpp` | k-means训练 + 编码/解码 | pq_encoder.h |
| `src/core/pq_distance.h` | ADC距离计算 | pq_encoder.h |
| `src/core/pq_distance.cpp` | 查找表 + SIMD累加 | pq_distance.h |

### Step 2: Disk布局实现

| 文件 | 内容 | 依赖 |
|------|------|------|
| `src/io/disk_layout.h` | 磁盘节点记录结构 | types.h, pq_encoder.h |
| `src/io/disk_layout.cpp` | 写入/读取4KB记录 | disk_layout.h, io_uring_engine.h |

### Step 3: 重写benchmark（DiskANN风格）

| 文件 | 内容 | 依赖 |
|------|------|------|
| `src/benchmark.cpp` | 完整重写：PQ+SSD+io_uring+SIMD | 全部新模块 |

关键变化：
- 向量数据写入SSD文件（O_DIRECT）
- 内存中只保留PQ codes + 图邻接表
- 搜索使用ADC + beam prefetch
- 内存限制严格验证

### Step 4: SIMD距离计算

| 文件 | 内容 | 依赖 |
|------|------|------|
| `src/core/simd_distance.h` | SIMD距离函数声明 | types.h |
| `src/core/simd_distance.cpp` | AVX2/SSE实现 | simd_distance.h |

### Step 5: 搜索算法优化

- 修改graph search使用VisitedBitmap
- 实现beam-style prefetch
- 多query并行处理

### Step 6: 验证与调优

- Recall@10 ≥ 85% 验证
- 内存 ≤ 20% 验证（cgroups/setrlimit）
- QPS > 1000 验证
- P99延迟 < 10ms 验证
- 混合读写负载测试

---

## 六、关键参考文献

1. **DiskANN** (NeurIPS 2019): PQ压缩+SSD图索引+beam search的基础架构
2. **Starling** (arxiv 2024): 数据局部性优化，4KB页面利用率从6%提升到接近100%
3. **PipeSearch** (OSDI 2025): I/O pipeline width设计，CPU与I/O严格重叠
4. **Turbocharging Vector DBs** (VLDB 2025): io_uring在向量数据库中的应用，11.1x性能提升
5. **PQ原始论文** (Jegou et al., 2011): Product Quantization for Nearest Neighbor Search
6. **OPQ** (Ge et al., 2013): Optimized Product Quantization，优化子空间分解
7. **MARGO** (VLDB 2025): Monotonic Path Aware Graph Layout Optimization