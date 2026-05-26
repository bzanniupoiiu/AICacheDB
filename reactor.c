


#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#include <time.h>

#include "server.h"

#include <stdlib.h>
#include "kvstore.h"

#define CONNECTION_SIZE			1024  // 1024 * 1024


extern int resp_parse_one_command(const char *buf, size_t len, size_t *cmd_len);//判断是不是一条resp命令


int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

int epfd = 0;

struct conn conn_list[CONNECTION_SIZE] = {0};


int set_event(int fd, int event, int flag) {

	if (flag) {  // non-zero add

		struct epoll_event ev;
		ev.events = event;
		ev.data.fd = fd;
		epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

	} else {  // zero mod

		struct epoll_event ev;
		ev.events = event;
		ev.data.fd = fd;
		epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
		
	}
	

}


int event_register(int fd, int event) {
    if (fd < 0) return -1;
    struct conn *c = &conn_list[fd];
    c->fd = fd;
    c->r_action.recv_callback = recv_cb;
    c->send_callback = send_cb;

    c->rbuffer = (char*)kvs_malloc(4*1024*1024);
    c->rbuf_size = 4*1024*1024;
    c->rlength = 0;
    c->rparse_offset = 0;

    c->wbuffer = (char*)kvs_malloc(4*1024*1024);
    c->wbuf_size = 4*1024*1024;
    c->wlength = 0;
    c->woffset = 0;

    set_event(fd, event, 1);
    return 0;
}


// listenfd(sockfd) --> EPOLLIN --> accept_cb
int accept_cb(int fd) {

	struct sockaddr_in  clientaddr;
	socklen_t len = sizeof(clientaddr);

	int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
	if (clientfd < 0) {
		printf("accept errno: %d --> %s\n", errno, strerror(errno));
		return -1;
	}
	event_register(clientfd, EPOLLIN);  // | EPOLLET

	return 0;
}

// 修改 send_cb 函数
int send_cb(int fd) {
    struct conn *c = &conn_list[fd];
    
    if (c->wlength == 0) {
        return 0;
    }

    int n = send(fd, c->wbuffer + c->woffset, c->wlength - c->woffset, 0);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            set_event(fd, EPOLLIN | EPOLLOUT, 0);  // 关键修复：确保写事件注册
            return 0;
        }
        perror("send");
        close(fd);
        return -1;
    }
    
    c->woffset += n;
    if (c->woffset >= c->wlength) {
        // 发送完毕
        c->wlength = 0;
        c->woffset = 0;
        set_event(fd, EPOLLIN, 0);   // 移除 EPOLLOUT
    } else {
        // 部分发送，必须确保 EPOLLOUT 仍然注册
        set_event(fd, EPOLLIN | EPOLLOUT, 0);
    }
    return n;
}

// 修改 recv_cb，在末尾立即发送并添加调试
int recv_cb(int fd) {
    struct conn *c = &conn_list[fd];

    // 确保接收缓冲区有空间（原代码不变）
    if (c->rlength >= c->rbuf_size - 1) {
        printf("kuorong!!~\n");
        size_t new_size = c->rbuf_size * 2;
        char *new_buf = (char*)kvs_malloc(new_size);
        if (!new_buf) { close(fd); return -1; }
        // 拷贝旧数据
        memcpy(new_buf, c->rbuffer, c->rlength);
        // 释放旧缓冲区
        kvs_free(c->rbuffer);
        c->rbuffer = new_buf;
        c->rbuf_size = new_size;
    }

    int n = recv(fd, c->rbuffer + c->rlength, c->rbuf_size - c->rlength - 1, 0);
    if (n <= 0) {
        // if (n == 0) printf("[recv_cb] connection closed\n");
        kvs_free(c->rbuffer); kvs_free(c->wbuffer);
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return -1;
    }
    c->rlength += n;
    c->rbuffer[c->rlength] = '\0';

    // 解析命令循环（原代码不变）
    while (1) {
        int remaining = c->rlength - c->rparse_offset;
        if (remaining <= 0) break;
        size_t cmd_len;
        int ret = resp_parse_one_command(c->rbuffer + c->rparse_offset, remaining, &cmd_len);
        if (ret == 1) {
            char *resp_buf = NULL;
            int resp_len = g_network_callback(fd, c->rbuffer + c->rparse_offset, cmd_len, &resp_buf);
            if (resp_len > 0 && resp_buf) {
                // 追加到发送缓冲区
                if (c->wlength + resp_len > c->wbuf_size) {
                    size_t new_size = c->wbuf_size * 2;
                    if (new_size < c->wlength + resp_len) new_size = c->wlength + resp_len;
                    char *new_wbuf = (char*)kvs_malloc(new_size);
                    if (!new_wbuf) { kvs_free(resp_buf); close(fd); return -1; }
                    // 拷贝旧数据
                    memcpy(new_wbuf, c->wbuffer, c->wlength);
                    // 释放旧缓冲区
                    kvs_free(c->wbuffer);
                    c->wbuffer = new_wbuf;
                    c->wbuf_size = new_size;
                }
                memcpy(c->wbuffer + c->wlength, resp_buf, resp_len);
                c->wlength += resp_len;
                kvs_free(resp_buf);
            } else if (resp_len < 0) {
                close(fd); return -1;
            }
            c->rparse_offset += cmd_len;
        } else if (ret == 0) {
            break;
        } else {
            return -1;
        }
    }

    // 移动未解析数据
    if (c->rparse_offset > 0) {
        int left = c->rlength - c->rparse_offset;
        if (left > 0) memmove(c->rbuffer, c->rbuffer + c->rparse_offset, left);
        c->rlength = left;
        c->rparse_offset = 0;
    }

    // 立即尝试发送响应（关键修复：无论是否已有 EPOLLOUT，都尝试发送）
    if (c->wlength > 0) {
        send_cb(fd);
    }
    return n;
}

int r_init_server(unsigned short port) {

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    // ˿ڸ
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (-1 == bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr))) {
        printf("bind failed: %s\n", strerror(errno));
    }

    listen(sockfd, 128);

    return sockfd;
}



int reactor_start(unsigned short port) {
    epfd = epoll_create(1);
    int sockfd = r_init_server(port);
    if (sockfd < 0) return -1;
    conn_list[sockfd].fd = sockfd;
    conn_list[sockfd].r_action.recv_callback = accept_cb;
    set_event(sockfd, EPOLLIN, 1);


   

    
    while (1) {
        struct epoll_event events[1024] = {0};
        int nready = epoll_wait(epfd, events, 1024, -1);

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;
            if (events[i].events & EPOLLIN) {
                conn_list[fd].r_action.recv_callback(fd);
            }
            if (events[i].events & EPOLLOUT) {
                conn_list[fd].send_callback(fd);
            }
            
        }
    }
}