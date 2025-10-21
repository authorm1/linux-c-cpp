
// ---------------------------线程池SDK (FIFO版本)----------------------------------


#include"thread_pool.h"

#define FIFO_INSERT(head, tail, item)     \
do{                                     \
    item->prev = tail;                  \
    item->next = NULL;                  \
    if (tail) {                         \
        tail->next = item;              \
    } else {                            \
        head = item;                    \
    }                                   \
    tail = item;                        \
} while(0)


#define FIFO_REMOVE(head, tail, item)   \
do{                                     \
    if (item->prev) {                   \
        item->prev->next = item->next;  \
    }                                   \
    if (item->next) {                   \
        item->next->prev = item->prev;  \
    }                                   \
    if (head == item) {                 \
        head = item->next;              \
    }                                   \
    if (tail == item) {                 \
        tail = item->prev;              \
    }                                   \
    item->prev = item->next = NULL;     \
} while(0)

#if 0
// -----------------------------线程池的属性---------------------------------------

struct nTask{ // 任务队列中的任务元素
    void(*task_func)(void* arg); // 一个函数指针，绑定到每个元素具体的任务上
    void* user_date; // 这个任务肯可能需要的用户数据
    struct nTask* prev;
    struct nTask* next;
};

typedef struct nManager nThreadPool; // 管理模块其实就是线程池

struct nWorker{ // 执行队列中的线程
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

// -------------------------------end---------------------------
#endif

// -----------------------线程池提供的方法---------------------------

static void* nThreadPoolWorkerCallBack(void* arg){
    struct nWorker* worker = (struct nWorker*)(arg);

    while(1){
        pthread_mutex_lock(&worker->manager->mutex);

        while(worker->manager->tasks == NULL){
            if(worker->terminate) break;
            pthread_cond_wait(&worker->manager->cond, &worker->manager->mutex);
        }
        if(worker->terminate){
            pthread_mutex_unlock(&worker->manager->mutex);
            break;
        }

      
        struct nTask* task = worker->manager->tasks;
        
        FIFO_REMOVE(worker->manager->tasks, worker->manager->tasks_tail, task); // 去除队首
    
        pthread_mutex_unlock(&worker->manager->mutex);

        task->task_func(task);
    }

    // free(worker); 
    return NULL; 
} 

int nThreadPoolInit(nThreadPool* pool, int numWorkers){
    if(pool == NULL) return -1;
    if(numWorkers < 1) return -2;

    memset(pool, 0, sizeof(nThreadPool)); 
    
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    
    // 初始化头尾指针为NULL
    pool->tasks = NULL;
    pool->tasks_tail = NULL;
    pool->workers = NULL;
    pool->workers_tail = NULL;

    for(int i = 0; i < numWorkers; ++i){
        struct nWorker* worker = (struct nWorker*)malloc(sizeof(struct nWorker));
        if(worker == NULL){
            perror("malloc nWorker");
            nThreadPoolDestroy(pool); 
            return -3;
        }
        memset(worker, 0, sizeof(struct nWorker));
        
        worker->manager = pool;
        int ret = pthread_create(&worker->threadid, NULL, nThreadPoolWorkerCallBack, worker);
        if(ret != 0){
            perror("pthread_create");
            free(worker);
            nThreadPoolDestroy(pool);
            return -4;
        }

        
        FIFO_INSERT(pool->workers, pool->workers_tail, worker);
    }

    return 0; // success
}

int nThreadPoolDestroy(nThreadPool* pool){
    if(pool == NULL) return -1;

    // 加锁以安全地修改terminate标志
    pthread_mutex_lock(&pool->mutex);

    // 设置所有worker的终止标志
    for(struct nWorker* worker = pool->workers; worker != NULL; worker = worker->next){
        worker->terminate = 1;
    }
    
    // 唤醒所有可能在等待任务的线程
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    
    // 等待所有线程执行完毕
    for(struct nWorker* worker = pool->workers; worker != NULL; worker = worker->next){
        pthread_join(worker->threadid, NULL);
    }

    // 释放所有worker节点的内存
    struct nWorker* curr_worker = pool->workers;
    while (curr_worker != NULL) {
        struct nWorker* next_worker = curr_worker->next;
        free(curr_worker);
        curr_worker = next_worker;
    }
    pool->workers = pool->workers_tail = NULL;

    // 线程池本身不负责task节点的内存管理，应由调用者处理。
    pool->tasks = pool->tasks_tail = NULL;
    
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);

    return 0;
}

int nThreadPoolPushTask(nThreadPool* pool, struct nTask* task){
    if(pool == NULL || task == NULL) return -1; 

    pthread_mutex_lock(&pool->mutex);
    

    FIFO_INSERT(pool->tasks, pool->tasks_tail, task);
    
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}