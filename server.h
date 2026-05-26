



#ifndef __SERVER_H__
#define __SERVER_H__

#define BUFFER_LENGTH		1024*512

#define ENABLE_HTTP			0
#define ENABLE_WEBSOCKET	0
#define ENABLE_KVSTORE		1


typedef int (*RCALLBACK)(int fd);


struct conn {
	int fd;

	char *rbuffer;          // 动态接收缓冲区
    size_t rbuf_size;       // 当前接收缓冲区大小//可扩容
    int rparse_offset;       // 已解析偏移
    int rlength;             // 已接收数据长度

    char *wbuffer;           // 动态发送缓冲区
    size_t wbuf_size;        // 当前发送缓冲区大小
    int woffset;             // 已发送偏移
    int wlength;             // 待发送数据长度

	RCALLBACK send_callback;

	union {
		RCALLBACK recv_callback;
		RCALLBACK accept_callback;
	} r_action;

	int status;
#if 1 // websocket
	char *payload;
	char mask[4];
#endif
};

#if ENABLE_HTTP
int http_request(struct conn *c);
int http_response(struct conn *c);
#endif

#if ENABLE_WEBSOCKET
int ws_request(struct conn *c);
int ws_response(struct conn *c);
#endif

#if ENABLE_KVSTORE
// int kvs_request(struct conn *c);
// int kvs_response(struct conn *c);

#endif







typedef int (*message_callback)(int fd, const char *cmd, size_t cmd_len, char **out_buf);
extern message_callback g_network_callback;

#endif


