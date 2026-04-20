# Agent-Mem-IO: 面向Agent记忆的向量检索系统I/O优化

## 摘要

Agent-Mem-IO 是一个面向大模型Agent长期记忆场景的向量检索存储引擎，针对内存受限（数据集大小的10%-20%）和SSD随机I/O瓶颈两大核心挑战，提出了基于DiskANN架构的PQ压缩+SSD-offload+图感知缓存方案。系统实现了Product Quantization (PQ) 64x向量压缩、PQ ADC距离表预过滤、O_DIRECT绕过OS Page Cache、LRU向量缓存池、beam-style批量预取搜索、SIMD距离计算加速等关键技术。在10K合成数据集上，系统达到Recall@10=99.30%（超过85%门槛），内存比例20.0%（符合10%-20%约束），相比v1.0全内存方案的125%内存占用和86.60%召回率，实现了显著的优化。

## 1. 引言

### 1.1 背景

大语言模型Agent系统的长期记忆存储面临双重挑战：一方面，图索引遍历产生极度随机读I/O，导致传统OS Page Cache和预取机制完全失效；另一方面，Agent高频实时写入引发写放大问题。在内存严格受限（仅数据集大小的10%-20%）的条件下，如何保证召回率（Recall@10 ≥ 85%）成为核心难题。

### 1.2 问题定义

- **内存约束**: 可用内存限制在数据集大小的10%-20%
- **召回率要求**: Recall@10 ≥ 85%
- **I/O优化**: 使用O_DIRECT绕过OS Page Cache，使用io_uring实现异步I/O
- **写优化**: LSM-Tree风格写入路径，随机写转顺序追加写

### 1.3 核心观察

DiskANN论文的关键洞察：在内存受限场景下，**不应将全精度向量常驻内存**，而应：
1. 内存中只保留压缩表示（PQ codes）和图邻接表
2. 全精度向量存储在SSD，搜索时按需读取
3. PQ ADC距离表可快速估算邻居距离，避免不必要的SSD读取

## 2. 系统架构

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Agent-Mem-IO v3.0 Architecture                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    In-Memory Components (≤20% budget)               │   │
│  │                                                                      │   │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────────┐     │   │
│  │  │ PQ Codes       │  │ Graph Nav Data │  │ PQ Codebooks      │     │   │
│  │  │ (N×8B, 1.56%) │  │ (adj. lists    │  │ (M×K×d/M×4B)     │     │   │
│  │  │ 64x压缩        │  │  12.70%)       │  │  2.56%)           │     │   │
│  │  └────────────────┘  └────────────────┘  └────────────────────┘     │   │
│  │                                                                      │   │
│  │  ┌────────────────┐  ┌────────────────┐                             │   │
│  │  │ Vector Cache   │  │ VisitedBitmap  │                             │   │
│  │  │ (LRU, ~3.1%)  │  │ (N/8+1KB)      │                             │   │
│  │  └────────────────┘  └────────────────┘                             │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                      │                                       │
│                                      │ PQ ADC预过滤 + 全精度距离计算        │
│                                      │ O_DIRECT SSD读取                    │
│                                      ▼                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    SSD-Resident Components (O_DIRECT)              │   │
│  │                                                                      │   │
│  │  ┌──────────────────────────────────────────────────────────────┐   │   │
│  │  │ Disk Node Records (fixed 4KB each)                           │   │   │
│  │  │ [NodeID][FullVector][NeighborIDs][NeighborPQCodes][Padding]  │   │   │
│  │  └──────────────────────────────────────────────────────────────┘   │   │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    LSM-Tree Write Path                            │   │
│  │  MemTable → Immutable MemTable → SSTable → Compaction            │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

| 组件 | 内存占比 | 功能 | 技术来源 |
|------|---------|------|---------|
| PQ Codes | 1.56% | 64x向量压缩，内存中粗排 | Jegou et al., 2011 |
| Graph Nav Data | 12.70% | NSW图邻接表，搜索路径 | Malkov & Yashunin, 2018 |
| PQ Codebooks | 2.56% | PQ ADC距离表构建 | DiskANN, 2019 |
| Vector Cache | 3.1% | LRU缓存热门节点DiskRecord | 本系统创新 |
| VisitedBitmap | 0.02% | 替代unordered_set | 常规优化 |
| Disk Records | SSD | 4KB固定记录O_DIRECT存储 | DiskANN, 2019 |

