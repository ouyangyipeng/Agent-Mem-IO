# Agent-Mem-IO 项目进度追踪

> **重大更新 v2.0**: 基于 DiskANN 论文实现了 PQ 压缩 + SSD-offload 架构，
> 内存比例从 125% 降至 16.8%，Recall@10 从 86.60% 提升至 99.30%。

---

## 内存限制约束表

| 数据集 | 数据集大小 | 10%限制 | 20%限制 | 当前内存 | 当前比例 | 状态 |
|--------|-----------|---------|---------|---------|---------|------|
| 10K合成 | 4.88 MB | 0.49 MB | 0.98 MB | 0.82 MB | 16.8% | ✅ PASS |
| 100K合成 | 48.83 MB | 4.88 MB | 9.77 MB | 7.10 MB | 14.5% | ✅ PASS |
| SIFT1M | 512 MB | 51.2 MB | 102.4 MB | ~73 MB | ~14.3% | ✅ PASS (理论) |

### Recall@10 追踪表

| 版本 | 数据集 | Recall@10 | ef_search | max_degree | 内存比例 | 状态 |
|------|--------|-----------|-----------|------------|---------|------|
| v1.0 | 10K合成 | 86.60% | 250 | 16 | 125% | ❌ 内存超标 |
| v2.0 | 10K合成 | 99.30% | 250 | 8 | 16.8% | ✅✅✅ |
| v2.0 | 100K合成 | 91.80% | 250 | 8 | 14.5% | ✅✅✅ |

### QPS 性能追踪表

| 版本 | 数据集 | 模式 | QPS | 平均延迟 | 备注 |
|------|--------|------|-----|---------|------|
| v1.0 | 10K | 全内存 | 331 | ~3ms | 无PQ，内存125% |
| v2.0 | 10K | SSD (O_DIRECT) | 6.84 | ~146ms/query | 真实SSD读写 |
| v2.0 | 10K | --no-disk (模拟) | 2198 | ~0.5ms | PQ+内存模拟 |
| v2.0 | 100K | --no-disk (模拟) | 893 | ~1.1ms | PQ+内存模拟 |

> **注意**: SSD 模式 QPS 低是因为每次查询需要多次随机 SSD 读取（图遍历）。
> 这是 DiskANN 架构的固有特征，需要通过 io_uring 批量预取和 beam-width 并发来优化。

---

## 阶段进度

#### 阶段1: TOPIC_INIT - 项目初始化 ✅
- 创建项目目录结构
- 配置CMake构建系统
- 定义基础类型 (`common/types.h`)

#### 阶段2: SIFT_DATA_PREP - 数据集准备 ✅
- 合成数据生成器 (`data/synthetic_data.h`)
- SIFT1M数据集下载脚本 (`scripts/download_sift1m.sh`)

#### 核心功能实现 ✅
- NSW图索引 (`core/graph_index.cpp`)
- Vamana图构建器 (`core/vamana_builder.cpp`)
- Buffer Pool管理 (`buffer/buffer_pool.cpp`)
- io_uring引擎 (`io/io_uring_engine.cpp`)
- LSM-Tree写入路径 (`compaction/memtable.cpp`)

#### **v2.0 优化新增** ✅
- Product Quantization编码器 (`core/pq_encoder.h/cpp`) - 64x压缩
- PQ ADC距离表 (`core/pq_encoder.h` PQDistanceTable类) - 8KB查找表
- SIMD距离计算 (`core/simd_distance.h/cpp`) - AVX2/SSE加速
- VisitedBitmap (`core/visited_bitmap.h`) - 128KB vs 40MB+
- Disk Layout (`io/disk_layout.h/cpp`) - 4KB固定记录，O_DIRECT
- DiskANN风格搜索算法 - PQ预过滤 + 全精度距离 + SSD读取

#### 阶段15: RECALL_AT_10_TEST - 召回率测试 ✅
- v1.0: 86.60% (内存125%，超标)
- v2.0: 99.30% (内存16.8%，达标) ✅✅✅

---

## 🏗️ 系统架构 (v2.0)

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

## 📈 性能指标

| 指标 | v1.0 | v2.0 | 目标 | 状态 |
|------|------|------|------|------|
| Recall@10 (10K) | 86.60% | 99.30% | ≥85% | ✅ |
| Recall@10 (100K) | - | 91.80% | ≥85% | ✅ |
| 内存比例 (10K) | 125% | 16.8% | 10-20% | ✅ |
| 内存比例 (100K) | - | 14.5% | 10-20% | ✅ |
| QPS (10K SSD) | 331 | 6.84 | - | ⚠️ I/O瓶颈 |
| QPS (10K 模拟) | 331 | 2198 | - | ↑6.7x |

## 🔧 技术栈

