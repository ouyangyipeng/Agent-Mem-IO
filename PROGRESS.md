# Agent-Mem-IO 项目进度追踪

> **重大更新 v6.1**: io_uring async_buffers_线程安全化 + PQ ADC阈值优化 + entry point pinning。
> SSD实测: SIFT 10K QPS=7.62(1线程)/9.25(4线程), SIFT 100K QPS=5.38, Recall=98.90%/99.60%, Memory=20.0%, Cache=66.7%。
> ⚠️ 内存模式QPS(1961/12887/770/335)违反赛题约束(~100%内存)，仅供对比参考。
> v5.1: SIFT1M全量1M向量benchmark通过！Recall@10=98.30%, Memory=14.3%, QPS=335(内存模式)。
> v5.0: Vamana两遍构建 + medoid entry point + robust_prune + graph compact + VisitedBitmap + SIMD + ef_search + compute_distance_direct。

---

## 内存限制约束表

| 数据集 | 数据集大小 | 10%限制 | 20%限制 | 当前内存 | 当前比例 | 状态 |
|--------|-----------|---------|---------|---------|---------|------|
| 10K合成 | 4.88 MB | 0.49 MB | 0.98 MB | 0.82 MB | 16.8% | ✅ PASS |
| SIFT 10K | 4.88 MB | 0.49 MB | 0.98 MB | 0.82 MB | 16.8% | ✅ PASS |
| SIFT 100K | 48.83 MB | 4.88 MB | 9.77 MB | 7.10 MB | 14.5% | ✅ PASS |
| SIFT 1M | 488.28 MB | 48.83 MB | 97.66 MB | 69.86 MB | 14.3% | ✅ PASS |

### Recall@10 追踪表

| 版本 | 数据集 | Recall@10 | ef_search | max_degree | 内存比例 | 状态 |
|------|--------|-----------|-----------|------------|---------|------|
| v1.0 | 10K合成 | 86.60% | 250 | 16 | 125% | ❌ 内存超标 |
| v3.0 | 10K合成(简单) | 99.30% | 250 | 8 | 20.0% | ✅ (数据太简单) |
| v5.0 | 10K合成(harder) | 85.30% | 350 | 16 | 16.8% | ✅✅✅ |
| v5.0 | SIFT 10K | 98.90% | 350 | 16 | 16.8% | ✅✅✅ |
| v5.0 | SIFT 100K | 99.60% | 350 | 16 | 14.5% | ✅✅✅ |
| v5.1 | SIFT 1M | 98.30% | 350 | 16 | 14.3% | ✅✅✅ |

### QPS 性能追踪表

| 版本 | 数据集 | 模式 | QPS | P99延迟 | 内存比例 | 合规 | 备注 |
|------|--------|------|-----|---------|---------|------|------|
| v6.1 | SIFT 10K | SSD+io_uring | **7.62** | 3013.90ms | 20.0% | ✅ | PQ ADC阈值+entry pinning |
| v6.1 | SIFT 10K | SSD 4线程 | **9.25** | 720.26ms | 20.0% | ✅ | 多线程并发同步读取 |
| v6.1 | SIFT 100K | SSD+io_uring | **5.38** | 3095.74ms | 20.0% | ✅ | PQ ADC阈值+io_uring |
| v5.2 | SIFT 1M | SSD+io_uring | **5.99** | 291.23ms | 20.0% | ✅ | 赛题核心场景 |
| v5.2 | SIFT 10K | --no-disk | 1961 | ~1.7ms | ~100% | ❌ | ⚠️违反≤20%约束 |
| v5.2 | SIFT 10K | --no-disk 4线程 | 12887 | ~1.7ms | ~100% | ❌ | ⚠️违反≤20%约束 |
| v5.2 | SIFT 100K | --no-disk | 770 | ~4.3ms | ~100% | ❌ | ⚠️违反≤20%约束 |
| v5.1 | SIFT 1M | --no-disk | 335 | ~13.6ms | ~100% | ❌ | ⚠️违反≤20%约束 |

### 延迟统计 (v5.1, SIFT 1M, 内存模式)

| 指标 | SIFT 10K | SIFT 100K | SIFT 1M |
|------|----------|-----------|---------|
| Avg延迟 | 0.66 ms | 1.85 ms | 2.98 ms |
| P99延迟 | 1.71 ms | 11.16 ms | 13.59 ms |
| P99.9延迟 | 1.71 ms | 11.16 ms | 13.59 ms |
| QPS | 1516 | 541 | 335 |
| Recall@10 | 98.90% | 99.60% | 98.30% |
| 内存比例 | 16.8% | 14.5% | 14.3% |

