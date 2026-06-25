#!/usr/bin/env bash

HOST=127.0.0.1
PORT=8888

# 插入数据总量：10 万条
N=100000

# QPS 测试请求数
REQ=100000

DIM=4
TOPK=1
THRESHOLD=0.90

QUERY_VECTOR="0.100000,0.200000,0.300000,0.400000"

echo "========== 1. 插入 ${N} 条 VSET 数据 =========="

START=$(date +%s%N)

{
    for i in $(seq 1 $N); do
        KEY=$(printf "q%06d" "$i")
        ANS=$(printf "a%06d" "$i")

        # 生成相似但不完全一样的 4 维向量
        # q000001 附近都围绕 0.1,0.2,0.3,0.4 做微小扰动
        V3=$(awk -v i="$i" 'BEGIN { printf "%.6f", 0.300000 + (i % 100) * 0.000100 }')
        V4=$(awk -v i="$i" 'BEGIN { printf "%.6f", 0.400000 + (i % 50) * 0.000100 }')

        echo "VSET $KEY $ANS $DIM 0.100000,0.200000,$V3,$V4"
    done
} | redis-cli -h $HOST -p $PORT --raw > /dev/null

END=$(date +%s%N)
COST_NS=$((END - START))
QPS=$(awk "BEGIN { printf \"%.2f\", $N * 1000000000 / $COST_NS }")

echo "VSET insert rows : $N"
echo "VSET insert qps  : $QPS"

echo
echo "========== 2. 精确查询 VGET 验证 =========="

redis-cli -h $HOST -p $PORT VGET q000001
redis-cli -h $HOST -p $PORT VGET q050000
redis-cli -h $HOST -p $PORT VGET q100000

echo
echo "========== 3. TOP3 向量检索 VSEARCH 验证 =========="
echo "query vector: $QUERY_VECTOR"
echo "topk=$TOPK threshold=$THRESHOLD"

redis-cli -h $HOST -p $PORT VSEARCH $DIM $QUERY_VECTOR $TOPK $THRESHOLD

echo
echo "========== 4. VSET QPS 测试 =========="

START=$(date +%s%N)

{
    for i in $(seq 1 $REQ); do
        KEY=$(printf "bench_vset_%06d" "$i")
        ANS=$(printf "bench_ans_%06d" "$i")

        V3=$(awk -v i="$i" 'BEGIN { printf "%.6f", 0.300000 + (i % 100) * 0.000100 }')
        V4=$(awk -v i="$i" 'BEGIN { printf "%.6f", 0.400000 + (i % 50) * 0.000100 }')

        echo "VSET $KEY $ANS $DIM 0.100000,0.200000,$V3,$V4"
    done
} | redis-cli -h $HOST -p $PORT --raw > /dev/null

END=$(date +%s%N)
COST_NS=$((END - START))
QPS=$(awk "BEGIN { printf \"%.2f\", $REQ * 1000000000 / $COST_NS }")

echo "VSET requests: $REQ"
echo "VSET qps     : $QPS"

echo
echo "========== 5. VGET QPS 测试 =========="

START=$(date +%s%N)

{
    for i in $(seq 1 $REQ); do
        IDX=$((i % N + 1))
        KEY=$(printf "q%06d" "$IDX")
        echo "VGET $KEY"
    done
} | redis-cli -h $HOST -p $PORT --raw > /dev/null

END=$(date +%s%N)
COST_NS=$((END - START))
QPS=$(awk "BEGIN { printf \"%.2f\", $REQ * 1000000000 / $COST_NS }")

echo "VGET requests: $REQ"
echo "VGET qps     : $QPS"

echo
echo "========== 6. VSEARCH QPS 测试 =========="

START=$(date +%s%N)

{
    for i in $(seq 1 $REQ); do
        echo "VSEARCH $DIM $QUERY_VECTOR $TOPK $THRESHOLD"
    done
} | redis-cli -h $HOST -p $PORT --raw > /dev/null

END=$(date +%s%N)
COST_NS=$((END - START))
QPS=$(awk "BEGIN { printf \"%.2f\", $REQ * 1000000000 / $COST_NS }")

echo "VSEARCH requests: $REQ"
echo "VSEARCH qps     : $QPS"

echo
echo "========== 测试完成 =========="