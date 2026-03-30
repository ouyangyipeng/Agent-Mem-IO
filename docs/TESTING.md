# Agent-Mem-IO 测试指南

> 本文档介绍项目的测试策略、测试方法和本地测试流程。

---

## 一、测试概述

### 1.1 测试目标

- **召回率验证**: 确保 Recall@10 ≥ 85%
- **内存限制验证**: 确保内存占用在数据集大小的 10%-20%
- **功能正确性**: 确保所有核心功能正常工作
- **性能基准**: 建立 QPS 和延迟基准

### 1.2 测试类型

| 测试类型 | 文件位置 | 说明 |
|---------|---------|------|
| 单元测试 | `tests/test_main.cpp` | 测试各组件功能 |
| 基准测试 | `src/benchmark.cpp` | 测试性能指标 |
| 内存测试 | 手动验证 | 验证内存限制 |

---

## 二、本地测试环境搭建

### 2.1 环境要求

```bash
# 操作系统
Ubuntu 22.04+ (推荐)
# 或其他 Linux 发行版（需要支持 io_uring）

# 编译器
GCC 11+ 或 Clang 14+
# 需要 C++20 支持

# 依赖工具
CMake 3.16+
Make
# 可选: liburing-dev (io_uring 支持)
```

### 2.2 安装依赖

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y build-essential cmake git
sudo apt install -y liburing-dev  # 可选，用于 io_uring 支持

# 验证编译器版本
g++ --version  # 需要 >= 11.0
cmake --version  # 需要 >= 3.16
```

### 2.3 构建项目

```bash
# 克隆项目
git clone https://github.com/ouyangyipeng/Agent-Mem-IO.git
cd Agent-Mem-IO

# 创建构建目录
mkdir -p build && cd build

# 配置 CMake
cmake ..

# 编译（使用所有核心）
make -j$(nproc)

# 编译完成后，会生成以下可执行文件：
# - agent_mem_io_tests    # 单元测试
# - agent_mem_io_benchmark # 基准测试
# - agent_mem_io_cli      # 命令行工具
```

---

## 三、运行测试

### 3.1 单元测试

```bash
cd build

# 运行所有单元测试
./agent_mem_io_tests

# 预期输出示例：
# [TEST] Testing distance functions... PASSED
# [TEST] Testing graph navigation data... PASSED
# [TEST] Testing graph index... PASSED
# [TEST] Testing buffer pool... PASSED
# [TEST] Testing memtable... PASSED
# [TEST] Testing LSM write manager... PASSED
# [TEST] Testing storage engine... PASSED
# [TEST] Testing types and constants... PASSED
# [TEST] Testing error handling... PASSED
# 
# All tests passed!
```

### 3.2 基准测试

```bash
cd build

# 基本用法
./agent_mem_io_benchmark

# 自定义参数
./agent_mem_io_benchmark -n <向量数量> -d <维度> -q <查询数量> -k <Top-K>

# 示例：10万向量，128维，100次查询，Top-10
./agent_mem_io_benchmark -n 100000 -d 128 -q 100 -k 10

# 示例输出：
# === Agent-Mem-IO Benchmark ===
# Configuration:
#   Vectors: 100000
#   Dimensions: 128
#   Queries: 100
#   K: 10
#   Max Degree: 16
#   EF Search: 250
# 
# Building index...
# Index built in 2345 ms
# 
# Running search...
# Search completed in 302 ms
# 
# Results:
#   QPS: 331.13
#   Avg Latency: 3.02 ms
#   Recall@10: 86.60%
#   Memory Usage: 125.00 MB
```

### 3.3 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-n` | 向量数量 | 10000 |
| `-d` | 向量维度 | 128 |
| `-q` | 查询数量 | 100 |
| `-k` | Top-K 值 | 10 |
| `--ef-search` | 搜索时的候选列表大小 | 250 |
| `--max-degree` | 图索引最大度数 | 16 |

---

## 四、测试数据

### 4.1 合成数据

项目内置合成数据生成器，用于在没有 SIFT1M 数据集时进行测试：

```cpp
// 位于 src/data/synthetic_data.h
// 生成随机向量数据
std::vector<Vector> generate_synthetic_data(Size n, Dim d);
```

### 4.2 SIFT1M 数据集

如需使用真实数据集，可下载 SIFT1M：

```bash
# 创建数据目录
mkdir -p data/sift1m

# 下载 SIFT1M 数据集（如果可用）
# 注意：原始数据源可能不可用，可使用合成数据替代

# 数据集文件：
# - sift_base.fvecs: 1,000,000 个基准向量
# - sift_query.fvecs: 10,000 个查询向量
# - sift_groundtruth.ivecs: ground truth 结果
```

### 4.3 数据格式

SIFT 数据集使用 `.fvecs` 格式：

```
[维度: 4字节][向量数据: 维度 * 4字节]
[维度: 4字节][向量数据: 维度 * 4字节]
...
```

---

## 五、性能指标验证

### 5.1 召回率验证

召回率是最重要的指标，必须 ≥ 85%：

