/*
* 单reactor + 多线程
* I/O线程 -> 只负责 I/O 事件处理（包括对 epoll 的控制）
* worker线程 -> 只负责业务逻辑处理
*/

/*
* 1. 解决了上版程序中并发量比较大时，如果业务处理的很快(例如echo)，发送缓冲区堆积导致send返回值<0直接close的问题
    -> 解决方法：send_callback支持分段发送，发送缓冲区满时保持EPOLLOUT事件，下次可写时继续发送剩余数据
*/

#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/epoll.h>
#include<sys/eventfd.h> // eventfd

#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<pthread.h>
#include<errno.h>
#include<time.h>

#include"thread_pool.h"

#define BUFFER_SIZE                     1024
#define CONN_LIST_SIZE                  1024 * 128
#define EPOLL_EVENT_SIZE                1024
#define WORKER_THREAD_NUM               8
#define TASK_QUEUE_SIZE                 1024 * 64
#define MAX_RESPONSE_SIZE               1024     

#define ENABLE_SERVICE_SIMULATION       0 // 模拟业务处理逻辑 -> 0-10ms

typedef int(*RCALLBACK)(int);

#define XIUGAI 1

typedef struct {
    int fd;
#if XIUGAI

#else
    char* w_buffer; 
    long w_length;
#endif
} result_t;

typedef struct {
    result_t* results; // ringbuf
    int head;
    int tail;
    int count;
    int capacity;
    pthread_mutex_t lock;
} result_queue_t;

struct conn{
    int fd;

    char r_buffer[BUFFER_SIZE];
    char* w_buffer;
    long r_length;
    long w_length;

    union{
        RCALLBACK accept_cb;
        RCALLBACK recv_cb;
    }r_action;
    RCALLBACK send_cb;
/*
* 防止还有线程正在用这个 fd 时，其他线程因为某些原因将 fd close 了.
* 比如 I/O 线程收到了断连请求，将 fd closeed，，但任务队列中还有此 fd 的任务，内核将此 fd 回收，并对后面的新连接分配此 fd，
* worker 线程取走旧 fd 的任务并完成，然后 I/O 线程对旧 fd 执行 send 任务，但此时数据发到新 fd 那里去了。
*/
    int send_offset; // 发送偏移，用于分段发送
    int ref_count;
    pthread_mutex_t lock;
};

static int g_eventfd = 0;
static nThreadPool g_pool;
static result_queue_t g_result_queue;


int accept_callback(int fd);
int recv_callback(int fd);
int send_callback(int fd);
int set_event(int fd, int event,int flag);

int epollfd = 0;
struct conn conn_list[CONN_LIST_SIZE] = {0};

// =============================================================================

void conn_inc_ref(struct conn* conn_state){
    pthread_mutex_lock(&conn_state->lock);
    ++conn_state->ref_count;
    pthread_mutex_unlock(&conn_state->lock);
}

void conn_dec_ref(struct conn* conn_state){
    int destroy_lock = 0;
    pthread_mutex_lock(&conn_state->lock);
    --conn_state->ref_count;
    if(conn_state->ref_count == 0){
        if(conn_state->w_buffer){
            free(conn_state->w_buffer);
            conn_state->w_buffer = NULL;
        }
        close(conn_state->fd);
        destroy_lock = 1;
    }
    pthread_mutex_unlock(&conn_state->lock);

    if (destroy_lock) {
        pthread_mutex_destroy(&conn_state->lock);
    }
}

