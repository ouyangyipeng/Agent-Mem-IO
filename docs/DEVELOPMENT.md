# Agent-Mem-IO 开发指南

> 本文档面向项目开发者，介绍代码结构、开发规范和贡献流程。

---

## 一、项目结构

### 1.1 目录结构

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
│   │   └── types.h         # 基础类型、常量、错误码
│   │
│   ├── core/               # 核心算法
│   │   ├── distance.cpp    # 距离计算（L2平方）
│   │   ├── graph_index.cpp # NSW图索引
│   │   ├── graph_index.h   # 图索引接口
│   │   ├── vamana_builder.cpp # Vamana图构建
│   │   └── vector_dataset.cpp # 向量数据集
│   │
│   ├── buffer/             # Buffer Pool管理
│   │   ├── buffer_pool.cpp # 缓存池实现
│   │   ├── buffer_pool.h   # 缓存池接口
│   │   ├── eviction_policy.cpp # 淘汰策略
│   │   └── page.cpp        # 页面管理
│   │
│   ├── io/                 # I/O引擎
│   │   ├── direct_io.cpp   # O_DIRECT实现
│   │   ├── file_manager.cpp # 文件管理
│   │   ├── io_uring_engine.cpp # io_uring引擎
│   │   └── io_uring_engine.h   # io_uring接口
│   │
│   ├── compaction/         # LSM-Tree组件
│   │   ├── memtable.cpp    # MemTable实现
│   │   ├── memtable.h      # MemTable接口
│   │   ├── sstable.cpp     # SSTable实现
│   │   ├── compaction_manager.cpp # 压缩管理
│   │   └ wal.cpp           # WAL日志
│   │
│   ├── engine/             # 存储引擎
│   │   ├── storage_engine.cpp # 存储引擎实现
│   │   ├── storage_engine.h   # 存储引擎接口
│   │   ├── query_processor.cpp # 查询处理
│   │   └ prefetcher.cpp    # 预取器
│   │
│   ├── data/               # 数据加载
│   │   ├── sift_loader.h   # SIFT数据加载器
│   │   └ synthetic_data.h  # 合成数据生成器
│   │
│   ├── bindings/           # 语言绑定
│   │   └ python_bindings.cpp # Python绑定
│   │
│   ├── main.cpp            # CLI入口
│   └ benchmark.cpp         # 基准测试
│
├── tests/                  # 测试代码
│   └ test_main.cpp         # 单元测试
│
├── scripts/                # 工具脚本
│   └ download_sift1m.sh    # 数据下载脚本
│
├── docs/                   # 文档
│   ├── ARCHITECTURE.md     # 架构设计
│   ├── PROBLEM.md          # 赛题分析
│   ├── PAPER.md            # 学术论文
│   ├── GUIDE.md            # 团队指南
│   ├── TESTING.md          # 测试指南
│   └ DEVELOPMENT.md        # 开发指南（本文件）
│
├── data/                   # 数据目录
│   └ sift1m/               # SIFT1M数据
│
└── build/                  # 构建输出
```

### 1.2 核心模块依赖关系

```
┌─────────────────────────────────────────────────────────────┐
│                    Storage Engine                            │
│  (src/engine/storage_engine.cpp)                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ Graph Index │  │ Buffer Pool │  │ I/O Engine          │  │
│  │ (core/)     │  │ (buffer/)   │  │ (io/)               │  │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘  │
│         │               │                    │              │
│         └───────────────┴────────────────────┘              │
│                         │                                    │
│                         ▼                                    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              Common Types (common/types.h)          │    │
│  └─────────────────────────────────────────────────────┘    │
│                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ MemTable    │  │ SSTable     │  │ Compaction Manager  │  │
│  │ (compaction/)│ │ (compaction/)│ │ (compaction/)       │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、核心类型定义

### 2.1 基础类型 (`src/common/types.h`)

