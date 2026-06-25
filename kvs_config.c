#include "kvstore.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

kvs_config_t g_config;




static void trim(char *str) {
    char *start;
    char *end;

    if (!str) {
        return;
    }

    start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (*start == '\0') {
        str[0] = '\0';
        return;
    }

    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}


int load_config(const char *filename)
{
    FILE *fp = fopen(filename,"r");
    if (!fp) return -1;

    char line[256];
    while (fgets(line,sizeof(line),fp))
    {
        trim(line);
        if(line[0]=='#'||line[0]=='\0') continue;

        // 修改这里：同时支持空格和制表符作为分隔符
        char *key = strtok(line, " \t");  // 空格和制表符都作为分隔符
        char *value = strtok(NULL, "");
        
        // 处理value前面的空格
        if (value) {
            trim(value);
        }

        if(!key || !value) {
            printf("Warning: Invalid line: '%s'\n", line);  // 调试用
            continue;
        }



        if(strcmp(key,"bind") == 0)
        {
            snprintf(g_config.bind_ip, sizeof(g_config.bind_ip), "%s", value);
        }
        else if (strcmp(key, "port") == 0) {
            // 仅支持单端口
            trim(value);
            g_config.port_count = 1;
            int port = atoi(value);
            if (port > 0 && port <= 65535) {
                g_config.ports[0] = port;
            } else {
                printf("Warning: Invalid port: '%s'\n", value);
            }
        }
        else if (strcmp(key, "appendonly") == 0) {
            g_config.appendonly = (strcmp(value, "yes") == 0 || strcmp(value, "true") == 0);
        }
        else if (strcmp(key, "appendfsync") == 0) {
            if (strcmp(value, "no") == 0) g_config.appendfsync = 0;
            // else if (strcmp(value, "everysec") == 0) g_config.appendfsync = 1;
            else if (strcmp(value, "always") == 0) g_config.appendfsync = 2;
        }
        else if (strcmp(key, "slaveof") == 0) {
            char host[64] = {0};
            int port = 0;
            if (sscanf(value, "%63s %d", host, &port) == 2 &&
                port > 0 && port <= 65535) {
                g_config.slave_mode = 1;
                snprintf(g_config.master_host, sizeof(g_config.master_host), "%s", host);
                g_config.master_port = port;
            } else {
                printf("Warning: Invalid slaveof: '%s'\n", value);
            }
        }
        
    }

    fclose(fp);
    return 0;
    
}