---

## 阶段进度

#### 阶段1: TOPIC_INIT - 项目初始化 ✅
#### 阶段2: SIFT_DATA_PREP - 数据集准备 ✅
#### 核心功能实现 ✅ (NSW图索引、Buffer Pool、io_uring、LSM-Tree)
#### v2.0 PQ优化 ✅ (PQ编码器、Disk Layout、SIMD、VisitedBitmap)
#### v3.0 增强优化 ✅ (BufferPool、beam-style搜索、混合负载)
#### v5.0 Vamana优化 ✅ (两遍构建+medoid+robust_prune+compact+VisitedBitmap+compute_distance_direct)
#### v5.1 1M全量验证 ✅ (自适应Phase2 ef=100、VisitedBitmap复用、ground truth子集兼容、SIFT1M Recall=98.30% Memory=14.3%)
#### v5.2 搜索热路径优化 ✅ (search_memory_fast: 10K QPS 1516→1961, 4线程→12887, 100K→770)

---

## 🏗️ 系统架构 (v3.0)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Agent-Mem-IO v3.0 Architecture                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    In-Memory Components (≤20% budget)               │   │
│  │  PQ Codes(1.56%) + Graph(12.70%) + Codebooks(2.56%)                │   │
│  │  + Vector Cache(3.1%) + VisitedBitmap(0.02%) = 20.0%              │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                      │ PQ ADC预过滤 + beam-style批量预取   │
│                                      │ O_DIRECT SSD读取                    │
│                                      ▼                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    SSD-Resident Components (O_DIRECT)              │   │
│  │  Disk Node Records (4KB each: vector+neighbors+neighborPQcodes)    │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    LSM-Tree Write Path                            │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 📈 性能指标

### SSD模式（赛题核心场景，内存≤20%）

| 指标 | v5.2实测 | 目标 | 合规 | 状态 |
|------|---------|------|------|------|
| Recall@10 (SIFT 10K, SSD) | 98.90% | ≥85% | ✅ | ✅ |
| Recall@10 (SIFT 100K, SSD) | 99.60% | ≥85% | ✅ | ✅ |
| QPS (SSD, SIFT 10K, 1线程) | 7.62 | - | ✅ | PQ ADC阈值+entry pinning |
| QPS (SSD, SIFT 10K, 4线程) | 9.25 | - | ✅ | 多线程并发同步读取 |
| QPS (SSD, SIFT 100K, 1线程) | 5.38 | - | ✅ | PQ ADC阈值+io_uring |
| 内存比例 | 20.0% | 10-20% | ✅ | ✅ |
| 缓存命中率 (Graph-Aware 2Q) | 66.8% | - | - | ↑111x vs LRU |

### 内存模式（⚠️违反≤20%约束，仅供对比）

| 指标 | v5.2实测 | 内存比例 | 合规 | 备注 |
|------|---------|---------|------|------|
| QPS (内存, SIFT 10K, 1线程) | 1961 | ~100% | ❌ | ⚠️违反≤20%约束 |
| QPS (内存, SIFT 10K, 4线程) | 12887 | ~100% | ❌ | ⚠️违反≤20%约束 |
| QPS (内存, SIFT 100K, 1线程) | 770 | ~100% | ❌ | ⚠️违反≤20%约束 |
| QPS (内存, SIFT 1M, 1线程) | 335 | ~100% | ❌ | ⚠️违反≤20%约束 |

## ✅ 完成的阶段

- [x] v1.0: NSW图索引 + 全内存搜索 (Recall 86.60%, Memory 125%)
- [x] v2.0: PQ编码器 + Disk Layout + SSD-offload (Recall 99.30%, Memory 16.8%)
- [x] v3.0: VectorCache + beam-style搜索 + P99延迟统计 (Memory 20.0%)
- [x] 单元测试: 12个模块全部通过
- [x] 学术论文 PAPER.md 已更新
- [x] 实验报告 REPORT.md 已创建

## 构建

```bash
mkdir build && cd build
cmake .. -DENABLE_PYTHON_BINDINGS=OFF -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 运行基准测试

```bash
# SSD模式 (10K向量)
./agent_mem_io_benchmark -n 10000 -q 100

# 内存模拟模式
./agent_mem_io_benchmark -n 10000 -q 100 --no-disk

# 混合读写负载
./agent_mem_io_benchmark -n 10000 -q 100 --mixed

# 单元测试
./agent_mem_io_tests