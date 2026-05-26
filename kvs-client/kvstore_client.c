#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_ARGS 128
#define BUF_SIZE (1024*1024) //1 MB

// 解析命令行，支持单引号/双引号
// 解析命令行，支持单引号/双引号和转义字符


// *3\r\n$3\r\nSET\r\n$2\r\nK1\r\n$2\r\nV1\r\n

int parse_command_line(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    
    while (*p) {
        // 跳过开头的空格
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (argc >= max_args) break;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;  // 记录引号类型，跳过左引号
            char *start = p;     // 记录内容开始位置
            char *dest = p;       // 用于处理转义后的写入位置
            
            // 解析引号内的内容，处理转义
            while (*p) {
                if (*p == '\\') {//如果找到了\。
                    p++;  // 跳过反斜杠
                    if (*p == '\0') break;
                    
                    // 处理\后面的数据
                    switch (*p) {
                        case 'n': *dest++ = '\n'; break;
                        case 't': *dest++ = '\t'; break;
                        case 'r': *dest++ = '\r'; break;
                        case '\\': *dest++ = '\\'; break;
                        case '"': *dest++ = '"'; break;
                        case '\'': *dest++ = '\''; break;
                        default: 
                            *dest++ = '\\';
                            *dest++ = *p;//如果不是特殊的转义字符，就不需要替换处理
                            break;
                    }
                    p++;
                }
                else if (*p == quote) {
                    p++;  // 跳过右引号
                    break;
                }
                else {
                    *dest++ = *p++;
                }
            }
            
            *dest = '\0';  // 结束字符串
            argv[argc++] = start;
        } else {
            // 处理普通参数（不带引号）
            char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) {//如果是空格或者制表符
                *p = '\0';
                p++;
            }
            argv[argc++] = start;
        }
    }
    return argc;
}

// 从socket读取一行（以 \r\n 结尾），返回读取的字节数（包括\r\n），buf需足够大
int read_line(int sock, char *buf, int maxlen) {
    int total = 0;
    while (total < maxlen - 1) {
        int n = recv(sock, buf + total, 1, 0);
        // fprintf(stderr, "DEBUG: recv returned %d, errno=%d\n", n, errno);
        if (n <= 0) {
            return -1;
        }
        total++;
        if (total >= 2 && buf[total-2] == '\r' && buf[total-1] == '\n') {
            buf[total] = '\0';
            return total;
        }
    }
    return -1;
}

// 接收并打印 RESP 响应
void recv_resp_and_print(int sock) {
    char header[1024];
    int n = read_line(sock, header, sizeof(header));
    if (n <= 0) {
        printf("Connection closed or protocol error\n");
        exit(1);
    }
    char type = header[0];
    if (type == '+') {
        // 简单字符串：+OK\r\n
        printf("%s", header+1); // 已包含换行
    } else if (type == '-') {
        fprintf(stderr, "(error) %s", header+1);
    } else if (type == ':') {
        printf("(integer) %s", header+1);
    } else if (type == '$') {
        // 批量字符串：$<len>\r\n<data>\r\n
        int len = atoi(header+1);
        if (len == -1) {
            printf("(nil)\n");
        } else {
            // 读取数据和尾部的 \r\n
            int total = 0;
            char *data = malloc(len + 2);
            while (total < len + 2) {
                int r = recv(sock, data + total, len + 2 - total, 0);
                if (r <= 0) {
                    free(data);
                    printf("Connection closed\n");
                    exit(1);
                }
                total += r;
            }
            data[len] = '\0'; // 去掉最后的 \r\n
            printf("\"%s\"\n", data);
            free(data);
        }
    } else if (type == '*') {
        // 数组
        printf("%s", header);
    } else {
        printf("Unknown response: %s", header);
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected to %s:%d\n", host, port);

    char line[1024*1024];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;//读取一行存到line里面
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;

        char *args[MAX_ARGS];
        int argc = parse_command_line(line, args, MAX_ARGS);
        if (argc == 0) continue;

        // 构造 RESP 数组
        char *cmd_buf = malloc(BUF_SIZE);
        int pos = 0;
        pos += snprintf(cmd_buf + pos, BUF_SIZE - pos, "*%d\r\n", argc);//填入*3（tokens个数）
        for (int i = 0; i < argc; i++) {
            pos += snprintf(cmd_buf + pos, BUF_SIZE - pos, "$%zu\r\n%s\r\n", strlen(args[i]), args[i]);//填入$+字符串长度+字符串
        }
        // 发送
        if (send(sock, cmd_buf, pos, 0) < 0) {
            perror("send");
            break;
        }
        // 接收响应
        recv_resp_and_print(sock);
        free(cmd_buf);
    }
    close(sock);
    return 0;
}


/*
同时发送的脚本
#!/bin/bash

# 服务器地址和端口
HOST="127.0.0.1"
PORT="8888"

# 循环生成 RESP 命令并通过管道发送
{
    for i in {0..1000}; do
        key="K$i"
        value="V$i"
        # 构造 RESP 数组：*3\r\n$3\r\nSET\r\n$key_len\r\nkey\r\n$value_len\r\nvalue\r\n
        printf "*3\r\n\$3\r\nSET\r\n\$%d\r\n%s\r\n\$%d\r\n%s\r\n" \
               "${#key}" "$key" "${#value}" "$value"
    done
} | nc "$HOST" "$PORT"
 
*/