/*
 * 主从reactor+多线程
 * mainreactor->accept   subreactor->recv/send
 * worker线程 -> 只负责业务逻辑处理
 * 相对于单reactor模式更擅长处理高并发和I/O密集型的情况，这种情况下通过解耦accept和recv能达到更高的QPS,
 * 通俗来讲，单reactor的accept和recv会互相限制，处理accept就可能导致recv不及时->吞吐量下降；
 *                                   处理recv就可能导致accept不及时->连接时延变长；
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h> // eventfd

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "thread_pool.h"

#define BUFFER_SIZE                   1024
#define CONN_LIST_SIZE                1024 * 128
#define EPOLL_EVENT_SIZE              1024
#define WORKER_THREAD_NUM             64
#define SUBREACTOR_THREAD_NUM         8
#define TASK_QUEUE_SIZE               1024
#define FD_QUEUE_SIZE                 1024
#define MAX_RESPONSE_SIZE             1024    

#define ENABLE_SERVICE_SIMULATION     1 // 模拟业务处理逻辑

/*
 * QPS 极限 = (1000ms / 10ms) * 64 = 6400
 * 实测 QPS =  4158
 */

typedef struct
{
    int fd;
    char *w_buffer;
    long w_length;
} result_t;

typedef struct
{
    result_t *results; // ringbuf
    int head;
    int tail;
    int count;
    int capacity;
    pthread_mutex_t lock;
} result_queue_t;

struct subreactor_state_s;

struct conn
{
    int fd;

    char r_buffer[BUFFER_SIZE];
    char *w_buffer;
    long r_length;
    long w_length;

    // accept_cb for main reactor
    int (*accept_cb)(int fd);
    int (*recv_cb)(int fd, struct subreactor_state_s *state);
    int (*send_cb)(int fd, struct subreactor_state_s *state);
    /*
     * 防止还有线程正在用这个 fd 时，其他线程因为某些原因将 fd close 了.
     * 比如 I/O 线程收到了断连请求，将 fd close了，，但任务队列中还有此 fd 的任务，内核将此 fd 回收，并对后面的新连接分配此 fd，
     * worker 线程取走旧 fd 的任务并完成，然后 I/O 线程对旧 fd 执行 send 任务，但此时数据发到新 fd 那里去了。
     */
    int ref_count;
    pthread_mutex_t lock;
};

struct conn g_conn_list[CONN_LIST_SIZE];
nThreadPool g_worker_pool;

typedef struct
{
    int *clientfds; // ringbuf
    int head;
    int tail;
    int count;
    int capacity;
    pthread_mutex_t lock;
} fd_queue_t;

// result_queue 由 subreactor 负责管理，fd_queue 由 mainreactor 负责管理
typedef struct subreactor_state_s
{
    int epollfd;

    result_queue_t result_queue;
    int resqueue_eventfd;

    int fdqueue_eventfd;
    fd_queue_t *fd_queue;

} subreactor_state_t;

subreactor_state_t subreactor_state_list[SUBREACTOR_THREAD_NUM];

typedef struct
{
    int index;
    int fdqueue_eventfd;
    fd_queue_t fd_queue;
    pthread_t threadid;
} subreactor_data_t;

subreactor_data_t subreactor_data_list[SUBREACTOR_THREAD_NUM] = {0};

int recv_callback(int fd, subreactor_state_t *state);
int send_callback(int fd, subreactor_state_t *state);
int set_event(int fd, int event, int flag, subreactor_state_t *state);
int clientfd_event_register(int clientfd, subreactor_state_t *state);

// =============================================================================

void conn_inc_ref(struct conn *conn_state)
{
    pthread_mutex_lock(&conn_state->lock);
    ++conn_state->ref_count;
    pthread_mutex_unlock(&conn_state->lock);
}

void conn_dec_ref(struct conn *conn_state)
{
    pthread_mutex_lock(&conn_state->lock);
    --conn_state->ref_count;
    if (conn_state->ref_count == 0)
    {
        if (conn_state->w_buffer)
        {
            free(conn_state->w_buffer);
            conn_state->w_buffer = NULL;
        }
        close(conn_state->fd);
    }
    pthread_mutex_unlock(&conn_state->lock);
}

void result_queue_init(result_queue_t *result_queue)
{
    result_queue->results = (result_t *)malloc(sizeof(result_t) * TASK_QUEUE_SIZE);
    if (result_queue->results == NULL)
    {
        printf("malloc results queue failed.\n");
        exit(1);
    }
    result_queue->head = 0;
    result_queue->tail = 0;
    result_queue->count = 0;
    result_queue->capacity = TASK_QUEUE_SIZE;
    pthread_mutex_init(&result_queue->lock, NULL);
}

