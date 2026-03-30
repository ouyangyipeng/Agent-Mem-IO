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
| ✅ Recall@10 ≥ 85% | **86.60%** | 超过赛题要求的召回率 |
| ✅ NSW图索引 | 已实现 | 基于可导航小世界图的近似最近邻搜索 |
| ✅ O_DIRECT支持 | 已实现 | 绕过OS Page Cache，用户态接管I/O调度 |
| ✅ io_uring异步I/O | 已实现 | Linux 5.1+高性能异步I/O框架 |
| ✅ LSM-Tree写入路径 | 已实现 | MemTable + SSTable + 后台压缩 |
| ⚠️ 内存优化 | 进行中 | 当前125%，目标10%-20% |

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
cmake ..
make -j$(nproc)
```

### 运行测试

```bash
# 单元测试
./agent_mem_io_tests

# 基准测试
./agent_mem_io_benchmark -n 100000 -d 128 -q 100 -k 10
```

### 预期输出

```
=== Agent-Mem-IO Benchmark ===
Configuration:
  Vectors: 100000
  Dimensions: 128
  Queries: 100
  K: 10
  Max Degree: 16
  EF Search: 250

Results:
  QPS: 331.13
  Avg Latency: 3.02 ms
  Recall@10: 86.60%  ✅
  Memory Usage: 125.00 MB
```

---

## 📁 项目结构

```
Agent-Mem-IO/
├── CMakeLists.txt          # 构建配置
├── README.md               # 项目简介
├── PROGRESS.md             # 进度追踪
├── ROADMAP.md              # 开发路线图
├── MEGA_PROMPT.md          # 赛题详细要求
│
├── src/                    # 源代码
│   ├── common/             # 公共类型定义
│   ├── core/               # 核心算法（NSW图索引、距离计算）
│   ├── buffer/             # Buffer Pool管理
│   ├── io/                 # I/O引擎（io_uring、O_DIRECT）
│   ├── compaction/         # LSM-Tree组件（MemTable、SSTable）
│   ├── engine/             # 存储引擎
│   ├── data/               # 数据加载器
│   ├── bindings/           # Python绑定
│   ├── main.cpp            # CLI入口
│   └── benchmark.cpp       # 基准测试
│
├── tests/                  # 单元测试
├── scripts/                # 工具脚本
├── docs/                   # 文档
└── data/                   # 数据目录
```

---

## 📊 性能指标

| 指标 | 当前值 | 目标值 | 状态 |
|------|--------|--------|------|
| Recall@10 | 86.60% | ≥85% | ✅ 达标 |
| QPS (100K向量) | 331 | >1000 | ⚠️ 待优化 |
| 平均查询延迟 | ~3ms | <10ms | ✅ 达标 |
| 内存比例 | 125% | 10%-20% | ⚠️ 待优化 |

---

## 🏗️ 技术架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Storage Engine                            │
│  (统一接口层)                                                │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ NSW Graph   │  │ Buffer Pool │  │ I/O Engine          │  │
│  │ Index       │  │ Manager     │  │ (io_uring/sync)     │  │
│  │ (core/)     │  │ (buffer/)   │  │ (io/)               │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ MemTable    │  │ SSTable     │  │ Compaction Manager  │  │
│  │ (LSM-Tree)  │  │ Manager     │  │                     │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 核心技术

1. **NSW图索引**: 可导航小世界图，实现高效的近似最近邻搜索
2. **O_DIRECT + Buffer Pool**: 绕过OS Page Cache，用户态管理缓存
3. **io_uring异步I/O**: 批量提交、零拷贝、CPU-I/O重叠
4. **LSM-Tree写入路径**: 随机写转顺序追加写，后台异步压缩

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
| `max_degree` | 16 | 图节点最大邻居数 |
| `ef_construction` | 200 | 构建时候选列表大小 |
| `ef_search` | 250 | 搜索时候选列表大小 |

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
# 快速测试
./agent_mem_io_tests

# 基准测试（小规模）
./agent_mem_io_benchmark -n 10000 -q 100

# 基准测试（中等规模）
./agent_mem_io_benchmark -n 100000 -q 100

# 基准测试（大规模）
./agent_mem_io_benchmark -n 1000000 -q 1000

# 自定义参数
./agent_mem_io_benchmark -n <向量数> -d <维度> -q <查询数> -k <Top-K>
```

---

## 📈 后续优化方向

1. **内存优化**: 实现更高效的缓存淘汰策略，达到10%-20%目标
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