```bash
# 运行基准测试并检查召回率
./agent_mem_io_benchmark -n 100000 -q 100 -k 10

# 检查输出中的 Recall@10
# Recall@10: 86.60%  ✅ 合格
# Recall@10: 84.00%  ❌ 不合格
```

### 5.2 内存限制验证

```bash
# 方法1：使用 /usr/bin/time
/usr/bin/time -v ./agent_mem_io_benchmark -n 100000

# 查看输出中的 "Maximum resident set size"
# 对于 100K 向量（约 50MB），内存应限制在 5-10MB

# 方法2：使用 valgrind massif
valgrind --tool=massif ./agent_mem_io_benchmark -n 10000
ms_print massif.out.*

# 方法3：使用 cgroups 限制内存
sudo cgcreate -g memory:/agent_mem_io
sudo cgset agent_mem_io memory.limit_in_bytes 100M
sudo cgexec -g memory:agent_mem_io ./agent_mem_io_benchmark
```

### 5.3 QPS 和延迟验证

```bash
# 运行基准测试
./agent_mem_io_benchmark -n 100000 -q 1000

# 关注以下指标：
# - QPS: 每秒查询数（越高越好）
# - Avg Latency: 平均延迟（越低越好）
# - P99 Latency: 99分位延迟（越低越好）
```

---

## 六、回归测试清单

每次代码修改后，应运行以下测试：

### 6.1 快速测试（开发时）

```bash
cd build
make -j$(nproc) && ./agent_mem_io_tests
```

### 6.2 完整测试（提交前）

```bash
cd build

# 1. 清理并重新构建
make clean
cmake ..
make -j$(nproc)

# 2. 运行单元测试
./agent_mem_io_tests

# 3. 运行基准测试（小规模）
./agent_mem_io_benchmark -n 10000 -q 100

# 4. 运行基准测试（中等规模）
./agent_mem_io_benchmark -n 100000 -q 100

# 5. 检查召回率
# 确保输出 Recall@10 >= 85%
```

### 6.3 压力测试（可选）

```bash
# 大规模测试
./agent_mem_io_benchmark -n 1000000 -q 1000

# 长时间运行测试
for i in {1..10}; do
    echo "Run $i"
    ./agent_mem_io_benchmark -n 100000 -q 100
done
```

---

## 七、常见问题排查

### 7.1 编译错误

**问题**: `error: 'io_uring' was not declared`

**解决**: 安装 liburing 开发包
```bash
sudo apt install liburing-dev
```

**问题**: `error: C++20 feature required`

**解决**: 升级 GCC 到 11+
```bash
sudo apt install gcc-11 g++-11
export CC=gcc-11
export CXX=g++-11
```

### 7.2 运行时错误

**问题**: 召回率过低（< 80%）

**排查步骤**:
1. 检查图索引参数（max_degree, ef_search）
2. 增大 ef_search 值（如从 200 增加到 250）
3. 检查距离计算是否正确

**问题**: 内存占用过高

**排查步骤**:
1. 检查 Buffer Pool 大小配置
2. 减少缓存页面数量
3. 检查是否有内存泄漏

### 7.3 性能问题

**问题**: QPS 过低

**排查步骤**:
1. 确认是否启用了 io_uring
2. 检查是否使用了 O_DIRECT
3. 分析 I/O 等待时间

---

## 八、测试报告模板

```markdown
## Agent-Mem-IO 测试报告

**测试日期**: YYYY-MM-DD
**测试环境**: 
- OS: Ubuntu 22.04
- CPU: [CPU型号]
- RAM: [内存大小]
- SSD: [SSD型号]

### 测试配置
- 向量数量: 100,000
- 向量维度: 128
- 查询数量: 100
- Top-K: 10

### 测试结果

| 指标 | 值 | 目标 | 状态 |
|------|-----|------|------|
| Recall@10 | 86.60% | ≥85% | ✅ |
| QPS | 331 | - | - |
| 平均延迟 | 3.02ms | - | - |
| 内存占用 | 125MB | 50-100MB | ⚠️ |

### 问题记录
1. [描述发现的问题]
2. [描述优化建议]

### 结论
[测试结论]
```

---

## 九、持续集成

项目可配置 GitHub Actions 进行自动化测试：

```yaml
# .github/workflows/test.yml
name: Test

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v3
      
      - name: Install dependencies
        run: |
          sudo apt update
          sudo apt install -y build-essential cmake liburing-dev
      
      - name: Build
        run: |
          mkdir build && cd build
          cmake ..
          make -j$(nproc)
      
      - name: Run tests
        run: |
          cd build
          ./agent_mem_io_tests
          ./agent_mem_io_benchmark -n 10000 -q 100
```

---

## 十、参考资料

- [SIFT1M 数据集](http://corpus-texmex.irisa.fr/)
- [DiskANN 论文](https://proceedings.neurips.cc/paper/2019/file/95c7dda544b41dae6f7c950e65b9c799-Paper.pdf)
- [io_uring 文档](https://kernel.dk/io_uring.pdf)