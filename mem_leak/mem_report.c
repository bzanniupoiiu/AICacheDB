#include "mem_report.h"
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

void cleanup_mem_files(void) {
    DIR *dir = opendir("./mem_block");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof(path), "./mem_block/%s", entry->d_name);
            unlink(path);
        }
        closedir(dir);
        rmdir("./mem_block");
    }
}