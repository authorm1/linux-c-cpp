#ifndef _REACTOR_H
#define _REACTOR_H

#include<stdio.h>

typedef int(*RCALLBACK)(int);
typedef int(*protocol_handle)(char* request, int length, char* response);

#define CONN_LIST_SIZE          1024
#define EPOLL_EVENT_SIZE        1024

// 1 - 考虑 TCP 分包/粘包， 0 - 不考虑
#define TCP_PKT_SPLIT_PASTE     0

#define ENABLE_THREADPOOL       0

#if TCP_PKT_SPLIT_PASTE

#include"send_queue.h"

#define RECV_BUFFER_SIZE 1024
#define SEND_BUFFER_SIZE 1024

typedef struct node send_queue_node_t;

typedef struct queue  send_queue_t;

struct conn{
    int fd;

    char* recv_buf;
    int read_pos; // 下一次将读取到的数据追加的位置
    int capacity;
    int process_pos; // 下一次解析数据开始的位置

    send_queue_t* send_queue;

    union{
        RCALLBACK accept_cb;
        RCALLBACK recv_cb;
    }r_action;
    RCALLBACK send_cb;

};

#else

#define BUFFER_SIZE 1024

struct conn{
    int fd;

    char r_buffer[BUFFER_SIZE];
    char w_buffer[BUFFER_SIZE];
    int r_length;
    int w_length;

    union{
        RCALLBACK accept_cb;
        RCALLBACK recv_cb;
    }r_action;
    RCALLBACK send_cb;

    int status;
};

#endif

#if ENABLE_THREADPOOL

/*
* 需要有两个地方处理竞争问题，一个是任务队列，还有一个就是对存储结构的操作（暂时没处理）。
* 加互斥锁的话又会引入锁的竞争，造成新的性能问题，
* 由于暂时 KV 操作执行的很快，目前引入多线程的方式会带来线程上下文切换和锁竞争的开销，实测 QPS 相较于单线程更低了。
* 所以暂时不引入线程池了。
*/

#include"thread_pool.h"

#define THREADPOOL_WORKER_COUNT 16

typedef struct nManager reactor_threadpool_t;
typedef struct nTask reactor_task_t;
typedef struct nWorker reactor_worker_t;

int reactor_threadpool_init(reactor_threadpool_t* pool, int num_workers){
    return nThreadPoolInit(pool, num_workers);
}

int reactor_threadpool_destroy(reactor_threadpool_t* pool){
    return nThreadPoolDestroy(pool);
}

int reactor_threadpool_pushtask(reactor_threadpool_t* pool, reactor_task_t* task){
    return nThreadPoolPushTask(pool, task);
}

#endif

#endif