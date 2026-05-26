
````markdown
# AICacheDB

> 面向后端开发工程师 AI 助手的高性能本地语义缓存数据库  
> A high-performance local semantic cache database for backend-engineer-oriented AI assistants.

---

## 1. 项目简介

AICacheDB 是一个面向后端开发 AI 助手场景设计的高性能本地语义缓存数据库。系统底层基于 C 语言实现兼容 Redis RESP 协议的 KV 存储内核，上层可与 Go + gRPC 构建的 AI Chat 服务集成，用于缓存后端技术问答中的“问题、回答、语义向量”，从而减少重复调用公有大模型的 token 成本，并提升相似问题的响应速度。

AICacheDB 不只是一个普通 KV 数据库，而是围绕大模型问答系统设计的语义缓存基础设施：

- 用户提出后端开发问题；
- 系统优先查询本地语义缓存；
- 命中时直接返回历史答案，并统计节省的 token；
- 未命中时调用公有大模型生成答案；
- 将问题、回答和向量通过 `VSET` 写入 AICacheDB；
- 后续相似问题通过 `VGET` 进行本地语义命中。

典型场景：

```text
问题 1：请写一个红黑树代码
问题 2：请写一个红黑树代码（C++版本）
````

传统字符串缓存容易将两者误判为相同问题，而 AICacheDB 支持接入面向代码语义的 Embedding 模型，将“编程语言、实现约束、技术上下文”等信息编码进向量表示，从而避免错误缓存命中。

---

## 2. 核心定位

AICacheDB 的目标不是替代通用数据库，而是作为 AI 助手系统中的本地语义缓存层：

```text
AI Chat Service
      │
      │  VGET：查询本地语义缓存
      ▼
AICacheDB Slave
      │
      │  未命中
      ▼
Public LLM / OpenAI-compatible Proxy
      │
      │  生成答案
      ▼
AICacheDB Master
      │
      │  VSET：写入问题、回答、向量
      ▼
Replication
      │
      ▼
AICacheDB Slave
```

核心目标：

* 降低大模型重复调用成本；
* 提升后端技术问答响应速度；
* 支持问题级、语义级缓存命中；
* 支持主写从读的语义缓存架构；
* 提供高性能 KV 存储内核与可观测性能数据。

---

## 3. 系统架构

AICacheDB 可以独立作为 RESP 兼容 KV 数据库运行，也可以作为 AI Chat 系统的语义缓存层接入。

完整系统包含：

```text
AICacheDB Project
├── ai-chat-backend        # HTTP 接入层、前端交互、请求转发
├── ai-chat-service        # Go + gRPC 问答调度服务
├── keywords-filter        # 关键词 / 敏感词过滤服务
├── openai-api-proxy       # OpenAI-compatible API 代理
├── mock-openai-api        # 本地 mock 大模型服务
├── tokenizer              # token 估算服务
└── AICacheDB                # C 语言高性能 KV / VectorKV 存储内核
```

其中 AICacheDB 是核心存储内核，负责：

* RESP 协议解析；
* 多存储引擎；
* 向量缓存命令；
* 网络模型切换；
* RDB / AOF 持久化；
* 主从复制；
* Pipeline；
* 过期机制；
* 大 Key / 二进制安全支持；
* 内存池与性能优化。

---

## 4. 核心特性

### 4.1 AI 语义缓存能力

AICacheDB 扩展了面向 AI 问答缓存的向量命令：

| 命令                          | 说明              |
| --------------------------- | --------------- |
| `VSET`                      | 写入问题、回答和问题向量    |
| `VGET key`                  | 根据问题文本进行精确查询    |
| `VGET dim vector threshold` | 根据问题向量进行语义相似度查询 |

示例：

```bash
VSET "请写一个红黑树代码" "这里是红黑树实现..." 4 "0.12,0.33,-0.08,0.76"

VGET "请写一个红黑树代码"

