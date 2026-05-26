
---

# KVStore 高性能键值存储系统

KVStore 是一个**基于 C 语言实现的高性能键值存储系统**，支持多种网络模型、存储引擎、持久化机制与主从复制，兼容 **Redis RESP 协议**，可直接使用 `redis-cli` 访问。

---

## 项目特性

* **网络模型**：EPOLL Reactor / NtyCo 协程 / io_uring
* **存储引擎**：Array / RBTree / Hash / SkipList
* **协议兼容**：Redis RESP
* **持久化**：RDB + AOF
* **功能特性**：主从复制 / Pipeline / 过期时间
* **数据支持**：大 Key / Binary Value
* **内存优化**：不定长内存池

---

## 编译与运行

### 环境依赖

| 组件    | 要求                 |
| ----- | ------------------ |
| Linux | ≥ 5.1（支持 io_uring） |
| GCC   | C11                |
| make  | 构建工具               |
| NtyCo | 已内置                |

### 获取与编译

```bash
git clone http://gitlab.0voice.com/raoyipeng24/9.1-kvstore.git --recursive
cd 9.1-kvstore
make clean && make
```

### 运行

```bash
./bin/kvstore -c kvstore.conf
```

客户端：

```bash
redis-cli -h 127.0.0.1 -p 8888
```

---

## 网络模型

| 模型       | 技术       | 特点        |
| -------- | -------- | --------- |
| Reactor  | epoll    | 成熟稳定      |
| Proactor | io_uring | 内核异步，高性能  |
| NtyCo    | 协程       | 同步编程，开发友好 |

> Reactor 与 NtyCo 实测支持 **百万级并发连接**

---

## 存储引擎

| 引擎       | 结构   | 复杂度      | 场景   |
| -------- | ---- | -------- | ---- |
| Array    | 动态数组 | O(n)     | 小数据  |
| RBTree   | 红黑树  | O(log n) | 有序   |
| Hash     | 哈希表  | O(1)     | 高并发  |
| SkipList | 跳表   | O(log n) | 有序集合 |

### 引擎测试

```bash
./bin/kvstore -c kvstore.conf
./test/test_engine 127.0.0.1 8888
```

覆盖操作：SET / GET / MOD / EXIST / DEL / EXPIRE / TTL / SETEX
总操作量：**36 万次**
输出 `engine all tests passed!` 表示通过。

---

## 持久化

KVStore 支持 **RDB + AOF 混合持久化**。

### RDB

* fork 子进程生成快照（不阻塞）
* mmap 加速加载

### AOF

* **always**：每次写入立即刷盘（高可靠）
* **no**：io_uring 异步刷盘（高性能）

### AOF性能对比（100w 数据）

测试命令 redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 200 -d 128 -t SET,GET -q

| 操作 | 无持久化 | AOF(io_uring) | AOF(fwrite) |Redis| 
|--------|---------|---------|----------|----------|
| SET/GET| 112422/120889-- | 111333/119161 |  101122/115526.80   | 108049/99980    | 
| PING   | 175642  |  161008.09 | 158647  |  169121   |


### BGSAVE 测试： redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 100 -d 128 -t SET -q

| 测试类型 | BGSAVE间隔 | 写入QPS |  
|---------|-----------|---------|
| 低频快照 | 每100万条 | 115821 |   
| 中低频快照 | 每10万条 | 112485 |   
| 中频快照 | 每1万条 | 109265 |   
| 高频快照 | 每1千条 | 105898 |   



### RDB 加载

| 模式   | 耗时         |
| ---- | ---------- |
| read | 0.577s     |
| mmap | **0.199s** |

---

## 内存池

不定长内存池：

* 小对象走 block，大对象独立分配
* 减少 malloc/free
* 降低碎片

默认 block：**256MB**

> 高并发下内存更稳定，QPS 更高（优于 malloc / jemalloc）

---

# 主从复制

支持**全量 + 增量同步**

## 全量同步（初次）

流程：

1. 主节点生成 RDB
2. 建立 RDMA 连接
3. 从节点加载数据

| 模式   | 速率         |
| ---- | ---------- |
| sendfile | 190.86 MB/s     |
| rdma  | 134 MB/s |

## 增量同步（实时）

* 写命令实时同步（SET / DEL / EXPIRE 等）
* 从节点只读

### 性能影响
redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 200 -d 128 -t SET -q

| 操作   | 关闭同步  | 开启同步  |redis|
| ---- | ----- | ----- |------|
| SET  | 59923 | 57316 | 63019|

---

## Pipeline 性能

## Pipeline 性能

测试条件：1,000,000 次 SET 命令，不同批量大小（Pipeline），使用 `redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 200 -d 128 -t SET -q -P <batch>` 测试。

| Batch Size | Redis QPS | KVStore QPS | QPS 比值 (KVStore/Redis) | Redis p50 (ms) | KVStore p50 (ms) | 延迟优势 |
|-----------|-----------|-------------|---------------------------|----------------|------------------|-----------|
| 10         | 821,693   | 851,789     | 1.037x (+3.7%)            | 1.727          | 1.207            | KVStore 低 0.520 ms |
| 20         | 1,053,741 | 1,658,375   | **1.574x (+57.4%)**       | 3.135          | 1.255            | KVStore 低 1.880 ms |
| 40         | 1,336,898 | 2,352,941   | **1.760x (+76.0%)**       | 5.295          | 2.695            | KVStore 低 2.600 ms |
| 80         | 1,414,427 | 2,369,668   | **1.675x (+67.5%)**       | 10.343         | 4.543            | KVStore 低 5.800 ms |
| 160        | 1,385,042 | 2,680,965   | **1.936x (+93.6%)**       | 12.887         | 9.519            | KVStore 低 3.368 ms |



---

# 大 Key / 二进制支持

基于 RESP 长度前缀：

```
$len
data
```

支持：

* 二进制数据
* 空格 / 换行
* 超大 Key / Value

仅受**内存限制**

---

# 过期机制

* 惰性删除 + 定期扫描
* 自动同步 AOF 与从节点

---



---

**版本**：v2.1
**更新时间**：2026-04-28

---