void result_queue_init() {
    g_result_queue.results = (result_t*)malloc(sizeof(result_t) * TASK_QUEUE_SIZE);
    if(g_result_queue.results == NULL){
        printf("malloc results queue failed.\n");
        exit(1);
    }
    g_result_queue.head = 0;
    g_result_queue.tail = 0;
    g_result_queue.count = 0;
    g_result_queue.capacity = TASK_QUEUE_SIZE;
    pthread_mutex_init(&g_result_queue.lock, NULL);
}
#if XIUGAI
void submit_result_to_io_thread(int fd){
#else
void submit_result_to_io_thread(int fd, char* resp_buf, int len){

#endif
    pthread_mutex_lock(&g_result_queue.lock);

    if (g_result_queue.count == g_result_queue.capacity) {
        int new_capacity = g_result_queue.capacity * 2;
        printf("Result queue is full. Reallocating from %d to %d\n", g_result_queue.capacity, new_capacity);

        result_t* new_results = (result_t*)malloc(new_capacity * sizeof(result_t));
        if (new_results == NULL) {
            printf("malloc for new result queue failed.\n");
            exit(1);
        }

        int head_to_end_count = g_result_queue.capacity - g_result_queue.head;
        memcpy(new_results, g_result_queue.results + g_result_queue.head, head_to_end_count * sizeof(result_t));
        memcpy(new_results + head_to_end_count, g_result_queue.results, g_result_queue.tail * sizeof(result_t));
       
        free(g_result_queue.results);
        g_result_queue.results = new_results;
        g_result_queue.capacity = new_capacity;
        g_result_queue.head = 0; 
        g_result_queue.tail = g_result_queue.count;
    }
    
    int tail = g_result_queue.tail;
    g_result_queue.results[tail].fd = fd;
#if XIUGAI

#else
    g_result_queue.results[tail].w_buffer = resp_buf;
    g_result_queue.results[tail].w_length = len;
#endif
    g_result_queue.tail = (tail + 1) % g_result_queue.capacity; // ring
    g_result_queue.count++;

    pthread_mutex_unlock(&g_result_queue.lock);

    uint64_t val = 1;
    eventfd_write(g_eventfd, val); // 向 g_eventfd 写值唤醒 epollwait

}

void handle_pending_results() {
    pthread_mutex_lock(&g_result_queue.lock);
    int count = g_result_queue.count;
#if XIUGAI
    // 快照
    result_t* results_snapshot = malloc(sizeof(result_t) * count);
    if(results_snapshot == NULL){
        printf("malloc results_snapshot failed.\n");
        exit(1);
    }
    for(int i = 0; i < count; ++i){
        int head_pos = (g_result_queue.head + i) % g_result_queue.capacity;
        results_snapshot[i] = g_result_queue.results[head_pos];
    }
    g_result_queue.head = (g_result_queue.head + count) % g_result_queue.capacity;
    g_result_queue.count -= count;

    pthread_mutex_unlock(&g_result_queue.lock);
    
    for(int i = 0; i < count; ++i){
        int fd = results_snapshot[i].fd;
        struct conn* conn_state = &conn_list[fd];

        pthread_mutex_lock(&conn_state->lock);
        if (conn_state->ref_count == 0) { 
            pthread_mutex_unlock(&conn_state->lock);
            continue;
        }

        pthread_mutex_unlock(&conn_state->lock);
        set_event(fd, EPOLLOUT, 0); 
    }
    free(results_snapshot);

#else
    result_t* results_to_send = malloc(sizeof(result_t) * count);
    if(results_to_send == NULL){
        printf("malloc results_to_send failed.\n");
        exit(1);
    }

    for(int i = 0; i < count; i++) {
        int head_pos = (g_result_queue.head + i) % g_result_queue.capacity;
        results_to_send[i] = g_result_queue.results[head_pos];
    }
    g_result_queue.head = (g_result_queue.head + count) % g_result_queue.capacity;
    g_result_queue.count -= count;
    pthread_mutex_unlock(&g_result_queue.lock);

    for (int i = 0; i < count; i++) {
        int fd = results_to_send[i].fd;
        struct conn* conn_state = &conn_list[fd];

        // 需要判断这个 fd 是否已经被 close 了
        pthread_mutex_lock(&conn_state->lock);
        if (conn_state->ref_count == 0) { 
            pthread_mutex_unlock(&conn_state->lock);
            free(results_to_send[i].w_buffer);
            continue;
        }

        if (conn_state->w_buffer) {
            free(conn_state->w_buffer);
        }
        conn_state->w_buffer = results_to_send[i].w_buffer;
        conn_state->w_length = results_to_send[i].w_length;
        
        pthread_mutex_unlock(&conn_state->lock);

        set_event(fd, EPOLLOUT, 0); 
    }

    free(results_to_send);
#endif
}



typedef struct{
    int fd;
    char* request;
    int len;
}task_data_t;

int protocol_handle(char* request, int req_len, char* response){
    // echo
#if ENABLE_SERVICE_SIMULATION
    // 模拟业务处理操作，比如查询数据库

    long random_ns = rand() % 10000000;  // 10,000,000 ns = 10 ms
    struct timespec sleep_time = {0};
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = random_ns;

    nanosleep(&sleep_time, NULL);

 
#endif
    memcpy(response, request, req_len);
    return req_len;
}

// fd recv_buf recv_len
void task_callback(void* arg){
    struct nTask* task = (struct nTask*)arg;
    if(task == NULL){
        printf("task_callback arg == NULL.\n");
        exit(1);
    }

    task_data_t* task_data = task->user_date;
# if XIUGAI
    int fd = task_data->fd;;
    struct conn* conn_state = &conn_list[fd];
    pthread_mutex_lock(&conn_state->lock);

    char* response = malloc(MAX_RESPONSE_SIZE);
    if(response == NULL){
        printf("malloc response failed.\n");
        exit(1);
    }

    int len = protocol_handle(task_data->request, task_data->len, response);
    conn_state->w_buffer = response;
    conn_state->w_length = len;

    pthread_mutex_unlock(&conn_state->lock);
    submit_result_to_io_thread(task_data->fd);

#else
    char* response = malloc(MAX_RESPONSE_SIZE);
    if(response == NULL){
        printf("malloc response failed.\n");
        exit(1);
    }
    int res_len = protocol_handle(task_data->request, task_data->len, response); 
    submit_result_to_io_thread(task_data->fd, response, res_len);
#endif
    task_data_t* data = (task_data_t*)task->user_date;
    conn_dec_ref(&conn_list[data->fd]); // 此 worker 线程执行完这个任务，就减少对应 fd 的引用计数
    free(data->request);
    free(task->user_date);
    free(task);
}



// ===========================================================================

int create_listenfd(char* port){
    if(port == NULL) return -1;

    int socketfd = socket(AF_INET, SOCK_STREAM, 0);
    if(socketfd < 0) return -2;

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    uint16_t p = (uint16_t)atoi(port);
    listen_addr.sin_port = htons(p);


    if(bind(socketfd, (struct sockaddr*)&listen_addr, sizeof(struct sockaddr_in)) == -1){
        perror("bind");
        return -3;
    }

    if(listen(socketfd, 1024) == -1){
        perror("listen");
        return -4;
    }
    printf("tcp server is listening, port: %s\n", port);
    return socketfd;
}

int set_event(int fd, int event,int flag){
    struct epoll_event ev = {0};
    if(flag){
        ev.data.fd = fd;
        ev.events = event;
        epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    }
    else{
        ev.data.fd = fd;
        ev.events = event;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev);
    }
    return 0;
}

