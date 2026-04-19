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
| ✅ Recall@10 ≥ 85% | **99.30%** | v2.0 PQ+DiskANN架构 |
| ✅ 内存 ≤ 20% | **16.8%** | PQ压缩64x，向量SSD offload |
| ✅ NSW图索引 | 已实现 | 可导航小世界图近似最近邻搜索 |
| ✅ PQ编码器 | 已实现 | Product Quantization 64x压缩 |
| ✅ SIMD距离计算 | 已实现 | AVX2/SSE加速L2距离 |
| ✅ O_DIRECT支持 | 已实现 | 绕过OS Page Cache，用户态接管I/O |
| ✅ io_uring异步I/O | 已实现 | Linux 5.1+高性能异步I/O框架 |
| ✅ LSM-Tree写入路径 | 已实现 | MemTable + SSTable + 后台压缩 |
| ✅ DiskANN式搜索 | 已实现 | PQ ADC预过滤 + 全精度距离 + SSD读取 |

---

## 🚀 快速开始

### 环境要求

| 要求 | 版本 | 说明 |
|------|------|------|
| 操作系统 | Ubuntu 22.04+ | 需支持io_uring (Linux 5.1+) |
| 编译器 | GCC 11+ / Clang 14+ | C++20支持 |
| CMake | 3.16+ | 构建系统 |
| liburing | 可选 | io_uring开发包 |

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
# 单元测试
./agent_mem_io_tests

# 基准测试（10K向量，DiskANN风格含SSD读写）
./agent_mem_io_benchmark -n 10000 -q 100

# 基准测试（100K向量）
./agent_mem_io_benchmark -n 100000 -q 100

# 纯内存模式（不写SSD）
./agent_mem_io_benchmark -n 10000 -q 100 --no-disk
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
  Recall@10:  99.30%  ✓
  QPS:        6.84 (SSD mode)

MEMORY REPORT:
  PQ codes (RAM):    0.08 MB (1.56%)
  PQ codebooks (RAM): 128 KB (2.56%)
  Graph adj (RAM):   0.62 MB (12.70%)
  TOTAL RAM:         0.82 MB
  Memory ratio:      16.8%  PASS ✓
```

---

## 📁 项目结构

```
Agent-Mem-IO/
├── CMakeLists.txt              # 构建配置 (含-march=native)
├── README.md                   # 项目简介
├── PROGRESS.md                 # 进度追踪
├── ROADMAP.md                  # 开发路线图
├── MEGA_PROMPT.md              # 赛题详细要求
├── plans/OPTIMIZATION_PLAN.md  # v2.0优化方案
│
├── src/
│   ├── common/types.h          # 基础类型定义
│   ├── core/                   # 核心算法
│   │   ├── pq_encoder.h/cpp    # PQ编码器 + ADC距离表
│   │   ├── simd_distance.h/cpp # AVX2/SSE距离计算
│   │   ├── visited_bitmap.h    # 高效位图visited集合
│   │   ├── graph_index.cpp     # NSW图索引
│   │   ├── vamana_builder.cpp  # Vamana图构建器
│   │   └── distance.cpp        # 距离计算
│   ├── io/                     # I/O引擎
│   │   ├── disk_layout.h/cpp   # 4KB磁盘记录布局 (O_DIRECT)
│   │   ├── io_uring_engine.cpp # io_uring引擎
│   │   └── direct_io.cpp       # O_DIRECT封装
│   │   └── file_manager.cpp    # 文件管理
│   ├── buffer/                 # Buffer Pool
│   │   ├── buffer_pool.h/cpp   # Graph-Aware 2Q缓存
│   │   ├── eviction_policy.cpp # 淘汰策略
│   │   └── page.cpp            # 页面管理
│   ├── compaction/             # LSM-Tree组件
│   │   ├── memtable.h/cpp      # MemTable + WAL
│   │   ├── sstable.cpp         # SSTable管理
│   │   ├── compaction_manager.cpp # 后台压缩
│   ├── engine/                 # 存储引擎
│   │   ├── storage_engine.h/cpp # 统一接口
│   │   ├── query_processor.cpp  # 查询处理
│   │   ├── prefetcher.cpp       # 拓扑感知预取
│   ├── data/                   # 数据加载
│   │   ├── sift_loader.h       # SIFT1M加载器
│   │   ├── synthetic_data.h    # 合成数据生成
│   ├── main.cpp                # CLI入口
│   └── benchmark.cpp           # DiskANN风格benchmark
│
├── tests/test_main.cpp         # 单元测试
├── scripts/download_sift1m.sh  # SIFT数据集下载
└── docs/                       # 文档
    ├── ARCHITECTURE.md          # 架构设计
    ├── GUIDE.md                # 团队指南
    ├── TESTING.md              # 测试指南
    ├── DEVELOPMENT.md          # 开发指南
    ├── PROBLEM.md              # 赛题原文
    └── PAPER.md                # 学术论文
