# 运维、恢复与升级

## 配置

复制 `config.example.conf` 为 `config.local.conf`，本地配置会被 Git 忽略。支持 bind（IPv4）、port、dir、maxmemory（字节，至少 1 MiB）、maxclients（1–1024）、timeout（空闲秒，1–86400）、appendonly（yes/no）、appendfsync（仅 always）、requirepass。

`AICACHEDB_PASSWORD` 环境变量覆盖文件中的 requirepass。环境变量如果设置为空，表示无密码。非 127.0.0.0/8 地址绑定必须配置密码。配置严格校验，不支持的设置直接报出文件名与行号；注释放在独立行，不支持行尾注释。

```sh
./bin/kvstore -c config.local.conf --check-config
./bin/kvstore -c config.local.conf
```

相对数据目录以启动工作目录为准。生产使用绝对路径。数据目录需预先存在父目录，程序以 0700 创建末级目录、以 0600 创建日志与备份。必须使用专属目录；不要允许其他账户写入目录。

默认只监听本机。远程使用 SSH 隧道。AUTH 本身不加密网络，不可直接用于公网；浏览器控制台强制本机监听，并验证启动会话 token、Host 与 Origin。

## 日志格式与写入保证

新日常版使用 `data/cache.aof`，与旧版 `Persistence/kvstore.aof` 的 RESP 日志格式不兼容。文件由 AIC1 版本帧组成：16 字节帧头（magic、payload 长度、payload CRC32、帧头 CRC32），后跟固定元数据、二进制键/值和小端 FP32 向量。持久化的是变更结果与绝对过期时间，不会在重启后重新计算 TTL。

程序先准备新记录，追加日志并 fdatasync，完成后更新内存并回复成功。磁盘错误会停止后续写入；旧数据仍可读取。设备或文件系统必须兑现同步写语义；这不保护磁盘整体损坏。appendonly=no 仅保存内存，退出后全部丢失。

恢复按帧流式进行，验证帧头和内容校验和。最后一个帧因中断而不完整时自动截到上一完整帧，并打印恢复位置；完整帧校验错误直接拒绝启动，保留原文件。发现存储损坏时先保全现场，不能靠删数据文件“修好”。

64 MiB 以上且超过有效条目内存四倍的日志，会在下一次写入前同步整理，避免更新历史无限膨胀。也可以在空闲时手动 SAVE。整理写临时文件、fsync、原子替换并同步目录；不 fork，不额外复制整份内存数据。大数据集会暂停请求，因此仍需监控日志体积和延迟。

独立锁文件阻止同一个目录被两个进程同时使用。停止进程后锁由系统自动释放，不需要删除锁文件。

## 在线备份

```sh
python3 tools/client.py BACKUP
mkdir -p backups
cp data/backup.aof backups/cache-$(date +%Y%m%d-%H%M%S).aof
```

BACKUP 在事件循环内导出一致的当前记录，原子生成独立 `backup.aof`；随后正常写入不会改动它。不要在写入过程中直接复制活动 `cache.aof`。把备份复制到另一台物理设备；同一宿主机中的另一虚拟磁盘不是独立灾备。

## 恢复演练

在**新的空目录**中恢复；不要覆盖正在使用的数据目录。

```sh
mkdir restore-check
cp backups/your-backup.aof restore-check/cache.aof
cat > restore.local.conf <<'EOF'
bind 127.0.0.1
port 8890
dir ./restore-check
maxmemory 67108864
appendonly yes
appendfsync always
EOF
./bin/kvstore -c restore.local.conf
```

另开终端用 `python3 tools/client.py --port 8890 DBSIZE`、GET 和 VSEARCH 验证业务样本。备份中的 TTL 继续按原时间过期；过期记录不会复活。恢复所需 maxmemory 必须足够，否则拒绝启动并提示增大限制。验证后再安排生产停机和切换目录。

## 与旧版本共存

默认 make 构建 `bin/kvstore` 日常版。`make experimental` 用 `Makefile.experimental` 构建 `bin/kvstore-experimental`，保留原研究代码。新版本使用 `config.example.conf`，旧版本继续使用旧 `kvstore.conf`；两者使用独立目录和端口。两种构建的 clean 均不再删除持久化数据。

旧数据不可直接拷贝到新数据目录：旧 SETEX 参数顺序、多个独立引擎、二进制字符串限制及相对 TTL 都需要显式迁移策略。当前没有自动迁移工具。已有重要数据时保留旧程序和完整备份，先通过业务导出为明确的键、类型、值、绝对过期时间和原始向量，再用新接口导入空实例并逐项核对；无法确认有效期的缓存直接回源重建。不要尝试把旧日志改名后启动新版本。

## 长期运行

`tools/aicachedb.service` 是针对用户 raoyipeng 和 `/home/raoyipeng/aicachedb-daily` 的 systemd 示例。使用前创建数据目录与 config.local.conf；其他机器必须修改用户、路径和内存限制。此文件未自动安装。

不需要管理员权限的用户服务示例为 `tools/aicachedb-user.service`。同样先核对路径，再安装：

```sh
mkdir -p ~/.config/systemd/user
cp tools/aicachedb-user.service ~/.config/systemd/user/aicachedb.service
systemctl --user daemon-reload
systemctl --user enable --now aicachedb
systemctl --user status aicachedb
```

只能在没有其他实例占用数据目录/端口时启动。用户服务通常在用户登录后启动；是否能在未登录时随机器启动取决于管理员配置的 lingering。检查日志用 `journalctl --user -u aicachedb`。

检查 INFO 的 persistence_ok、used_memory、maxmemory、aof_bytes，结合操作系统 RSS、磁盘余量和业务延迟监控。达到 maxmemory 返回 OOM，不会自动淘汰数据；主动设 TTL、删除或增大预算。读和向量查询与写入共享线程，慢查询也会影响其他请求。

SIGINT/SIGTERM 停止接收请求并退出。进行中的命令完成后才处理退出；客户端应当能处理连接结束。不能用 `make clean` 删除用户数据。

## Docker

```sh
export AICACHEDB_PASSWORD='replace-with-a-long-random-password'
docker compose up -d --build
python3 tools/client.py PING
```

默认仅发布到主机 127.0.0.1:8888；数据放在命名 volume cache-data，容器以非 root 身份运行。`docker compose down` 保留 volume；`down -v` 会删除数据，不要在日常升级时使用。默认容器内存限额 256 MiB；应用数据限额为 64 MiB，其他开销仍需监控。镜像只包含数据库，浏览器界面在本机单独运行。

升级前先备份，再构建新二进制并运行测试。先停止旧实例，再启动新实例。降级只能读取其支持的数据格式；未来格式变化时应从经过验证的兼容备份恢复。单机版本需要短暂停机，业务应支持缓存回源。