```cpp
namespace agent_mem_io {

// 基础类型别名
using NodeId = uint32_t;      // 节点ID
using Dim = uint32_t;         // 向量维度
using Distance = float;       // 距离值
using Vector = std::vector<float>;  // 向量数据
using PageId = uint64_t;      // 页面ID
using Size = uint64_t;        // 大小/容量

// 常量定义
constexpr Size PAGE_SIZE = 4096;        // 4KB页面大小
constexpr Size MAX_DEGREE = 16;         // 图索引最大度数
constexpr Size EF_CONSTRUCTION = 200;   // 构建时候选列表大小
constexpr Size EF_SEARCH = 250;         // 搜索时候选列表大小

// 错误码
enum class Error {
    OK = 0,
    IO_ERROR = 1,
    MEMORY_ERROR = 2,
    INVALID_ARGUMENT = 3,
    NOT_FOUND = 4,
    // ...
};

}  // namespace agent_mem_io
```

### 2.2 图索引接口 (`src/core/graph_index.h`)

```cpp
class GraphIndex {
public:
    // 插入节点
    Error insert(NodeId id, const Vector& vector);
    
    // 搜索Top-K
    Error search(const Vector& query, Size k, 
                 std::vector<NodeId>& results);
    
    // 获取邻居列表
    const std::vector<NodeId>& get_neighbors(NodeId id) const;
    
    // 获取入口点
    NodeId get_entry_point() const;
};
```

### 2.3 Buffer Pool接口 (`src/buffer/buffer_pool.h`)

```cpp
class BufferPool {
public:
    // 初始化
    Error init(Size capacity, Size page_size);
    
    // 获取页面
    Error get_page(PageId id, char** data);
    
    // 释放页面
    void release_page(PageId id);
    
    // 淘汰策略
    void set_eviction_policy(std::unique_ptr<EvictionPolicy> policy);
};
```

### 2.4 I/O引擎接口 (`src/io/io_uring_engine.h`)

```cpp
class IoEngine {
public:
    // 提交异步读请求
    Error submit_read(int fd, void* buffer, Size size, 
                      Size offset, IoCompletion* completion);
    
    // 提交异步写请求
    Error submit_write(int fd, const void* buffer, Size size,
                       Size offset, IoCompletion* completion);
    
    // 等待完成
    Error wait_completion(IoCompletion* completion, int timeout_ms);
    
    // 是否使用io_uring
    bool using_io_uring() const;
};
```

---

## 三、开发规范

### 3.1 代码风格

**命名规范**:
- 类名: `PascalCase` (如 `GraphIndex`, `BufferPool`)
- 函数名: `snake_case` (如 `get_neighbors`, `submit_read`)
- 变量名: `snake_case` (如 `entry_point`, `page_size`)
- 常量: `UPPER_CASE` (如 `PAGE_SIZE`, `MAX_DEGREE`)
- 成员变量: 末尾加 `_` (如 `capacity_`, `buffer_`)

**格式化**:
- 使用 4 空格缩进
- 大括号独占一行
- 每行不超过 120 字符
- 函数之间空一行

**注释规范**:
```cpp
/**
 * @brief 简要描述函数功能
 * 
 * @param param1 参数1说明
 * @param param2 参数2说明
 * @return 返回值说明
 * 
 * @note 注意事项
 * @warning 警告信息
 */
Error search(const Vector& query, Size k, 
             std::vector<NodeId>& results);
```

### 3.2 错误处理

**使用 Error 枚举**:
```cpp
// 正确做法
Error GraphIndex::insert(NodeId id, const Vector& vector) {
    if (vector.empty()) {
        return Error::INVALID_ARGUMENT;
    }
    // ...
    return Error::OK;
}

// 错误做法 - 不要使用异常
Error GraphIndex::insert(NodeId id, const Vector& vector) {
    if (vector.empty()) {
        throw std::invalid_argument("empty vector");  // ❌
    }
    // ...
}
```