```

---

## 📊 性能指标

| 指标 | v1.0 | v2.0 | 目标 | 状态 |
|------|------|------|------|------|
| Recall@10 (10K) | 86.60% | **99.30%** | ≥85% | ✅ |
| Recall@10 (100K) | - | 91.80% | ≥85% | ✅ |
| 内存比例 (10K) | 125% | **16.8%** | 10-20% | ✅ |
| 内存比例 (100K) | - | 14.5% | 10-20% | ✅ |

---

## 🏗️ 技术架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Agent-Mem-IO v2.0 Architecture                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    In-Memory Components (~14-17% budget)            │   │
│  │                                                                      │   │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────────┐     │   │
│  │  │ PQ Codes       │  │ Graph Nav Data │  │ PQ Codebooks      │     │   │
│  │  │ (N×8B, 1.56%) │  │ (adj. lists    │  │ (8×256×16×4B      │     │   │
│  │  │ 64x压缩        │  │  12.70%)       │  │  =128KB, 2.56%)   │     │   │
│  │  └────────────────┘  └────────────────┘  └────────────────────┘     │   │
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

### 核心技术

1. **Product Quantization (PQ)**: 128维→8字节（64x压缩），仅PQ codes + codebooks 常驻内存
2. **DiskANN式搜索**: PQ ADC粗排预过滤 → 全精度距离计算（SSD O_DIRECT读取）
3. **O_DIRECT + Buffer Pool**: 绕过OS Page Cache，用户态Graph-Aware 2Q缓存管理
4. **io_uring异步I/O**: 批量提交、零拷贝、CPU-I/O重叠
5. **SIMD距离计算**: AVX2/SSE加速L2距离，4x-8x提速
6. **VisitedBitmap**: 替代unordered_set，128KB vs 40MB+
7. **4KB Disk Layout**: SSD友好的固定记录大小，对齐O_DIRECT要求
8. **LSM-Tree写入路径**: 随机写转顺序追加写，后台异步压缩

---

## 📚 文档

| 文档 | 说明 |
|------|------|
| [GUIDE.md](docs/GUIDE.md) | 团队成员指南（赛题、规则、进度） |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | 系统架构设计 |
| [TESTING.md](docs/TESTING.md) | 测试指南 |
| [DEVELOPMENT.md](docs/DEVELOPMENT.md) | 开发指南 |
| [PROBLEM.md](docs/PROBLEM.md) | 赛题原文 |
| [PAPER.md](docs/PAPER.md) | 学术论文 |

---

## 🔧 配置参数

### 图索引参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `max_degree` | 8 | 图节点最大邻居数 (v2.0调低以降低内存) |
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
| `capacity` | 动态 | 缓存页面数量 |

### I/O参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `queue_depth` | 128 | io_uring队列深度 |
| `use_io_uring` | true | 是否启用io_uring |

---

## 🧪 测试命令

```bash
# 快速单元测试
./agent_mem_io_tests

# 基准测试（10K向量，含SSD读写）
./agent_mem_io_benchmark -n 10000 -q 100

# 基准测试（100K向量）
./agent_mem_io_benchmark -n 100000 -q 100

# 纯内存模式（不写SSD，QPS更高）
./agent_mem_io_benchmark -n 10000 -q 100 --no-disk

# 自定义参数
./agent_mem_io_benchmark -n <向量数> -d <维度> -q <查询数> -k <Top-K>
```

---

## 📈 后续优化方向

1. **QPS优化**: 当前SSD模式QPS较低(6.84)，需优化I/O路径（io_uring批量预取、beam-width并发）
2. **预取优化**: 基于图拓扑的智能预取，减少I/O等待
3. **并行化**: 多线程查询处理，提升QPS
4. **压缩优化**: SSTable压缩，减少存储空间

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
- 南开大学 - 赛题设计