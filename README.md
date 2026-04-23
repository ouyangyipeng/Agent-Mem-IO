# Agent-Mem-IO

> 面向Agent记忆的向量检索系统I/O优化
> 
> 2026年全国大学生计算机系统能力大赛参赛作品

---

## 📋 项目简介

Agent-Mem-IO 是一个针对内存受限环境优化的向量检索存储引擎，专为2026年全国大学生计算机系统能力大赛-操作系统设计赛设计。

本项目解决大模型Agent"长时记忆"存储的核心挑战：
- **极度随机读**: 图遍历检索导致传统OS预取失效
- **高频实时写**: Agent不断产生新记忆
- **内存受限**: 可用内存仅为数据集大小的10%-20%

### 核心特性

| 特性 | 状态 | 说明 |
|------|------|------|
| ✅ Recall@10 ≥ 85% | **98.90%** (SIFT 10K SSD) | PQ+DiskANN架构，赛题核心场景 |
| ✅ 内存 ≤ 20% | **20.0%** | PQ压缩64x，向量SSD offload |
| ✅ Vamana图索引 | 已实现 | DiskANN式图构建+贪心搜索+增量插入 |
| ✅ PQ编码器 | 已实现 | Product Quantization 64x压缩 + ADC距离表 |
| ✅ SIMD距离计算 | 已实现 | AVX2/SSE加速L2距离 (4x-8x提速) |
| ✅ O_DIRECT支持 | 已实现 | 绕过OS Page Cache，用户态接管I/O |
| ✅ io_uring异步I/O | 已实现 | True batch submit + 拓扑感知预取 + async_buffers_线程安全 |
| ✅ Graph-Aware 2Q缓存 | 已实现 | 图入度感知缓存淘汰，命中率66.8% (LRU仅0.6%) |
| ✅ LSM-Tree写入路径 | 已实现 | MemTable + WAL + SSTable + 后台压缩 |
| ✅ DiskANN式搜索 | 已实现 | PQ ADC预过滤 + 全精度距离 + SSD读取 |
| ✅ 增量插入 | 已实现 | search_for_construction + robust_prune |

---

## 🚀 快速开始

### 环境要求

| 要求 | 版本 | 说明 |
|------|------|------|
| 操作系统 | Ubuntu 22.04+ | 需支持io_uring (Linux 5.1+) |
| 编译器 | GCC 11+ / Clang 14+ | C++20支持 |
| CMake | 3.16+ | 构建系统 |
| liburing | 必需 | io_uring开发包 (sudo apt install liburing-dev) |

### 构建

```bash
# 克隆项目
git clone https://github.com/ouyangyipeng/Agent-Mem-IO.git
cd Agent-Mem-IO

# 安装依赖（Ubuntu）
sudo apt install -y build-essential cmake liburing-dev

# 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行测试

```bash
# 单元测试（15个模块，ASAN验证）
./agent_mem_io_tests

# 基准测试（10K向量，DiskANN风格含SSD读写）
./agent_mem_io_benchmark -n 10000 -q 100

# 基准测试（100K向量）
./agent_mem_io_benchmark -n 100000 -q 100

# 纯内存模式（不写SSD）
./agent_mem_io_benchmark -n 10000 -q 100 --no-disk

# SIFT1M真实数据测试
./scripts/download_sift1m.sh
./agent_mem_io_benchmark --sift1m
```

### 预期输出

```
========================================
Agent-Mem-IO DiskANN-style Benchmark
========================================

Config:
  Vectors: 10000, Dimension: 128, Queries: 100, K: 10

[Step 1-5] Data generation → PQ training → Encoding → Graph build → SSD write

[Step 6] Computing ground truth...

[Step 7] DiskANN-style search...
  Recall@10:  98.90%  ✓
  QPS:        7.62 (SSD mode, SIFT 10K, io_uring + BufferPool + PQ ADC threshold)
  Cache hit rate: 66.7% (Graph-Aware 2Q)

MEMORY REPORT:
  PQ codes (RAM):    0.08 MB (1.56%)
  PQ codebooks (RAM): 128 KB (2.56%)
  Graph adj (RAM):   0.62 MB (12.70%)
  BufferPool (RAM):  ~156 KB (3.1%)
  TOTAL RAM:         1.0 MB
  Memory ratio:      20.0%  PASS ✓