int listenfd_event_register(int listenfd){ //
    if(listenfd < 0) return -1;

    memset(conn_list + listenfd, 0 , sizeof(struct conn));
    conn_list[listenfd].fd = listenfd;
    conn_list[listenfd].r_action.recv_cb = accept_callback;

    set_event(listenfd, EPOLLIN, 1); // 可读
    return 0;
}

int clientfd_event_register(int clientfd){
    if(clientfd < 0) return -1;

    memset(conn_list + clientfd, 0 , sizeof(struct conn));
    conn_list[clientfd].fd = clientfd;
    conn_list[clientfd].r_action.recv_cb = recv_callback;
    conn_list[clientfd].send_cb = send_callback;
    conn_list[clientfd].ref_count = 1; // -> I/O 线程
    pthread_mutex_init(&conn_list[clientfd].lock, NULL);

    set_event(clientfd, EPOLLIN, 1); // 可读
    return 0;
}

int accept_callback(int listenfd){
    struct sockaddr_in client_addr = {0};
    socklen_t len = sizeof(client_addr);
    int clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &len);
    if(clientfd == -1){
        perror("accept");
        return -1;
    }
    clientfd_event_register(clientfd);
    return 0;

}

int recv_callback(int clientfd){
    int recv_len = recv(clientfd, conn_list[clientfd].r_buffer, BUFFER_SIZE, 0);
    if(recv_len < 0){
        // perror("recv");
        conn_dec_ref(&conn_list[clientfd]);
        return -1;
    }
    else if(recv_len == 0){
        conn_dec_ref(&conn_list[clientfd]);
        return 0;
    }
    conn_list[clientfd].r_length = recv_len;

    char* request_copy = malloc(recv_len);
    if(request_copy == NULL){
        printf("malloc request_copy failed.\n");
        exit(1);
    }
    memcpy(request_copy, conn_list[clientfd].r_buffer, recv_len);

    // 封装任务放进线程池
    task_data_t* task_data = malloc(sizeof(task_data_t));
    if(task_data == NULL){
        printf("malloc task_data failed.\n");
        exit(1);
    }
    task_data->fd = clientfd;
    task_data->request = request_copy;
    task_data->len = recv_len;

    struct nTask* task = malloc(sizeof(struct nTask));
    if(task == NULL){
        printf("malloc task failed.\n");
        exit(1);
    }
    task->task_func = task_callback;
    task->user_date = task_data;

    nThreadPoolPushTask(&g_pool, task);
    conn_inc_ref(&conn_list[clientfd]); // 会被 worker 线程取到使用，增加引用计数

    return 0;
}