**错误检查**:
```cpp
Error err = engine->search(query, k, results);
if (err != Error::OK) {
    // 处理错误
    log_error("Search failed: {}", error_to_string(err));
    return err;
}
```

### 3.3 内存管理

**使用智能指针**:
```cpp
// 正确做法
std::unique_ptr<BufferPool> pool = std::make_unique<BufferPool>();
std::shared_ptr<IoEngine> engine = create_io_engine();

// 错误做法 - 不要使用裸指针new/delete
BufferPool* pool = new BufferPool();  // ❌
delete pool;  // ❌
```

**对齐内存分配**:
```cpp
// O_DIRECT需要4KB对齐
void* buffer;
posix_memalign(&buffer, PAGE_SIZE, buffer_size);
// 使用后释放
free(buffer);
```

### 3.4 日志规范

```cpp
// 使用统一的日志宏
#define LOG_DEBUG(fmt, ...) ...
#define LOG_INFO(fmt, ...)  ...
#define LOG_WARN(fmt, ...)  ...
#define LOG_ERROR(fmt, ...) ...

// 示例
LOG_INFO("Building index with {} vectors", num_vectors);
LOG_DEBUG("Node {} has {} neighbors", node_id, neighbors.size());
LOG_ERROR("IO error at offset {}: {}", offset, strerror(errno));
```

---

## 四、开发流程

### 4.1 分支管理

```
main (主分支)
  │
  ├─── feature/xxx (功能分支)
  │
  ├─── fix/xxx (修复分支)
  │
  └─── refactor/xxx (重构分支)
```

**分支命名规范**:
- 功能分支: `feature/<功能名称>`
- 修复分支: `fix/<问题描述>`
- 重构分支: `refactor/<重构内容>`

### 4.2 Commit规范

遵循 Conventional Commits 规范：

```
:<gitmoji>: <type>(<scope>): <subject>

<body>

<footer>
```

**示例**:
```
:sparkles: feat(graph): add NSW greedy search algorithm

Implement the greedy search algorithm for NSW graph index.
The search starts from entry point and traverses to nearest neighbors.

Recall@10: 86.60%
```

**Type类型**:
- `feat`: 新功能
- `fix`: Bug修复
- `refactor`: 重构
- `docs`: 文档更新
- `test`: 测试相关
- `chore`: 杂项（构建、配置等）

### 4.3 Pull Request流程

1. **创建分支**:
   ```bash
   git checkout -b feature/new-feature
   ```

2. **开发并测试**:
   ```bash
   # 编写代码
   # 运行测试
   make -j$(nproc) && ./agent_mem_io_tests
   ```

3. **提交代码**:
   ```bash
   git add .
   git commit -m ":sparkles: feat(core): add new feature"
   ```

4. **推送分支**:
   ```bash
   git push origin feature/new-feature
   ```

5. **创建PR**: 在GitHub上创建Pull Request

6. **代码审查**: 等待审查通过后合并

---

## 五、调试技巧

### 5.1 启用调试日志

```cpp
// 在编译时定义调试宏
cmake -DDEBUG_LOG=ON ..

// 或在代码中
#define DEBUG_LEVEL 2
```

### 5.2 使用GDB调试

```bash
# 编译时添加调试信息
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 使用GDB
gdb ./agent_mem_io_tests

# 常用命令
(gdb) break GraphIndex::search
(gdb) run
(gdb) next
(gdb) print neighbors
(gdb) continue
```

### 5.3 性能分析

```bash
# 使用perf分析
perf record -g ./agent_mem_io_benchmark -n 10000
perf report

# 使用valgrind检查内存
valgrind --leak-check=full ./agent_mem_io_tests
```

### 5.4 io_uring调试

```bash
# 检查io_uring是否启用
cat /proc/sys/kernel/io_uring_enabled
# 应返回 1

# 查看io_uring统计
cat /proc/<pid>/io_uring
```

---

## 六、常见开发任务

### 6.1 添加新的距离函数

