#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis/hiredis.h>

int main() {
    // 1. 连接 Redis
    redisContext *c = redisConnect("127.0.0.1", 8888);
    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            printf("Connection allocation failed\n");
        }
        return 1;
    }

    printf("Connected to Redis successfully!\n");

    // 2. 测试 SET
    redisReply *reply = redisCommand(c, "RSET test_key %s", "hello");
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);

    // 3. 测试 GET
    reply = redisCommand(c, "RGET test_key");
    if (reply->type == REDIS_REPLY_STRING) {
        printf("GET: %s\n", reply->str);
    }
    freeReplyObject(reply);

    // 4. 测试 HSET
    reply = redisCommand(c, "HSET myhash value1");
   

    // 5. 测试 HGET
    reply = redisCommand(c, "HGET myhash");
   
    freeReplyObject(reply);

    // 6. 测试 HEXISTS
    reply = redisCommand(c, "HEXIST myhash");
    printf("HEXIST: %lld\n", reply->integer);
    freeReplyObject(reply);

    // 7. 测试 HDEL
    reply = redisCommand(c, "HDEL myhash");
    printf("HDEL: %lld\n", reply->integer);
    freeReplyObject(reply);

    // 8. 清理
    redisFree(c);

    printf("Test finished successfully!\n");
    return 0;
}