void submit_result_to_io_thread(int fd, char *resp_buf, int len, subreactor_state_t *state)
{
    pthread_mutex_lock(&state->result_queue.lock);

    if (state->result_queue.count == state->result_queue.capacity)
    {
        int new_capacity = state->result_queue.capacity * 2;
        printf("Result queue is full. Reallocating from %d to %d\n", state->result_queue.capacity, new_capacity);

        result_t *new_results = (result_t *)malloc(new_capacity * sizeof(result_t));
        if (new_results == NULL)
        {
            printf("malloc for new result queue failed.\n");
            exit(1);
        }

        int head_to_end_count = state->result_queue.capacity - state->result_queue.head;
        memcpy(new_results, state->result_queue.results + state->result_queue.head, head_to_end_count * sizeof(result_t));
        memcpy(new_results + head_to_end_count, state->result_queue.results, state->result_queue.tail * sizeof(result_t));

        free(state->result_queue.results);
        state->result_queue.results = new_results;
        state->result_queue.capacity = new_capacity;
        state->result_queue.head = 0;
        state->result_queue.tail = state->result_queue.count;
    }

    int tail = state->result_queue.tail;
    state->result_queue.results[tail].fd = fd;
    state->result_queue.results[tail].w_buffer = resp_buf;
    state->result_queue.results[tail].w_length = len;
    state->result_queue.tail = (tail + 1) % state->result_queue.capacity; // ring
    state->result_queue.count++;

    pthread_mutex_unlock(&state->result_queue.lock);

    uint64_t val = 1;
    eventfd_write(state->resqueue_eventfd, val); // 向 g_eventfd 写值唤醒 epollwait
}

void submit_clientfd_to_subreactor(int clientfd, int index)
{
    subreactor_data_t *subreactor = &subreactor_data_list[index];

    pthread_mutex_lock(&subreactor->fd_queue.lock);

    if (subreactor->fd_queue.count == subreactor->fd_queue.capacity)
    {
        int new_capacity = subreactor->fd_queue.capacity * 2;
        printf("Result queue is full. Reallocating from %d to %d\n", subreactor->fd_queue.capacity, new_capacity);

        int *new_fds = (int *)malloc(new_capacity * sizeof(int));
        if (new_fds == NULL)
        {
            printf("malloc for new result queue failed.\n");
            exit(1);
        }

        int head_to_end_count = subreactor->fd_queue.capacity - subreactor->fd_queue.head;
        memcpy(new_fds, subreactor->fd_queue.clientfds + subreactor->fd_queue.head, head_to_end_count * sizeof(int));
        memcpy(new_fds + head_to_end_count, subreactor->fd_queue.clientfds, subreactor->fd_queue.tail * sizeof(int));

        free(subreactor->fd_queue.clientfds);
        subreactor->fd_queue.clientfds = new_fds;
        subreactor->fd_queue.capacity = new_capacity;
        subreactor->fd_queue.head = 0;
        subreactor->fd_queue.tail = subreactor->fd_queue.count;
    }

    int tail = subreactor->fd_queue.tail;
    subreactor->fd_queue.clientfds[tail] = clientfd;
    subreactor->fd_queue.tail = (tail + 1) % subreactor->fd_queue.capacity; // ring
    subreactor->fd_queue.count++;

    pthread_mutex_unlock(&subreactor->fd_queue.lock);

    uint64_t val = 1;
    eventfd_write(subreactor->fdqueue_eventfd, val); // 向 g_eventfd 写值唤醒 epollwait
}

void handle_pending_fds(int index)
{
    subreactor_state_t *state = &subreactor_state_list[index];
    pthread_mutex_lock(&state->fd_queue->lock);

    int count = state->fd_queue->count;
    if (count == 0)
    {
        pthread_mutex_unlock(&state->fd_queue->lock);
        return;
    }

    int *fds_to_handle = malloc(sizeof(int) * count);
    if (fds_to_handle == NULL)
    {
        printf("malloc for fds_to_handle failed.\n");
        exit(1);
    }

    for (int i = 0; i < count; i++)
    {
        int head_pos = (state->fd_queue->head + i) % state->fd_queue->capacity;
        fds_to_handle[i] = state->fd_queue->clientfds[head_pos];
    }
    state->fd_queue->head = (state->fd_queue->head + count) % state->fd_queue->capacity;
    state->fd_queue->count -= count;
    pthread_mutex_unlock(&state->fd_queue->lock);

    for (int i = 0; i < count; i++)
    {
        int fd = fds_to_handle[i];
        clientfd_event_register(fd, state);
    }

    free(fds_to_handle);
}