VGET 4 "0.11,0.32,-0.07,0.75" 0.80
```

返回结果包括：

* 命中的历史问题；
* 对应回答；
* 语义相似度 score。

---

### 4.2 后端开发场景专用向量策略

早期版本使用自定义哈希向量验证语义缓存链路。该方案实现简单，但更偏字符级相似，难以准确区分"主题相似但约束不同"的问题。

例如：

```text
请写一个红黑树代码
请写一个红黑树代码（C++版本）
```

两者字符高度相似，但第二个问题包含明确的语言约束。如果仅依赖哈希向量，可能错误命中第一条缓存。

因此 AICacheDB 支持接入面向代码检索的开源 Embedding 模型，用于后端开发问题的语义表示。推荐用于以下技术问答场景：

* Linux 网络编程；
* C / C++ 数据结构；
* Go + gRPC 业务开发；
* MySQL / Redis / Nginx；
* KV 存储系统；
* epoll / io_uring / eBPF；
* 并发编程与系统性能优化。

推荐向量化链路：

```text
用户问题
  ↓
Code Embedding Model
  ↓
问题向量
  ↓
AICacheDB VGET
  ↓
语义缓存命中 / 公有大模型兜底
```

AICacheDB 本身专注于向量存储与检索，Embedding 模型可由上层 AI Chat 服务接入。

---

### 4.3 高性能存储内核

AICacheDB 底层存储内核基于 C 语言实现，兼容 Redis RESP 协议，可直接使用 `redis-cli` 访问。

支持的存储引擎：

| 引擎       | 数据结构 | 查询复杂度    | 适用场景      |
| -------- | ---- | -------- | --------- |
| Array    | 动态数组 | O(n)     | 小规模数据     |
| RBTree   | 红黑树  | O(log n) | 有序索引、向量索引 |
| Hash     | 哈希表  | O(1)     | 高并发精确查询   |
| SkipList | 跳表   | O(log n) | 有序集合、范围查询 |

其中 VectorKV 基于 RBTree 扩展实现：

```text
key   = 用户问题
value = answer + vector + dim + norm
```

语义检索使用：

```text
红黑树全量遍历 + 余弦相似度
```

该方案适合作为轻量级本地语义缓存，不依赖外部向量数据库。

---

## 5. 网络模型

AICacheDB 支持多种网络模型，可根据部署场景灵活切换。

| 模型        | 技术       | 特点                |
| --------- | -------- | ----------------- |
| Reactor   | epoll    | 成熟稳定，适合高并发事件驱动    |
| Proactor  | io_uring | 内核异步 I/O，降低系统调用开销 |
| Coroutine | NtyCo    | 同步编程风格，降低业务开发复杂度  |

其中 io_uring、 Reactor 与 NtyCo 模型测试可支撑百万级并发连接。

---

## 6. 持久化机制

AICacheDB 支持 RDB + AOF 混合持久化。

### 6.1 RDB 快照

RDB 用于生成数据库快照：

* fork 子进程生成快照；
* 主进程继续处理请求；
* mmap 加速 RDB 加载；
* 适合全量恢复和主从全量同步。

RDB 加载性能：

| 模式   | 100 万条数据加载耗时 |
| ---- | ------------ |
| read | 0.577s       |
| mmap | 0.199s       |

---

### 6.2 AOF 日志

AOF 用于记录写命令：

| 模式     | 说明                   |
| ------ | -------------------- |
| always | 每次写入同步 fsync，高可靠     |
| no     | 基于 io_uring 异步刷盘，高性能 |

AOF 性能对比，测试命令：

```bash
redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 200 -d 128 -t SET,GET -q
```

| 操作        |            无持久化 |   AOF(io_uring) |        AOF(fwrite) |          Redis |
| --------- | --------------: | --------------: | -----------------: | -------------: |
| SET / GET | 112422 / 120889 | 111333 / 119161 | 101122 / 115526.80 | 108049 / 99980 |
| PING      |          175642 |       161008.09 |             158647 |         169121 |

---

### 6.3 BGSAVE 性能

测试命令：

```bash
redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 100 -d 128 -t SET -q
```

| 快照频率  | BGSAVE 间隔 | 写入 QPS |
| ----- | --------: | -----: |
| 低频快照  |  每 100 万条 | 115821 |
| 中低频快照 |   每 10 万条 | 112485 |
| 中频快照  |    每 1 万条 | 109265 |
| 高频快照  |    每 1 千条 | 105898 |

---

## 7. 主从复制

AICacheDB 支持主从复制与读写分离。

典型部署方式：

```text
Master：负责 VSET / RSET 写入
Slave ：负责 VGET / RGET 读取
```

适用于 AI 助手语义缓存场景：

```text
AI Chat Service
  ↓
