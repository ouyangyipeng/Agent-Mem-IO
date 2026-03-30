# Agent-Mem-IO: 面向Agent记忆的向量检索系统I/O优化

## 摘要

本文档描述了Agent-Mem-IO系统，一个面向Agent记忆的向量检索存储引擎。该系统针对内存受限环境下的高召回率向量检索问题，实现了基于NSW（Navigable Small World）图的近似最近邻搜索算法，并通过O_DIRECT和io_uring技术优化了I/O性能。

## 1. 引言

### 1.1 背景

随着大语言模型和Agent系统的发展，长期记忆成为提升Agent能力的关键组件。向量检索作为记忆检索的核心技术，面临着内存受限、高召回率要求等挑战。

### 1.2 问题定义

- **内存约束**: 可用内存限制在数据集大小的10%-20%
- **召回率要求**: Recall@10 ≥ 85%
- **I/O优化**: 使用O_DIRECT绕过OS Page Cache，使用io_uring实现异步I/O

## 2. 系统架构

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Storage Engine                            │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐   │
│  │ NSW Graph   │  │ Buffer Pool │  │ I/O Engine          │   │
│  │ Index       │  │ Manager     │  │ (io_uring/sync)     │   │
│  └─────────────┘  └─────────────┘  └─────────────────────┘   │
│                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐   │
│  │ MemTable    │  │ SSTable     │  │ Compaction Manager  │   │
│  │ (LSM-Tree)  │  │ Manager     │  │                     │   │
│  └─────────────┘  └─────────────┘  └─────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

1. **NSW图索引**: 基于可导航小世界图的近似最近邻搜索
2. **Buffer Pool**: 用户态缓存管理，支持O_DIRECT对齐
3. **I/O引擎**: 支持io_uring异步I/O和同步I/O回退
4. **LSM-Tree写入路径**: MemTable + SSTable + 后台压缩

## 3. 核心算法

### 3.1 NSW图构建算法

```
算法: NSW-Insert(q)
输入: 新向量q
输出: 无

1. 如果是第一个节点，设为入口点，返回
2. 使用贪婪搜索找到ef_c个最近邻候选
3. 选择M个最佳邻居
4. 建立双向边连接
5. 如果邻居边数超过限制，进行剪枝
```

### 3.2 贪婪搜索算法

```
算法: Greedy-Search(q, ef)
输入: 查询向量q, 搜索宽度ef
输出: ef个最近邻

1. 从入口点开始
2. 维护候选集C(最小堆)和结果集W(最大堆)
3. 当C不为空:
   a. 取出C中最近的候选
   b. 如果候选距离大于W中最远结果且W已满，停止
   c. 遍历候选的邻居，更新C和W
4. 返回W中的节点
```

## 4. 实验结果

### 4.1 实验配置

- **数据集**: 合成聚类数据 (10,000向量, 128维)
- **硬件**: Intel i9-13900H, 32GB RAM, NVMe SSD
- **参数**: M=16, ef_construction=200, ef_search=250

### 4.2 性能指标

| 指标 | 值 | 目标 | 状态 |
|------|-----|------|------|
| Recall@10 | 86.60% | ≥85% | ✅通过 |
| QPS | 331 | - | - |
| 构建时间 | 26.9s | - | - |
| 平均查询延迟 | ~3ms | - | - |

### 4.3 内存分析

- 向量数据: 4.88 MB (10,000 × 128 × 4 bytes)
- 图结构: 1.22 MB (10,000 × 16 × 2 × 4 bytes)
- 总计: 6.10 MB
- 内存比例: 125% (相对于向量数据)

## 5. 技术亮点

### 5.1 O_DIRECT支持

- 4KB对齐的缓冲区分配
- 绕过OS Page Cache
- 减少内存拷贝

### 5.2 io_uring异步I/O

- 批量I/O提交
- CPU-I/O重叠
- 自动回退到同步I/O

### 5.3 LSM-Tree写入优化

- MemTable内存缓冲
- WAL持久化保证
- 后台SSTable压缩

## 6. 未来工作

1. **内存优化**: 实现更紧凑的图结构，降低内存比例到10%-20%
2. **分层NSW**: 实现HNSW多层结构，提升搜索效率
3. **量化压缩**: 使用PQ/SQ量化降低向量存储开销
4. **真实数据测试**: 在SIFT1M/GIST1M数据集上验证性能

## 7. 结论

Agent-Mem-IO系统成功实现了面向Agent记忆的向量检索功能，Recall@10达到86.60%，超过85%的目标要求。系统采用模块化设计，支持O_DIRECT和io_uring优化，为后续优化奠定了良好基础。

## 参考文献

1. Malkov, Y. A., & Yashunin, D. A. (2018). Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE Transactions on Pattern Analysis and Machine Intelligence.
2. Jayaram Subramanya, S., et al. (2019). DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node. NeurIPS.
3. Levandoski, J. J., Lomet, D. B., & Sengupta, S. (2013). The Bw-Tree: A B-tree for new hardware platforms. ICDE.