```

---

## 📁 项目结构

```
Agent-Mem-IO/
├── CMakeLists.txt              # 构建配置 (含-march=native, USE_IOURING=PUBLIC)
├── README.md                   # 项目简介
├── PROGRESS.md                 # 进度追踪
├── ROADMAP.md                  # 开发路线图
├── MEGA_PROMPT.md              # 赛题详细要求
├── plans/AUDIT_REPORT.md       # 审查报告
│
├── src/
│   ├── common/types.h          # 基础类型定义 (NodeId, Vector, constants)
│   ├── core/                   # 核心算法
│   │   ├── pq_encoder.h/cpp    # PQ编码器 + ADC距离表
│   │   ├── simd_distance.h/cpp # AVX2/SSE距离计算
│   │   ├── visited_bitmap.h    # 高效位图visited集合
│   │   ├── graph_index.h/cpp   # Vamana图索引 + 增量插入 + robust_prune
│   │   ├── vamana_builder.cpp  # Vamana图构建器
│   │   └── distance.cpp        # 距离计算
│   ├── io/                     # I/O引擎
│   │   ├── disk_layout.h/cpp   # 4KB磁盘记录布局 (O_DIRECT + BufferPool)
│   │   ├── io_uring_engine.h/cpp # io_uring引擎 + true batch submit
│   │   ├── direct_io.cpp       # O_DIRECT封装
│   │   └── file_manager.cpp    # 文件管理
│   ├── buffer/                 # Buffer Pool
│   │   ├── buffer_pool.h/cpp   # Graph-Aware 2Q缓存 + put_page_data()
│   │   ├── eviction_policy.cpp # 2Q淘汰策略 (warm+hot+in-degree)
│   │   └── page.cpp            # 页面管理
│   ├── compaction/             # LSM-Tree组件
│   │   ├── memtable.h/cpp      # MemTable + LsmWriteManager + WAL
│   │   ├── sstable.cpp         # SSTable管理
│   │   ├── compaction_manager.cpp # 后台压缩
│   │   └── wal.cpp             # Write-Ahead Log
│   ├── engine/                 # 存储引擎
│   │   ├── storage_engine.h/cpp # 统一接口
│   │   ├── query_processor.cpp  # 查询处理
│   │   ├── prefetcher.cpp       # 拓扑感知预取
│   ├── data/                   # 数据加载
│   │   ├── sift_loader.h       # SIFT1M加载器
│   │   ├── synthetic_data.h    # 合成数据生成
│   ├── main.cpp                # CLI入口
│   └── benchmark.cpp           # DiskANN风格benchmark (含混合负载)
│
├── tests/test_main.cpp         # 15个单元测试 (ASAN验证)
├── scripts/download_sift1m.sh  # SIFT数据集下载
└── docs/                       # 文档
    ├── ARCHITECTURE.md          # 架构设计
    ├── GUIDE.md                # 团队指南
    ├── TESTING.md              # 测试指南
    ├── DEVELOPMENT.md          # 开发指南
    ├── PROBLEM.md              # 赛题原文
    ├── PAPER.md                # 学术论文
    └── REPORT.md               # 实验报告