```cpp
// 1. 在 src/core/distance.cpp 中添加函数
Distance compute_cosine_distance(const Vector& v1, const Vector& v2) {
    // 实现余弦距离
    ...
}

// 2. 在 src/core/distance.h 中声明
Distance compute_cosine_distance(const Vector& v1, const Vector& v2);

// 3. 在测试中验证
void test_cosine_distance() {
    Vector v1 = {1.0, 0.0, 0.0};
    Vector v2 = {0.0, 1.0, 0.0};
    Distance d = compute_cosine_distance(v1, v2);
    assert(std::abs(d - 1.0) < 0.001);
}
```

### 6.2 添加新的淘汰策略

```cpp
// 1. 在 src/buffer/eviction_policy.h 中定义接口
class NewEvictionPolicy : public EvictionPolicy {
public:
    PageId select_for_eviction() override;
    void on_access(PageId id) override;
    void on_insert(PageId id) override;
};

// 2. 在 src/buffer/eviction_policy.cpp 中实现
PageId NewEvictionPolicy::select_for_eviction() {
    // 实现淘汰逻辑
    ...
}

// 3. 在 BufferPool 中使用
pool->set_eviction_policy(std::make_unique<NewEvictionPolicy>());
```

### 6.3 添加新的I/O操作

```cpp
// 1. 在 src/io/io_uring_engine.h 中添加接口
Error submit_batch_read(const std::vector<IoRequest>& requests);

// 2. 在 src/io/io_uring_engine.cpp 中实现
Error IoEngine::submit_batch_read(const std::vector<IoRequest>& requests) {
    for (const auto& req : requests) {
        // 批量提交
        io_uring_prep_read(sqe, req.fd, req.buffer, req.size, req.offset);
    }
    io_uring_submit(&ring_);
    return Error::OK;
}
```

---

## 七、性能优化指南

### 7.1 内存优化

- 减少不必要的拷贝：使用 `const&` 和 `std::move`
- 预分配内存：`vector.reserve(n)`
- 使用内存池：避免频繁分配释放

### 7.2 I/O优化

- 批量I/O：合并多个小请求
- 异步I/O：使用 io_uring
- 预取：提前加载可能需要的页面

### 7.3 算法优化

- 减少距离计算：使用近似距离或缓存
- 优化图遍历：限制搜索范围
- 并行化：多线程处理

---

## 八、文档更新

每次代码修改后，应同步更新相关文档：

| 修改类型 | 需更新的文档 |
|---------|-------------|
| 新功能 | README.md, PROGRESS.md, docs/ARCHITECTURE.md |
| API变更 | docs/ARCHITECTURE.md, docs/DEVELOPMENT.md |
| Bug修复 | ROADMAP.md (记录问题和解决方案) |
| 性能优化 | PROGRESS.md (更新性能指标) |
| 测试变更 | docs/TESTING.md |

---

## 九、参考资料

### 9.1 相关论文

- [DiskANN: Fast Accurate Billion-point Nearest Neighbor Search](https://proceedings.neurips.cc/paper/2019/file/95c7dda544b41dae6f7c950e65b9c799-Paper.pdf)
- [HNSW: Efficient and robust approximate nearest neighbor search](https://arxiv.org/abs/1603.09320)
- [LSM-Tree: The Log-Structured Merge-Tree](https://www.cs.umb.edu/~poneil/lsmtree.pdf)

### 9.2 技术文档

- [io_uring 官方文档](https://kernel.dk/io_uring.pdf)
- [O_DIRECT 详解](https://man7.org/linux/man-pages/man2/open.2.html)
- [C++20 新特性](https://en.cppreference.com/w/cpp/20)

### 9.3 开源项目参考

- [DiskANN (Microsoft)](https://github.com/microsoft/DiskANN)
- [HNSWlib](https://github.com/nmslib/hnswlib)
- [Milvus](https://github.com/milvus-io/milvus)