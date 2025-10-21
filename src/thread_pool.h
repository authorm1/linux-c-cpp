#ifndef _THREAD_POOL_H
#define _THREAD_POOL_H

#include<pthread.h>
#include<stdlib.h>
#include<string.h>
#include<stdio.h>



struct nTask{ // 任务队列中的任务元素
    void(*task_func)(void* arg); // 一个函数指针，绑定到每个元素具体的任务上
    void* user_date; // 这个任务肯可能需要的用户数据
    struct nTask* prev;
    struct nTask* next;
};

typedef struct nManager nThreadPool; // 管理模块其实就是线程池

struct  nWorker{ // 执行队列中的线程
    pthread_t threadid;
    struct nManager* manager;
    int terminate; // 结束标志，如果非0代表销毁该worker
    struct nWorker* prev;
    struct nWorker* next;
};

struct nManager{ // 管理模块
     struct nTask* tasks;
    struct nTask* tasks_tail;

    struct nWorker* workers;
    struct nWorker* workers_tail; 

    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

int nThreadPoolInit(nThreadPool* pool, int numWorkers);
int nThreadPoolDestroy(nThreadPool* pool);
int nThreadPoolPushTask(nThreadPool* pool, struct nTask* task);

#endif