void handle_pending_results(subreactor_state_t *state)
{
    pthread_mutex_lock(&state->result_queue.lock);

    int count = state->result_queue.count;
    result_t *results_to_send = malloc(sizeof(result_t) * count);
    if (results_to_send == NULL)
    {
        printf("malloc results_to_send failed.\n");
        exit(1);
    }

    for (int i = 0; i < count; i++)
    {
        int head_pos = (state->result_queue.head + i) % state->result_queue.capacity;
        results_to_send[i] = state->result_queue.results[head_pos];
    }
    state->result_queue.head = (state->result_queue.head + count) % state->result_queue.capacity;
    state->result_queue.count -= count;
    pthread_mutex_unlock(&state->result_queue.lock);

    for (int i = 0; i < count; i++)
    {
        int fd = results_to_send[i].fd;
        struct conn *conn_state = &g_conn_list[fd];

        // 需要判断这个 fd 是否已经被 close 了
        pthread_mutex_lock(&conn_state->lock);
        if (conn_state->ref_count == 0)
        {
            pthread_mutex_unlock(&conn_state->lock);
            free(results_to_send[i].w_buffer);
            continue;
        }

        if (conn_state->w_buffer)
        {
            free(conn_state->w_buffer);
        }
        conn_state->w_buffer = results_to_send[i].w_buffer;
        conn_state->w_length = results_to_send[i].w_length;

        pthread_mutex_unlock(&conn_state->lock);

        set_event(fd, EPOLLOUT, 0, state);
    }

    free(results_to_send);
}

typedef struct
{
    int fd;
    char *request;
    int len;
    subreactor_state_t *state;
} task_data_t;

int protocol_handle(char *request, int req_len, char *response)
{
    // echo
#if ENABLE_SERVICE_SIMULATION
    // 模拟业务处理操作，比如查询数据库
    #include<time.h>

    struct timespec sleep_time = {0};
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 10 * 1000 * 1000; // 10ms

    nanosleep(&sleep_time, NULL);

    for (int i = 0; i < 1000; ++i);
#endif
    memcpy(response, request, req_len);
    return req_len;
}

// fd recv_buf recv_len
void task_callback(void *arg)
{
    struct nTask *task = (struct nTask *)arg;
    if (task == NULL)
    {
        printf("task_callback arg == NULL.\n");
        exit(1);
    }

    task_data_t *task_data = task->user_date;
    char *response = malloc(MAX_RESPONSE_SIZE);
    if (response == NULL)
    {
        printf("malloc response failed.\n");
        exit(1);
    }
    int res_len = protocol_handle(task_data->request, task_data->len, response);
    submit_result_to_io_thread(task_data->fd, response, res_len, task_data->state); // 需要传入 &result_queue res_eventfd

    task_data_t *data = (task_data_t *)task->user_date;
    conn_dec_ref(&g_conn_list[data->fd]); // 此 worker 线程执行完这个任务，就减少对应 fd 的引用计数
    free(data->request);
    free(task->user_date);
    free(task);
}

// ===========================================================================

int set_event(int fd, int event, int flag, subreactor_state_t *state)
{
    struct epoll_event ev = {0};
    if (flag)
    {
        ev.data.fd = fd;
        ev.events = event;
        epoll_ctl(state->epollfd, EPOLL_CTL_ADD, fd, &ev);
    }
    else
    {
        ev.data.fd = fd;
        ev.events = event;
        epoll_ctl(state->epollfd, EPOLL_CTL_MOD, fd, &ev);
    }
    return 0;
}

int clientfd_event_register(int clientfd, subreactor_state_t *state)
{
    if (clientfd < 0) return -1;

    memset(g_conn_list + clientfd, 0, sizeof(struct conn));
    g_conn_list[clientfd].fd = clientfd;
    g_conn_list[clientfd].recv_cb = recv_callback;
    g_conn_list[clientfd].send_cb = send_callback;
    g_conn_list[clientfd].ref_count = 1; // -> I/O 线程
    pthread_mutex_init(&g_conn_list[clientfd].lock, NULL);

    set_event(clientfd, EPOLLIN, 1, state); // 可读
    return 0;
}