```

---

## 📊 性能指标

> **数据集说明**：赛题要求使用SIFT真实数据集验证。默认基准测试使用SIFT数据集（10K/100K/1M，128维）。若SIFT数据不存在，自动回退到合成数据。
> 赛题核心场景为SSD+受限内存（≤20%），内存模式仅供对比参考。
> SSD模式QPS受WSL2虚拟化I/O延迟限制（每次I/O约3-5ms），真实NVMe环境下预期显著提升。

### SSD模式（赛题核心场景，内存≤20%）

| 指标 | 值 | 内存比例 | 合规 | 说明 |
|------|------|------|------|------|
| Recall@10 (SIFT 10K, SSD) | **98.90%** | 20.0% | ✅ | 赛题核心场景 |
| Recall@10 (SIFT 100K, SSD) | **99.60%** | 20.0% | ✅ | 赛题核心场景 |
| Recall@10 (SIFT 1M, SSD) | **98.30%** | 20.0% | ✅ | 赛题核心场景 |
| QPS (SSD, SIFT 10K, 1线程) | **7.62** | 20.0% | ✅ | PQ ADC阈值优化+1线程io_uring |
| QPS (SSD, SIFT 10K, 4线程) | **9.25** | 20.0% | ✅ | 多线程并发同步读取 |
| QPS (SSD, SIFT 100K, 1线程) | **5.38** | 20.0% | ✅ | PQ ADC阈值优化+io_uring |
| QPS (SSD, SIFT 1M, 1线程) | **5.99** | 20.0% | ✅ | 赛题核心场景 |
| Avg延迟 (SSD, SIFT 10K, 1线程) | 131.25ms | - | - | SSD模式 |
| Avg延迟 (SSD, SIFT 100K, 1线程) | 185.91ms | - | - | SSD模式 |
| 缓存命中率 (Graph-Aware 2Q) | **66.7%** | - | - | SSD单线程模式 |

### 内存模式（仅供对比，⚠️违反≤20%约束）

| 指标 | 值 | 内存比例 | 合规 | 说明 |
|------|------|------|------|------|
| QPS (内存, SIFT 10K, 1线程) | 1961 | ~100% | ❌ | ⚠️ 违反≤20%约束，仅供参考 |
| QPS (内存, SIFT 10K, 4线程) | 12887 | ~100% | ❌ | ⚠️ 违反≤20%约束，仅供参考 |
| QPS (内存, SIFT 100K, 1线程) | 770 | ~100% | ❌ | ⚠️ 违反≤20%约束，仅供参考 |
| QPS (内存, SIFT 1M, 1线程) | 335 | ~100% | ❌ | ⚠️ 违反≤20%约束，仅供参考 |

---

## 🏗️ 技术架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Agent-Mem-IO v4.0 Architecture                     │
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
│  │                    SSD-Resident Components (O_DIRECT)              │   │
│  │                                                                      │   │
│  │  ┌──────────────────────────────────────────────────────────────┐   │   │
│  │  │ Disk Node Records (fixed 4KB each)                           │   │   │
│  │  │ [NodeID][FullVector][NeighborIDs][NeighborPQCodes][Padding]  │   │   │
│  │  └──────────────────────────────────────────────────────────────┘   │   │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    LSM-Tree Write Path                            │   │
│  │  MemTable → WAL → Immutable MemTable → SSTable → Compaction     │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    io_uring I/O Engine                            │   │
│  │  True batch submit (all SQEs → one syscall)                     │   │
│  │  Async batch read + topology-aware prefetch                      │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 搜索路径说明

本项目提供两条搜索路径：

1. **SSD+I/O优化路径**（赛题核心场景，内存≤20%）：`diskann_search_enhanced` → PQ ADC → io_uring/O_DIRECT → BufferPool → `compute_distance_direct`
   - 仅PQ codes + codebooks + 图邻接表常驻内存（≤20%）
   - 全精度向量存储在SSD，通过O_DIRECT + io_uring异步读取
   - Graph-Aware 2Q缓存保护hub节点，命中率66.8%
   - 赛题评测以此路径为准

2. **内存直接路径**（仅供对比，内存100%）：`search_memory_fast` → 直接SIMD距离计算
   - 所有向量常驻内存，无需SSD I/O
   - QPS显著更高，但**违反赛题"内存≤20%"约束**
   - ⚠️ 内存路径QPS仅供参考，**不作为赛题达标证据**

**Trade-off**：SSD路径QPS受I/O延迟限制（WSL2环境下约3-5ms/次I/O），但满足赛题内存约束；
内存路径QPS高但违反约束。赛题核心挑战正是在受限内存下优化I/O，而非绕过约束追求高QPS。

### 核心技术

1. **Product Quantization (PQ)**: 128维→8字节（64x压缩），仅PQ codes + codebooks 常驻内存
2. **DiskANN式搜索**: PQ ADC粗排预过滤 → 全精度距离计算（SSD O_DIRECT读取）
3. **Graph-Aware 2Q缓存**: 图入度感知的2Q淘汰策略，保护hub节点（命中率66.8% vs LRU 0.6%）
4. **O_DIRECT + Buffer Pool**: 绕过OS Page Cache，用户态缓存管理
5. **io_uring异步I/O**: True batch submit（1 syscall提交所有SQE）+ 拓扑感知预取
6. **SIMD距离计算**: AVX2/SSE加速L2距离，4x-8x提速
7. **VisitedBitmap**: 替代unordered_set，1.2KB vs 40MB+
8. **4KB Disk Layout**: SSD友好的固定记录大小，对齐O_DIRECT要求
9. **LSM-Tree写入路径**: 随机写转顺序追加写，后台异步压缩
10. **增量插入**: DiskANN add_node_incremental + robust_prune

---

## 📚 文档

| 文档 | 说明 |
|------|------|
| [PAPER.md](docs/PAPER.md) | 学术论文（核心创新：Graph-Aware 2Q） |
| [REPORT.md](docs/REPORT.md) | 实验报告（性能数据、缓存对比） |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | 系统架构设计 |
| [GUIDE.md](docs/GUIDE.md) | 团队成员指南（赛题、规则、进度） |
| [TESTING.md](docs/TESTING.md) | 测试指南 |
| [DEVELOPMENT.md](docs/DEVELOPMENT.md) | 开发指南 |
| [PROBLEM.md](docs/PROBLEM.md) | 赞题原文 |

---

## 🔧 配置参数

### 图索引参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `max_degree` | 64 | 图节点最大邻居数 |
| `ef_construction` | 200 | 构建时候选列表大小 |
| `ef_search` | 250 | 搜索时候选列表大小 |

### PQ编码参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `pq_M` | 8 | PQ子空间数量 |
| `pq_K` | 256 | 每子空间聚类中心数 |
| `pq_dim` | 16 | 每子空间维度 (128/8) |

### Buffer Pool参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `page_size` | 4096 | 页面大小（4KB对齐） |
| `hot_queue_ratio` | 0.3 | 2Q热队列比例 |
| `enable_graph_aware` | true | 是否启用入度感知淘汰 |
| `capacity` | 动态 | 缓存页面数量（内存预算驱动） |

### I/O参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `queue_depth` | 128 | io_uring队列深度 |
| `use_polling` | false | 是否启用IOPOLL模式 |
| `enable_sqpoll` | false | 是否启用SQ Polling |

---

## 🧪 测试命令

```bash
# 快速单元测试（15个模块）
./agent_mem_io_tests