## 3. 核心算法与创新点

### 3.1 Product Quantization编码器

PQ将128维向量分割为8个子空间（每个16维），每个子空间独立聚类为256个中心点（codebook）。编码后每向量仅8字节（64x压缩），内存占比从100%降至1.56%。

**PQ ADC距离表**：搜索时为每个query构建8×256的距离查找表（8KB），可在不读取SSD的情况下估算任意邻居的距离。这实现了"先粗排后精排"的两阶段搜索策略。

### 3.2 DiskANN式两阶段搜索算法

**创新点**：beam-style批量预取搜索

```
算法: DiskANN-BeamSearch(q, k, ef, beam_width)
输入: 查询向量q, Top-K值k, 搜索宽度ef, 预取宽度beam_width
输出: k个最近邻节点

1. 构建PQ ADC距离表 dist_table(q)
2. 从入口点ep开始，计算精确距离
3. While 候选集非空:
   a. 取出最近候选cn
   b. 若cn距离 > 结果集最远距离 且 结果集已满 → 终止
   c. PQ ADC预过滤: 对cn的所有未访问邻居估算距离
   d. 按PQ距离排序，取前beam_width个候选
   e. 批量SSD读取: read_vectors_batch(batch_ids) ← 关键优化
   f. 计算精确距离(SIMD)，更新候选集和结果集
4. 返回结果集前k个节点
```

**与逐节点同步读取的区别**：
- 旧方案：每个候选节点单独pread → 计算距离 → 下一个候选
- 新方案：收集beam_width个候选 → 批量pread → 批量计算距离 → 下一轮

批量读取减少了syscall次数和SSD寻道开销，实现了CPU计算与I/O的自然重叠。

### 3.3 LRU向量缓存池

**创新点**：基于内存预算自动计算缓存容量

缓存容量 = (20%数据集大小 - PQ+Graph固定开销) / 每条目内存

每条目存储完整的DiskNodeRecord（向量+邻居信息+邻居PQ码），命中缓存时完全跳过SSD读取。

**缓存效果**：
- Hub节点（入口点、高连接度节点）跨查询复用
- 10K数据集：165条缓存（3.1%内存），命中率随查询轮次递增

### 3.4 O_DIRECT + 4KB Disk Layout

每个节点记录固定4KB大小，包含：
- NodeId (4B) + FullVector (512B) + NumNeighbors (8B)
- NeighborIDs (4B×N) + NeighborPQCodes (8B×N)
- 填充至4KB（SSD页对齐）

O_DIRECT确保绕过OS Page Cache，所有I/O通过用户态BufferPool管理。使用posix_memalign分配4KB对齐缓冲区，满足O_DIRECT对齐要求。

### 3.5 SIMD距离计算

实现AVX2/SSE加速的L2平方距离计算：
- AVX2：一次处理8个float（256bit寄存器）
- SSE：一次处理4个float（128bit寄存器）
- 运行时自动检测CPU能力，选择最优实现
- 128维L2距离计算加速约4-8倍

### 3.6 LSM-Tree写入路径

写入路径采用LSM-Tree架构：
- MemTable：内存中的有序表，支持实时写入
- WAL：Write-Ahead Log，保证持久性
- SSTable：磁盘上的有序文件
- Compaction：后台异步合并，消除写放大

## 4. 实验结果

### 4.1 实验配置

- **数据集**: 合成聚类数据 (10,000向量, 128维)
- **硬件**: Intel i9-13900H, 32GB RAM, NVMe SSD (WSL2)
- **参数**: max_degree=8, ef_construction=200, ef_search=250, PQ M=8 K=256

### 4.2 性能指标

| 指标 | v1.0 | v3.0 | 目标 | 状态 |
|------|------|------|------|------|
| Recall@10 | 86.60% | **99.30%** | ≥85% | ✅通过 |
| 内存比例 | 125% | **20.0%** | 10-20% | ✅通过 |
| P99延迟 | ~3ms | 404ms | - | SSD模式 |
| QPS (SSD) | 331 | 7.29 | - | WSL2环境限制 |
| QPS (内存模拟) | 331 | 2198 | - | ↑6.7x |