Slave VGET 查询缓存
  ↓
未命中
  ↓
Public LLM 生成答案
  ↓
Master VSET 写入缓存
  ↓
Replication 同步到 Slave
```

---

### 7.1 全量同步

全量同步流程：

1. Master 生成 RDB；
2. Slave 建立同步连接；
3. Master 传输 RDB；
4. Slave 加载数据；
5. 进入增量同步阶段。

文件传输方式对比：

| 模式       |        传输速率 |
| -------- | ----------: |
| sendfile | 190.86 MB/s |
| RDMA     |    134 MB/s |

---

### 7.2 增量同步

增量同步用于实时同步写命令：

* `SET`
* `DEL`
* `MOD`
* `EXPIRE`
* `VSET`
* 其他写命令

从节点保持只读，用于查询缓存。

性能影响测试：

```bash
redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 200 -d 128 -t SET -q
```

| 操作  |  关闭同步 |  开启同步 | Redis |
| --- | ----: | ----: | ----: |
| SET | 65923 | 64316 | 63019 |

---

## 8. Pipeline 性能

测试条件：

```text
1,000,000 次 SET 命令
200 并发连接
不同 Pipeline batch size
```

测试命令：

```bash
redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 200 -d 128 -t SET -q -P <batch>
```

| Batch Size | Redis QPS | AICacheDB QPS | QPS 比值 | Redis p50(ms) | AICacheDB p50(ms) |       延迟优势 |
| ---------: | --------: | ------------: | -----: | ------------: | ----------------: | ---------: |
|         10 |   821,693 |       851,789 | 1.037x |         1.727 |             1.207 | 低 0.520 ms |
|         20 | 1,053,741 |     1,658,375 | 1.574x |         3.135 |             1.255 | 低 1.880 ms |
|         40 | 1,336,898 |     2,352,941 | 1.760x |         5.295 |             2.695 | 低 2.600 ms |
|         80 | 1,414,427 |     2,369,668 | 1.675x |        10.343 |             4.543 | 低 5.800 ms |
|        160 | 1,385,042 |     2,680,965 | 1.936x |        12.887 |             9.519 | 低 3.368 ms |

---

## 9. 内存管理

AICacheDB 实现不定长内存池：

* 小对象从预分配 block 中分配；
* 大对象独立分配；
* 减少 malloc/free 调用；
* 降低内存碎片；
* 提升高并发场景下的内存稳定性。

默认 block 大小：

```text
256 MB
```

同时提供内存泄漏检测模块，用于辅助定位内存释放问题。

---

## 10. 大 Key 与二进制安全

AICacheDB 基于 RESP 长度前缀处理数据：

```text
$len
data
```

因此支持：

* 二进制数据；
* 空格；
* 换行；
* 特殊字符；
* 大 Key；
* 大 Value。

数据大小主要受可用内存限制。

---

## 11. Key 过期机制

支持 Key 过期：

* 惰性删除；
* 定期扫描；
* 过期事件同步到 AOF；
* 过期写命令同步到从节点。

支持命令：

```text
EXPIRE
TTL
SETEX
REXPIRE
RTTL
RSETEX
HEXPIRE
HTTL
HSETEX
SKEXPIRE
SKTTL
SKSETEX
```

---

## 12. 命令示例

### 12.1 启动服务

```bash
./bin/kvstore -c kvstore.conf
```

### 12.2 使用 redis-cli 连接

```bash
redis-cli -h 127.0.0.1 -p 8888
```

---

### 12.3 普通 KV 写入与读取

```bash
RSET "hello" "world"
RGET "hello"
```

---

### 12.4 向量缓存写入

```bash
VSET "请写一个红黑树代码" "这里是红黑树代码实现..." 4 "0.12,0.33,-0.08,0.76"
```

---

### 12.5 精确查询

```bash
VGET "请写一个红黑树代码"
```

---

### 12.6 语义查询

```bash
VGET 4 "0.11,0.32,-0.07,0.75" 0.80
```

---

## 13. 编译与运行

### 13.1 环境依赖

| 组件        | 要求                   |
| --------- | -------------------- |
| Linux     | >= 5.1，推荐支持 io_uring |
| GCC       | 支持 C11               |
| make      | 构建工具                 |
| NtyCo     | 已内置                  |
| redis-cli | 可选，用于 RESP 协议测试      |

---

### 13.2 获取代码

```bash
git clone https://github.com/bzanniupoiiu/AICacheDB.git --recursive
cd AICacheDB
```

---

### 13.3 编译

```bash
make clean && make
```

---

### 13.4 启动

```bash
./bin/kvstore -c kvstore.conf
```

---

### 13.5 客户端访问

```bash
redis-cli -h 127.0.0.1 -p 8888
```

---

## 14. 测试

### 14.1 存储引擎测试

```bash
./bin/kvstore -c kvstore.conf
./test/test_engine 127.0.0.1 8888
```

覆盖操作：

```text
SET / GET / MOD / EXIST / DEL / EXPIRE / TTL / SETEX
```

总操作量：

```text
360,000 次
```

通过标志：

```text
engine all tests passed!
```

---

### 14.2 持久化测试

```bash
./test/test_persistence
```

---

### 14.3 大 Key / 特殊字符测试

```bash
./test/test_bigkey_spchar
```

---

### 14.4 主从复制测试

```bash
./test/test_repl_perf
```

---

### 14.5 Pipeline 压测

```bash
redis-benchmark -h 127.0.0.1 -p 8888 -n 1000000 -c 200 -d 128 -t SET -q -P 160
```

---

## 15. 与 BackendAI-Chat 集成

AICacheDB 可作为 BackendGPT 的本地语义缓存数据库。

BackendGPT 请求流程：

```text
用户输入后端开发问题
  ↓
BackendGPT 生成问题向量
  ↓
AICacheDB Slave 执行 VGET
  ↓
命中：返回缓存答案，统计节省 token
  ↓
未命中：调用公有大模型
  ↓
AICacheDB Master 执行 VSET
  ↓
主从同步
```

Go 服务示例：

```go
masterKV := kvstore.NewKVStore("192.168.32.131:8888", "192.168.32.131:8888")
slaveKV  := kvstore.NewKVStore("192.168.32.133:8888", "192.168.32.133:8888")

// 写入：masterKV.SaveAnswer(question, answer)
// 查询：slaveKV.GetAnswerWithMeta(question)
```


---

## 16. 适用场景

AICacheDB 适用于：

* 后端开发 AI 助手；
* 大模型问答缓存；
* 本地语义缓存；
* token 成本优化；
* 高性能 KV 服务；
* RESP 协议兼容存储；
* 轻量级向量检索；
* 主写从读缓存服务；
* 系统编程与高性能网络实验平台。

---

## 17. License

MIT License

---

## 18. Version

```text
Version: v3.0
Updated: 2026-05-26
```

