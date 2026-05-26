#include "nty_coroutine.h"
#include "server.h"
#include <arpa/inet.h>
#include <stdlib.h>

#include "kvstore.h"
extern message_callback g_network_callback;

extern int resp_parse_one_command(const char *buf, size_t len, size_t *cmd_len) ;

void server_reader(void *arg) {
    int fd = (int)(long)arg;
    // 设置为非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    char *rbuf = (char*)malloc(4096);
    size_t rbuf_size = 4096;
    int rlen = 0;
    int parse_offset = 0;

    while (1) {
        // 确保接收缓冲区有空间
        if (rlen >= rbuf_size - 1) {
            size_t new_size = rbuf_size * 2;
            char *new_rbuf = (char*)realloc(rbuf, new_size);
            if (!new_rbuf) break;
            rbuf = new_rbuf;
            rbuf_size = new_size;
        }
        int n = recv(fd, rbuf + rlen, rbuf_size - rlen - 1, 0);
        if (n <= 0) {
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                break;
            }
            // EAGAIN，让出 CPU
            nty_coroutine_yield(nty_coroutine_get_sched()->curr_thread);
            continue;
        }
        rlen += n;
        rbuf[rlen] = '\0';

        while (1) {
            int remaining = rlen - parse_offset;
            if (remaining <= 0) break;
            size_t cmd_len;
            int ret = resp_parse_one_command(rbuf + parse_offset, remaining, &cmd_len);
            if (ret == 1) {
                char *resp_buf = NULL;
                int resp_len = g_network_callback(fd, rbuf + parse_offset, cmd_len, &resp_buf);
                if (resp_len > 0 && resp_buf) {
                    // 循环发送
                    int sent = 0;
                    while (sent < resp_len) {
                        int ns = send(fd, resp_buf + sent, resp_len - sent, 0);
                        if (ns <= 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                nty_coroutine_yield(nty_coroutine_get_sched()->curr_thread);
                                continue;
                            }
                            // 发送错误
                            kvs_free(resp_buf);
                            goto out;
                        }
                        sent += ns;
                    }
                    kvs_free(resp_buf);
                } else if (resp_len < 0) {
                    goto out;
                }
                parse_offset += cmd_len;
            } else if (ret == 0) {
                break;
            } else {
                goto out;
            }
        }

        if (parse_offset > 0) {
            int left = rlen - parse_offset;
            if (left > 0) {
                memmove(rbuf, rbuf + parse_offset, left);
            }
            rlen = left;
            parse_offset = 0;
        }
    }

out:
    free(rbuf);
    close(fd);
}

void server(void *arg) {

	unsigned short port = *(unsigned short *)arg;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return ;

	struct sockaddr_in local, remote;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in));

	listen(fd, 20);
	printf("listen port : %d\n", port);


	while (1) {
		socklen_t len = sizeof(struct sockaddr_in);
		int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
		

		nty_coroutine *read_co;
		nty_coroutine_create(&read_co, server_reader, (void*)(long)cli_fd);

	}
	
}





int ntyco_start(unsigned short port) {

	//int port = atoi(argv[1]);
	// kvs_handler = handler;

	
	nty_coroutine *co = NULL;
	nty_coroutine_create(&co, server, &port);

	nty_schedule_run();

}