// 支持分段发送
int send_callback(int clientfd){
    struct conn* conn_state = &conn_list[clientfd];
    if (conn_state->w_buffer == NULL || conn_state->w_length == 0) {
        // 发送完成，重置偏移，转为监听读事件
        conn_state->send_offset = 0;
        set_event(clientfd, EPOLLIN, 0);
        return 0;
    }

    // 尝试发送剩余数据
    size_t to_send = conn_state->w_length - conn_state->send_offset;
    int send_len = send(clientfd, conn_state->w_buffer + conn_state->send_offset, to_send, 0);
    if (send_len > 0) {
        conn_state->send_offset += send_len;
        if (conn_state->send_offset >= conn_state->w_length) {
            free(conn_state->w_buffer);
            conn_state->w_buffer = NULL;
            conn_state->w_length = 0;
            conn_state->send_offset = 0;
            set_event(clientfd, EPOLLIN, 0);
            return 0;
        }
        // 部分发送，继续尝试，下次epoll触发
        return 0;
    } 
    else if (send_len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 发送缓冲区满，保持EPOLLOUT，下次可写时重试
            return 0;
        } 
        else {
            // 如果不是发送缓冲区满导致的错误，就关闭这个连接
            perror("send");
            free(conn_state->w_buffer);
            conn_state->w_buffer = NULL;
            conn_dec_ref(conn_state);
            return -1;
        }
    } 
    else {
        free(conn_state->w_buffer);
        conn_state->w_buffer = NULL;
        conn_dec_ref(conn_state);
        return -1;
    }
}

int main(int argc, char** argv){
    if(argc < 2){
        printf("args not enough.\n");
        return -1;
    }

    epollfd = epoll_create(1);

    // 用于 I/O 线程与 worker 线程的通信 -> worker 线程通知 I/O 线程自己处理完了
    g_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); 
    if (g_eventfd < 0) {
        perror("eventfd");
        return -1;
    }
    struct epoll_event ev_eventfd = {0};
    ev_eventfd.data.fd = g_eventfd;
    ev_eventfd.events = EPOLLIN | EPOLLET; // 监听可读，每次 worker 向 result queue 中写结果都触发
    epoll_ctl(epollfd, EPOLL_CTL_ADD, g_eventfd, &ev_eventfd);

    struct epoll_event events[EPOLL_EVENT_SIZE] = {0};
    int listenfd = create_listenfd(argv[1]);
    listenfd_event_register(listenfd);

    nThreadPoolInit(&g_pool, WORKER_THREAD_NUM);
    result_queue_init();


    while(1){
        int nready = epoll_wait(epollfd, events, EPOLL_EVENT_SIZE, -1);
        for(int i = 0; i < nready; ++i){
            int fd = events[i].data.fd;

            if (fd == g_eventfd) {
                uint64_t val;
                eventfd_read(g_eventfd, &val); // 读取以清空计数器
                handle_pending_results(); 
                continue;
            }

            if(events[i].events & EPOLLIN){
                conn_list[fd].r_action.recv_cb(fd);
            }
            if(events[i].events & EPOLLOUT){
                conn_list[fd].send_cb(fd);
            }
        }
    }

end:
#if XIUGAI
    nThreadPoolDestroy(&g_pool);

    for(int fd = 0; fd < CONN_LIST_SIZE; ++fd){
        struct conn* conn_state = &conn_list[fd];
        if(conn_state->w_buffer != NULL){
            free(conn_state->w_buffer);
        }
        if(conn_state->ref_count > 0){ // 说明此 fd 还没被关闭
            close(conn_state->fd);
            pthread_mutex_destroy(&conn_state->lock);
        }
    }
    if(g_result_queue.results != NULL){
        free(g_result_queue.results);
    }

    close(g_eventfd);
    close(epollfd);
    close(listenfd);
#endif
    return 0;
}
