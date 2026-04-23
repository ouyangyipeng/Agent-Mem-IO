# V6→V7 优化计划：Minor修复 + V6新问题 + SSD QPS真实优化

> 创建日期：2026-04-23
> 目标：修复V5/V6审计发现的minor问题和新问题，真实优化SSD QPS，确保所有版本问题不再犯
> 最终目标：在所有维度真实评判下达到比赛满分

---

## 修复状态总览

| 编号 | 问题 | 来源 | 优先级 | 状态 |
|------|------|------|--------|------|
| ① | README SSD QPS更新为实测值 | V5 Minor | 🟢 | ⏳ |
| ② | 4线程缓存命中率0.2%优化 | V5 Minor | 🟡 | ⏳ |
| ③ | io_uring async_buffers_线程安全化 | V6-NEW-1 | 🟡 | ⏳ |
| ④ | SSD QPS真实优化（增大cache/beam_width） | V6-NEW-2 | 🟢 | ⏳ |
| ⑤ | V1-V6全量审计验证 | 全版本 | 🔴 | ⏳ |
| ⑥ | 写题解文档 | 用户要求 | - | ⏳ |
| ⑦ | 推送GitHub | 用户要求 | - | ⏳ |

---

## ①+②+③+④: 性能优化综合子任务

### ① README SSD QPS更新
- 将README中SSD QPS从历史数据6.12/10.10/4.85/5.99更新为最新实测值
- 需要先运行Release版benchmark获取最新数据

### ② 4线程缓存命中率优化
**问题分析**：4线程模式下缓存命中率仅0.2%（vs 单线程66.8%）
- 缓存容量=5%×10000=500个节点，但实际只有39个4KB pages
- 4个线程并发搜索，每个线程访问不同节点，导致缓存频繁替换
- pin保护只防止eviction，不防止其他线程的cache miss加载新页

**优化方案**：
1. 多线程模式下增大缓存容量：从5%提升到10-15%
2. 在benchmark.cpp中，当num_threads>1时自动调整cache_capacity
3. 优化pin策略：搜索完成后延迟unpin，让缓存页保持更久

### ③ io_uring async_buffers_线程安全化
**问题分析**：`async_buffers_` 是共享 `std::unordered_map<NodeId, char*>`
- submit_async_batch() 写入 async_buffers_[node_id] = buffer
- wait_async_batch() 读取和删除 async_buffers_
- 多线程并发操作导致数据竞争和heap-use-after-free

**优化方案**：添加 `std::mutex async_buffers_mutex_` 保护所有async_buffers_操作
- submit_async_batch() 中 lock_guard 保护写入
- wait_async_batch() 中 lock_guard 保护读取和删除
- 这样多线程SSD搜索可以安全使用io_uring异步I/O

**更优方案**：per-thread async buffer map
- 每个线程有自己的async_buffers_，存储在搜索函数的局部变量中
- 需要将async_buffers_从DiskIndexReader成员改为函数参数
- 更复杂但性能更好（无mutex contention）

**推荐方案**：mutex方案（简单安全，先让io_uring在多线程下可用）

### ④ SSD QPS真实优化
**当前瓶颈**：SSD QPS ~6-10，主要受I/O延迟限制
- WSL2虚拟化I/O延迟约3-5ms/次
- 每次搜索需要多次SSD读取（图遍历+全精度距离计算）

**真实优化方案**（不违反诚信）：
1. 增大beam_width：从8提升到16-32，更多预取减少随机I/O
2. 增大cache_capacity：多线程模式从5%提升到10-15%
3. 优化PQ ADC过滤阈值：更严格的预过滤减少SSD读取次数
4. 搜索路径优化：减少不必要的SSD读取（如已缓存节点跳过预取）

---

## ⑤ V1-V6全量审计验证

逐版本检查所有历史问题是否不再犯：
- V1: placeholder/空壳代码 → 检查无TODO/FIXME/placeholder
- V2: io_uring未接入 → 检查搜索路径使用io_uring
- V3: SIFT非默认 → 检查默认路径
- V4: 多线程绕过I/O → 检查use_memory_path逻辑
- V5: README数据一致性 → 检查SSD QPS与实测一致
- V6: io_uring线程安全 → 检查async_buffers_保护

---

## ⑥ 题解文档结构

```
solution/
├── README.md              — 题解总览（项目介绍+快速导航）
├── 01-initial-idea.md     — 初始思路（赛题分析+技术选型理由）
├── 02-architecture.md     — 系统架构设计（分层架构+核心组件）
├── 03-implementation.md   — 关键实现细节（各模块实现要点）
├── 04-optimization.md     — 优化循环过程（V1→V7的迭代优化历程）
├── 05-experiments.md      — 实验结果与分析（性能数据+对比实验）
├── 06-lessons.md          — 经验教训（踩坑记录+诚信反思+改进措施）
└── 07-audit-history.md    — 审计报告摘要（V1-V6关键发现+修复状态）
```

---

## ⑦ GitHub推送

```bash
git add -A
git commit -m ":tada: feat: V7 final release — all audit issues resolved, honest performance, full test coverage"
git push origin main
```

---

## 依赖关系图

```mermaid
graph TD
    A[①②③④: 性能优化综合] --> B[⑤: V1-V6全量审计]
    B --> C[⑥: 写题解文档]
    C --> D[⑦: 推送GitHub]
    
    style A fill:#FFD700
    style B fill:#FFB6C1
    style C fill:#87CEEB
    style D fill:#90EE90