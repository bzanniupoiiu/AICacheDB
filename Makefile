CC = gcc
Target = ./bin/kvstore

SRCS = kvstore.c kvs_command.c ntyco.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiplist.c kvs_vector.c\
       reactor.c kvs_persistence.c kvs_mempool.c kvs_repli_master.c kvs_repli_slave.c kvs_config.c kvs_expire.c

INCS = -I ./NtyCo/core/







# 根据第一步查到的实际路径修改
NTYCO_LIB = ./NtyCo/libntyco.a
LIBS = -L ./NtyCo/ -lntyco -lpthread -ldl -luring -libverbs -lrdmacm  -ljemalloc -lm

# 默认目标：先准备目录，再编译主程序
all: prepare $(Target) 

# 准备目录
prepare:
	@mkdir -p ./bin
	@mkdir -p ./Persistence/slave

# 编译 NtyCo 库（如果 libntyco.a 不存在或过期，则执行子 make）
$(NTYCO_LIB):
	$(MAKE) -C ./NtyCo


# 编译主程序及客户端、测试程序
$(Target): prepare $(NTYCO_LIB)
	$(CC) -o $(Target) $(SRCS) $(INCS) $(LIBS)
	cp -f $(Target) ./Persistence/slave/
	$(CC) -o ./kvs-client/kvstore_client ./kvs-client/kvstore_client.c

	$(CC) -o ./test/test_engine ./test/test_engine.c
	$(CC) -o ./test/test_persistence ./test/test_persistence.c 

	$(CC) -o ./test/test_mempool ./test/test_mempool.c
	$(CC) -o ./test/test_hiredis ./test/test_hiredis.c -lhiredis
	$(CC) -o ./test/test_bigkey_spchar ./test/test_bigkey_spchar.c
	$(CC) -o ./test/test_aof_rdb_replicaof_qps ./test/test_aof_rdb_replicaof_qps.c
	$(CC) -o ./test/test_expire ./test/test_expire.c
	$(CC) -o ./test/test_rdb_qps ./test/test_rdb_qps.c
	$(CC) -I. -o ./test/test_rdma_protocol ./test/test_rdma_protocol.c -libverbs -lrdmacm -lpthread
	

# 清理
clean:
	rm -rf $(Target)
	rm -rf ./kvs-client/kvstore_client
	rm -rf ./Persistence/slave/kvstore
	rm -rf ./test/test_engine ./test/test_persistence ./test/test_mempool ./test/test_mutil_cmd ./test/test_hiredis ./test/test_aof_rdb_replicaof_qps ./test/test_bigkey_spchar ./test/test_expire ./test/test_rdb_qps ./test/test_rdma_protocol
	rm -rf ./Persistence/kvstore.aof
	rm -rf ./Persistence/kvstore.rdb
	rm -rf ./Persistence/slave/Persistence/kvstore.aof
	rm -rf ./Persistence/slave/Persistence/kvstore.rdb

	$(MAKE) -C ./NtyCo clean

	