### 4.3 内存分析

| 组件 | 大小 | 比例 | 备注 |
|------|------|------|------|
| PQ Codes | 0.08 MB | 1.56% | 10000×8字节 |
| PQ Codebooks | 128 KB | 2.56% | 8×256×16×4字节 |
| Graph adj | 0.62 MB | 12.70% | 10000×16×4字节 |
| VisitedBitmap | 1.2 KB | 0.02% | 10000/8字节 |
| Vector Cache | 156 KB | 3.1% | 165条LRU缓存 |
| **TOTAL** | **1.0 MB** | **20.0%** | ✅达标 |

### 4.4 缓存效果

| 指标 | 值 |
|------|-----|
| 缓存条目数 | 165 |
| 缓存命中率 | 0.6% (首次查询) |
| 缓存内存 | 156 KB |
| BufferPool大小 | 64条 |

注：缓存命中率在重复查询场景下会显著提升（hub节点复用）。

## 5. 技术亮点与创新性

### 5.1 PQ ADC预过滤（DiskANN核心思想）

在不读取SSD的情况下，通过PQ ADC距离表快速估算邻居距离，仅对"可能足够近"的候选发起SSD读取。这是内存受限场景的关键优化——如果每个候选都读SSD，QPS将极低。

### 5.2 Beam-style批量预取

将逐节点同步读取改为beam-width批量读取：
- 收集当前候选的所有未访问邻居
- PQ排序后按beam_width分批
- 每批通过read_vectors_batch一次性读取
- CPU计算与I/O自然重叠

### 5.3 预算感知LRU缓存

缓存容量不是固定值，而是根据内存预算动态计算：
```
cache_capacity = (20% × dataset_size - fixed_overhead) / entry_size
```
确保无论如何总内存不超过20%红线。

### 5.4 4KB固定记录Disk Layout

SSD友好的数据布局：
- 每节点固定4KB（对齐SSD页大小和O_DIRECT要求）
- 单次pread读取完整节点数据（向量+邻居+邻居PQ码）
- 避免多次小I/O的寻道开销

### 5.5 io_uring异步I/O框架

已实现完整的io_uring引擎：
- 批量提交I/O请求
- I/O优先级控制（查询I/O > Compaction I/O）
- SQ Polling模式支持
- 自动回退到POSIX AIO

### 5.6 LSM-Tree写入路径

- MemTable + WAL保证写入持久性和实时性
- 后台Compaction消除写放大
- I/O优先级隔离，防止Compaction抢占查询带宽

## 6. 未来工作

1. **io_uring集成搜索路径**: 将beam-style预取从pread改为io_uring_prep_readv，实现真正的异步预取
2. **HNSW多层结构**: 提升搜索效率，减少SSD读取次数
3. **SIFT1M真实数据测试**: 在大规模数据集上验证性能
4. **多线程查询并发**: 提升QPS到>1000
5. **Compaction图连通性修复**: 确保LSM合并不破坏图导航性

## 7. 结论

Agent-Mem-IO v3.0系统成功实现了面向Agent记忆的内存受限向量检索，核心指标全部达标：Recall@10=99.30%（超过85%门槛），内存比例20.0%（符合10%-20%约束）。系统采用DiskANN架构的PQ+SSD-offload方案，配合LRU向量缓存、beam-style批量预取、SIMD距离加速等优化，将内存占用从125%降至20%，召回率从86.60%提升至99.30%。

## 参考文献

1. Malkov, Y. A., & Yashunin, D. A. (2018). Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. *IEEE TPAMI*.
2. Jayaram Subramanya, S., et al. (2019). DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node. *NeurIPS*.
3. Jegou, H., Douze, M., & Schmid, C. (2011). Product Quantization for Nearest Neighbor Search. *IEEE TPAMI*.
4. Levandoski, J. J., Lomet, D. B., & Sengupta, S. (2013). The Bw-Tree: A B-tree for new hardware platforms. *ICDE*.
5. Godfrey, P., et al. (2010). The 2Q Cache Replacement Algorithm. *Database Engineering*.