int recv_callback(int clientfd, subreactor_state_t *state)
{
    int recv_len = recv(clientfd, g_conn_list[clientfd].r_buffer, BUFFER_SIZE, 0);
    if (recv_len < 0)
    {
        // perror("recv");
        conn_dec_ref(&g_conn_list[clientfd]);
        return -1;
    }
    else if (recv_len == 0)
    {
        conn_dec_ref(&g_conn_list[clientfd]);
        return 0;
    }
    g_conn_list[clientfd].r_length = recv_len;

    char *request_copy = malloc(recv_len);
    if (request_copy == NULL)
    {
        printf("malloc request_copy failed.\n");
        exit(1);
    }
    memcpy(request_copy, g_conn_list[clientfd].r_buffer, recv_len);

    // 封装任务放进线程池
    task_data_t *task_data = malloc(sizeof(task_data_t));
    if (task_data == NULL)
    {
        printf("malloc task_data failed.\n");
        exit(1);
    }
    task_data->fd = clientfd;
    task_data->request = request_copy;
    task_data->len = recv_len;
    task_data->state = state;

    struct nTask *task = malloc(sizeof(struct nTask));
    if (task == NULL)
    {
        printf("malloc task failed.\n");
        exit(1);
    }
    task->task_func = task_callback;
    task->user_date = task_data;

    nThreadPoolPushTask(&g_worker_pool, task);
    conn_inc_ref(&g_conn_list[clientfd]); // 会被 worker 线程取到使用，增加引用计数

    return 0;
}

int send_callback(int clientfd, subreactor_state_t *state)
{
    struct conn *conn_state = &g_conn_list[clientfd];
    if (conn_state->w_buffer == NULL || conn_state->w_length == 0)
    {
        set_event(clientfd, EPOLLIN, 0, state);
        return 0;
    }

    int send_len = send(clientfd, g_conn_list[clientfd].w_buffer, g_conn_list[clientfd].w_length, 0);
    if (send_len < 0)
    {
        perror("send");
        if (conn_state->w_buffer != NULL)
        {
            free(conn_state->w_buffer);
        }
        conn_state->w_buffer = NULL;
        conn_dec_ref(conn_state);
        return -1;
    }

    free(conn_state->w_buffer);
    conn_state->w_buffer = NULL;
    conn_state->w_length = 0;

    set_event(clientfd, EPOLLIN, 0, state); // 写完关注可读事件

    return 0;
}

// 传入 mainractor 创建的属于此 subreactor 的 eventfd 和 fd_queue
void *subreactor_callback(void *arg)
{
    subreactor_data_t *data = (subreactor_data_t *)arg;
    int index = data->index;

    subreactor_state_t *sub_state = NULL;
    sub_state = &subreactor_state_list[index];
    memset(sub_state, 0, sizeof(subreactor_state_t));

    int epollfd = epoll_create(1);
    sub_state->epollfd = epollfd;

    sub_state->fd_queue = &data->fd_queue;
    sub_state->fdqueue_eventfd = data->fdqueue_eventfd;

    struct epoll_event ev_eventfd = {0};
    ev_eventfd.data.fd = sub_state->fdqueue_eventfd;
    ev_eventfd.events = EPOLLIN | EPOLLET; // 监听可读，每次 mainreactor 向 fd_queue 中写结果都触发
    epoll_ctl(sub_state->epollfd, EPOLL_CTL_ADD, sub_state->fdqueue_eventfd, &ev_eventfd);

    // 用于 I/O 线程与 worker 线程的通信 -> worker 线程通知 I/O 线程自己处理完了
    int result_queue_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (result_queue_eventfd < 0)
    {
        perror("eventfd");
        exit(1);
    }
    sub_state->resqueue_eventfd = result_queue_eventfd;
    memset(&ev_eventfd, 0, sizeof(struct epoll_event));
    ev_eventfd.data.fd = result_queue_eventfd;
    ev_eventfd.events = EPOLLIN | EPOLLET; // 监听可读，每次 worker 向 result queue 中写结果都触发
    epoll_ctl(sub_state->epollfd, EPOLL_CTL_ADD, sub_state->resqueue_eventfd, &ev_eventfd);

    result_queue_init(&sub_state->result_queue);

    struct epoll_event events[EPOLL_EVENT_SIZE] = {0};
    while (1)
    {
        int nready = epoll_wait(epollfd, events, EPOLL_EVENT_SIZE, -1);
        for (int i = 0; i < nready; ++i)
        {
            int fd = events[i].data.fd;

            if (fd == result_queue_eventfd)
            {
                uint64_t val;
                eventfd_read(result_queue_eventfd, &val); // 读取以清空计数器
                handle_pending_results(sub_state);
                continue;
            }

            if (fd == sub_state->fdqueue_eventfd)
            {
                uint64_t val;
                eventfd_read(sub_state->fdqueue_eventfd, &val); // 读取以清空计数器
                handle_pending_fds(index);
                continue;
            }

            if (events[i].events & EPOLLIN)
            {
                g_conn_list[fd].recv_cb(fd, sub_state);
            }
            if (events[i].events & EPOLLOUT)
            {
                g_conn_list[fd].send_cb(fd, sub_state);
            }
        }
    }

end:
    for (int fd = 0; fd < BUFFER_SIZE; ++fd)
    {
        struct conn *conn_state = &g_conn_list[fd];
        if (conn_state->w_buffer != NULL)
        {
            free(conn_state->w_buffer);
        }
    }

    if (sub_state->result_queue.results != NULL)
    {
        free(sub_state->result_queue.results);
    }
    return NULL;
}

