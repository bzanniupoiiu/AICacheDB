#include "kvstore.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

kvs_config_t g_config;




static void trim(char *str) {
    char *start = str;
    while (isspace(*start)) start++;
    char *end = start + strlen(start) - 1;
    while (end > start && isspace(*end)) *end-- = '\0';
    if (start != str) memmove(str, start, strlen(start) + 1);
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
            strncpy(g_config.bind_ip, value, sizeof(g_config.bind_ip)-1);
        }
        else if (strcmp(key, "port") == 0) {
            // 仅支持单端口
            trim(value);
            g_config.port_count = 1;
            g_config.ports[0] = atoi(value);
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
            char host[64];
            int port;
            if (sscanf(value, "%s %d", host, &port) == 2) {
                g_config.slave_mode = 1;
                strcpy(g_config.master_host, host);
                g_config.master_port = port;
            }
        }
        
    }

    fclose(fp);
    return 0;
    
}