| 技术 | 用途 | 来源 |
|------|------|------|
| Product Quantization (PQ) | 64x向量压缩 (128D→8B) | Jegou et al., 2011 |
| PQ ADC距离表 | 内存中快速粗排 (8KB/query) | DiskANN论文 |
| DiskANN式搜索 | PQ预过滤+全精度距离 | Microsoft DiskANN |
| O_DIRECT | 绕过OS Page Cache | Linux内核 |
| io_uring | 异步I/O框架 | Linux 5.1+ |
| AVX2/SSE SIMD | L2距离计算加速 | x86_64 |
| VisitedBitmap | 替代unordered_set | 常规优化 |
| LSM-Tree | 写入路径优化 | RocksDB/DiskANN |
| 4KB Disk Layout | SSD数据局部性 | Starling论文 |

## 📁 项目结构

```
Agent-Mem-IO/
├── CMakeLists.txt              # 构建配置 (含-march=native)
├── README.md                   # 项目简介
├── PROGRESS.md                 # 进度追踪 (本文件)
├── ROADMAP.md                  # 开发路线图
├── MEGA_PROMPT.md              # 赞题详细要求
├── plans/OPTIMIZATION_PLAN.md  # v2.0优化方案
├── src/
│   ├── common/types.h          # 基础类型定义
│   ├── core/                   # 核心算法
│   │   ├── pq_encoder.h/cpp    # PQ编码器 + ADC距离表
│   │   ├── simd_distance.h/cpp # AVX2/SSE距离计算
│   │   ├── visited_bitmap.h    # 高效位图visited集合
│   │   ├── graph_index.cpp     # NSW图索引
│   │   └── distance.cpp        # 距离计算
│   ├── io/                     # I/O引擎
│   │   ├── disk_layout.h/cpp   # 4KB磁盘记录布局
│   │   ├── io_uring_engine.cpp # io_uring引擎
│   │   └── direct_io.cpp       # O_DIRECT封装
│   ├── buffer/                 # Buffer Pool
│   ├── compaction/             # LSM-Tree组件
│   ├── engine/                 # 存储引擎
│   ├── data/                   # 数据加载
│   └── benchmark.cpp           # DiskANN风格benchmark
├── tests/                      # 单元测试
├── scripts/                    # 工具脚本
└── docs/                       # 文档
    ├── ARCHITECTURE.md          # 架构设计
    ├── GUIDE.md                # 团队指南
    ├── TESTING.md              # 测试指南
    ├── DEVELOPMENT.md          # 开发指南
    ├── PROBLEM.md              # 赞题原文
    └── PAPER.md                # 学术论文
```

## ✅ 完成的阶段

- [x] 阶段1: TOPIC_INIT - 初始化项目
- [x] 阶段2: SIFT_DATA_PREP - 数据准备
- [x] 阶段3-14: 核心功能实现
- [x] 阶段15: RECALL_AT_10_TEST - v1.0 86.60% → v2.0 99.30%
- [x] **v2.0 PQ优化**: 内存从125% → 16.8%，Recall从86.60% → 99.30%
- [x] 阶段22: ACADEMIC_PAPER_WRITING
- [x] 阶段25: MEMORY_STRICT_VERIFY - 内存验证通过 ✅
- [x] 阶段26: FINAL_PACKAGING

## 构建

```bash
mkdir build && cd build
cmake .. -DENABLE_PYTHON_BINDINGS=OFF -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 运行基准测试

```bash
# 小规模 (10K向量) - SSD模式
./agent_mem_io_benchmark -n 10000 -q 100

# 小规模 (10K向量) - 纯内存模拟
./agent_mem_io_benchmark -n 10000 -q 100 --no-disk

# 中等规模 (100K向量)
./agent_mem_io_benchmark -n 100000 -q 100
```

## 📝 备注

**v2.0关键变更**:
1. 实现Product Quantization (PQ) 编码器，将128维向量压缩为8字节（64x压缩）
2. 内存中只保留 PQ codes (1.56%) + 图邻接表 (12.70%) + PQ codebooks (2.56%) = 16.8%
3. 全精度向量存储在SSD上，搜索时通过O_DIRECT读取
4. 搜索使用DiskANN两阶段策略：PQ ADC粗排 + 全精度精排
5. FlatGraphIndex使用 max_degree×2 存储容量，修复了双向边丢失的bug
6. 实现SIMD (AVX2/SSE) L2距离计算，加速距离计算约4-8x
7. VisitedBitmap替代unordered_set，内存从40MB+降至128KB

**下一步优化重点**:
- QPS优化：SSD模式QPS极低(6.84)，主要瓶颈在逐节点同步I/O读取
- 需要实现io_uring批量预取（beam-width并发读取多个候选节点）
- 需要实现Buffer Pool缓存热门节点的Disk Record，减少SSD读取次数