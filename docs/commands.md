# 日常版命令参考

通过 RESP2 数组协议访问，命令名不区分大小写。可使用 `redis-cli` 或 `tools/client.py`。键与值支持 NUL、UTF-8、换行及空字节串。普通键值和向量共享键名，SET/VSET 会覆盖同名旧记录。

| 命令 | 行为 |
|---|---|
| `PING [message]` | 检查连接，或返回传入内容 |
| `AUTH password` | 有密码时先认证；没有用户名与 ACL |
| `SET key value [NX] [EX seconds 或 PX milliseconds]` | 保存字符串，默认清除旧 TTL；NX 仅在键不存在时写入，未写入返回 nil |
| `GET key` | 读取字符串；不存在返回 nil，向量键返回 WRONGTYPE |
| `SETEX key seconds value` / `PSETEX key milliseconds value` | 标准 Redis 参数顺序。与旧实验版的 SETEX 顺序不同 |
| `DEL key` / `VDEL key` | 删除任意类型单个键，返回 1 或 0 |
| `EXISTS key` / `EXIST key` | 存在返回 1，否则 0 |
| `TYPE key` | string、vector 或 none |
| `EXPIRE key seconds` / `PEXPIRE key milliseconds` | 设置过期时间；非正数立即删除 |
| `TTL key` / `PTTL key` | 剩余秒/毫秒；-1 永不过期，-2 不存在 |
| `PERSIST key` | 移除 TTL，变更返回 1，否则 0 |
| `VSET key answer dim comma_separated_vector [EX seconds]` | 写入答案与向量，支持有效期；拒绝零向量、NaN、Inf、维度不匹配 |
| `VGET key` | 返回 `[key, answer]`，不存在返回空数组 |
| `VGET dim vector threshold [PREFIX prefix]` | 单个最相似结果 `[key, answer, score]`，不存在返回空数组 |
| `VSEARCH dim vector topK threshold [PREFIX prefix]` | 返回多个 `[key, answer, score]`，按余弦相似度降序 |
| `SCAN cursor [COUNT count]` | 返回 `[next_cursor, keys]`，从 0 开始，下个游标为字符串 0 时结束 |
| `DBSIZE` | 当前未过期的记录数 |
| `INFO [section]` | 返回版本、数据内存、日志字节、持久化健康等；section 暂不筛选 |
| `SAVE` / `BGSAVE` | 原子整理当前日志；两个名字都**同步执行**，没有后台 fork |
| `BACKUP` | 在服务端数据目录原子生成 `backup.aof`，不修改当前日志 |
| `QUIT` | 回复后关闭连接 |

兼容辅助命令：`SELECT 0`、`CLIENT SETINFO ...`、`CLIENT SETNAME ...`。其余数据库编号、Redis Hash/List/Set、事务、Lua、Cluster、Pub/Sub、RESP3、旧引擎前缀命令以及主从命令不支持，不应宣称完整 Redis 兼容。

## 限制与一致性

- 单请求最多 1 MiB、最多 16 个参数；键最多 4096 字节，值/答案最多 512 KiB；向量最多 4096 维，topK 最多 100。
- 相似度阈值在 [-1,1]。所有向量写入时归一化为 FP32，累积相似度采用 double；同分时按键的二进制字典序排列。极近的分数可能受 FP32 舍入影响。
- 搜索为精确扫描 O(N×dimension)，适合小规模缓存。PREFIX 在评分前过滤，但它是组织机制，**不是租户权限隔离**；不同 embedding 模型或版本要采用不同前缀并显式过滤。
- 响应最多 4 MiB；返回过多大答案会报错，请减少 topK。大键批量输出不会无限增长。
- SCAN 是本项目的有限子集，不支持 Redis MATCH/TYPE；页大小为 1–100。数据变化时游标不提供快照保证，可能漏项或重复。
- 单个命令串行执行。客户端超时、断开或存储错误后，结果可能不确定；本客户端不会自动重试写入。SET/DEL 可按业务幂等重试，先查询再决定。
- maxmemory 统计条目、键、值和向量的应用字节，不包含所有分配器、连接、journal 暂存与进程基础开销，不等于 RSS 硬上限。
