#ifndef _PROACTOR_H
#define _PROACTOR_H


#define ENABLE_PRINT 0

#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2


#define ENTRIES_LENGTH		1024
#define CONNFD_LIST_SIZE 	1024

// 1 - 考虑 TCP 分包/粘包， 0 - 不考虑
#define TCP_PKT_SPLIT_PASTE		0 

typedef int(*protocol_handle)(char* request, int length, char* response);

struct conn_info {
	int fd;
	int event;
};



#if TCP_PKT_SPLIT_PASTE

#include"send_queue.h"

#define RECV_BUFFER_SIZE 1024
#define SEND_BUFFER_SIZE 1024

typedef struct node send_queue_node_t;

typedef struct queue  send_queue_t;



struct conn{
    char* recv_buf;
    int read_pos; // 下一次将读取到的数据追加的位置
    int capacity;
    int process_pos; // 下一次解析数据开始的位置

    send_queue_t* send_queue;
    int is_sending;
};

#else

#define BUFFER_LENGTH		1024

struct conn{
	char* request;
	char* response;
};

#endif

#endif