// ==========================================================================================

/*
 * mainreactor 将建立好连接的 fd 轮询放入到与 subreactor 相关的 fd_queue 中，并通知对应 subreactor，
 * subreactor 取出 fd 并用自己的 epoll 进行管理
 */

#if 1

int create_listenfd(char *port)
{
    if (port == NULL) return -1;

    int socketfd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketfd < 0) return -2;

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    uint16_t p = (uint16_t)atoi(port);
    listen_addr.sin_port = htons(p);

    if (bind(socketfd, (struct sockaddr *)&listen_addr, sizeof(struct sockaddr_in)) == -1)
    {
        perror("bind");
        return -3;
    }

    if (listen(socketfd, 1024) == -1)
    {
        perror("listen");
        return -4;
    }
    printf("tcp server is listening, port: %s\n", port);
    return socketfd;
}

int accept_callback(int listenfd);

int listenfd_event_register(int listenfd, int epollfd)
{ //
    if (listenfd < 0) return -1;

    memset(g_conn_list + listenfd, 0, sizeof(struct conn));
    g_conn_list[listenfd].fd = listenfd;
    g_conn_list[listenfd].accept_cb = accept_callback;

    struct epoll_event ev = {0};
    ev.data.fd = listenfd;
    ev.events = EPOLLIN;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev);
    return 0;
}

void fd_queue_init(fd_queue_t *fd_queue)
{
    fd_queue->clientfds = (int *)malloc(sizeof(int) * FD_QUEUE_SIZE);
    if (fd_queue->clientfds == NULL)
    {
        printf("malloc results queue failed.\n");
        exit(1);
    }
    fd_queue->head = 0;
    fd_queue->tail = 0;
    fd_queue->count = 0;
    fd_queue->capacity = FD_QUEUE_SIZE;
    pthread_mutex_init(&fd_queue->lock, NULL);
}

int accept_callback(int listenfd)
{
    struct sockaddr_in client_addr = {0};
    socklen_t len = 0;
    int clientfd = accept(listenfd, (struct sockaddr *)&client_addr, &len);
    if (clientfd == -1)
    {
        perror("accept");
        return -1;
    }

    submit_clientfd_to_subreactor(clientfd, clientfd % SUBREACTOR_THREAD_NUM);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("args not enough.\n");
        return -1;
    }

    int listenfd = create_listenfd(argv[1]);

    int epollfd = epoll_create(1);
    listenfd_event_register(listenfd, epollfd);

    nThreadPoolInit(&g_worker_pool, WORKER_THREAD_NUM);

    // 创建 sub_reactor 
    for (int i = 0; i < SUBREACTOR_THREAD_NUM; ++i)
    {
        subreactor_data_list[i].index = i;

        int fdqueue_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fdqueue_eventfd < 0)
        {
            perror("eventfd");
            return -1;
        }
        subreactor_data_list[i].fdqueue_eventfd = fdqueue_eventfd;
        subreactor_data_list[i].fd_queue;
        fd_queue_init(&subreactor_data_list[i].fd_queue);

        pthread_create(&subreactor_data_list[i].threadid, NULL, subreactor_callback, &subreactor_data_list[i]);
    }

    struct epoll_event events[EPOLL_EVENT_SIZE] = {0};
    while (1)
    {
        int nready = epoll_wait(epollfd, events, EPOLL_EVENT_SIZE, -1);
        for (int i = 0; i < nready; ++i)
        {
            int fd = events[i].data.fd;
            if (events[i].events & EPOLLIN)
            {
                g_conn_list[fd].accept_cb(fd);
            }
        }
    }

end:
    for (int i = 0; i < SUBREACTOR_THREAD_NUM; ++i)
    {
        if (subreactor_data_list[i].fd_queue.clientfds != NULL)
        {
            free(subreactor_data_list[i].fd_queue.clientfds);
        }
    }

    nThreadPoolDestroy(&g_worker_pool);
}

#endif