# ASAN验证测试
cmake .. -DENABLE_SANITIZERS=ON && make && ./agent_mem_io_tests

# 基准测试（10K向量，含SSD读写+io_uring+BufferPool）
./agent_mem_io_benchmark -n 10000 -q 100

# 基准测试（100K向量）
./agent_mem_io_benchmark -n 100000 -q 100

# 纯内存模式（不写SSD，QPS更高）
./agent_mem_io_benchmark -n 10000 -q 100 --no-disk

# 混合读写负载测试
./agent_mem_io_benchmark -n 10000 -q 100 --mixed

# SIFT1M真实数据测试
./agent_mem_io_benchmark --sift1m

# 自定义参数
./agent_mem_io_benchmark -n <向量数> -d <维度> -q <查询数> -k <Top-K>
```

---

## 📈 后续优化方向

1. **SIMD对齐优化**: 128维向量SIMD计算需确保内存对齐，避免unaligned load性能损失
2. **批量距离计算**: 将逐向量距离计算改为批量SIMD计算
3. **SIFT1M大规模测试**: 在100万向量真实数据集上验证
4. **多线程查询并发**: 提升QPS到>1000
5. **Compaction图连通性修复**: 确保LSM合并不破坏图导航性

---

## 📜 许可证

MIT License

---

## 👥 作者

Agent-Mem-IO Team

2026年全国大学生计算机系统能力大赛参赛作品

---

## 🙏 致谢

- [DiskANN (Microsoft)](https://github.com/microsoft/DiskANN) - 参考架构设计
- [HNSWlib](https://github.com/nmslib/hnswlib) - NSW算法参考
- Johnson & Shasha (1994) - 2Q缓存淘汰算法
- 南开大学 - 赞题设计