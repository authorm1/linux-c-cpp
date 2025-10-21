#define _GNU_SOURCE
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/epoll.h>

#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include <fcntl.h>

#include"reactor.h"

#define ECHO 0

// 业务逻辑处理
static protocol_handle g_service_handle;

int accept_callback(int fd);
int recv_callback(int fd);
int send_callback(int fd);
int set_event(int fd, int event,int flag);

int epollfd = 0;
struct conn conn_list[CONN_LIST_SIZE] = {0};

#if ENABLE_THREADPOOL

static reactor_threadpool_t g_threadpool;


// task->user_data = &fd;
void protocol_handle_entry(void* arg){
    reactor_task_t* task = (reactor_task_t*)arg;

    int clientfd = *(int*)(task->user_date);
    struct conn* conn_state = &conn_list[clientfd];

    conn_state->w_length = g_service_handle(conn_state->r_buffer, conn_state->r_length, conn_state->w_buffer);

    set_event(clientfd, EPOLLOUT, 0); // 读完关注可写事件
    free(task);
}


#endif

#if TCP_PKT_SPLIT_PASTE

void conn_cleanup(struct conn* conn_state){
    if(conn_state == NULL) return;

    free(conn_state->recv_buf);
    while(1){
        send_queue_node_t* send_info = send_queue_dequeue(conn_state->send_queue);
        if(send_info == NULL) break;
        if(send_info->data != NULL) free(send_info->data);
        free(send_info);
    }

    struct epoll_event ev = {0};
    epoll_ctl(epollfd, EPOLL_CTL_DEL, conn_state->fd, &ev);

    close(conn_state->fd);
    memset(conn_list + conn_state->fd, 0, sizeof(struct conn));
}

#endif

int create_listenfd(uint16_t port){

    int socketfd = socket(AF_INET, SOCK_STREAM, 0);
    if(socket < 0) return -2;

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(port);


    if(bind(socketfd, (struct sockaddr*)&listen_addr, sizeof(struct sockaddr_in)) == -1){
        perror("bind");
        return -3;
    }

    if(listen(socketfd, 10) == -1){
        perror("listen");
        return -4;
    }
    printf("tcp server is listening, port: %d\n", port);
    return socketfd;
}

int set_event(int fd, int event,int flag){
    struct epoll_event ev = {0};
    if(flag){ // !0
        ev.data.fd = fd;
        ev.events = event;
        epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    }
    else{ // 0
        ev.data.fd = fd;
        ev.events = event;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev);
    }
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

#if TCP_PKT_SPLIT_PASTE

    conn_list[clientfd].capacity = RECV_BUFFER_SIZE;
    conn_list[clientfd].read_pos = 0;
    conn_list[clientfd].process_pos = 0;
    conn_list[clientfd].recv_buf = malloc(RECV_BUFFER_SIZE);
    if(conn_list[clientfd].recv_buf == NULL){
        printf("malloc recv_buf failed.\n");
        exit(1);
    }
    memset(conn_list[clientfd].recv_buf, 0, RECV_BUFFER_SIZE);
    conn_list[clientfd].send_queue = send_queue_create();

#endif

    set_event(clientfd, EPOLLIN, 1); // 可读
    return 0;
}

int accept_callback(int listenfd){
    struct sockaddr_in client_addr = {0};
    socklen_t len = 0;
    int clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &len);

    // set non-block
    int flags = fcntl(clientfd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl(clientfd, F_SETFL, flags);

    if(clientfd == -1){
        perror("accept");
        return -1;
    }

    clientfd_event_register(clientfd);
    return 0;

}

int recv_callback(int clientfd){
#if TCP_PKT_SPLIT_PASTE
    struct conn* conn_state = &conn_list[clientfd];
    int recv_len = recv(clientfd, conn_state->recv_buf + conn_state->read_pos, conn_state->capacity - conn_state->read_pos, 0);
    if(recv_len < 0){
        perror("recv");
        conn_cleanup(conn_state);
        return -1;
    }
    else if(recv_len == 0){
        conn_cleanup(conn_state);
        return 0;
    }
    conn_state->read_pos +=  recv_len;
    if(conn_state->read_pos == conn_state->capacity){
        if(conn_state->process_pos == 0){ // 扩容
            char* new_buf = realloc(conn_state->recv_buf, 2 * conn_state->capacity);
            if(new_buf == NULL){
                printf("realloc recv buf failed.\n");
                exit(1);
            }
            conn_state->capacity *= 2;
            conn_state->recv_buf = new_buf;
        }
        else if(conn_state->process_pos > 0){ // 前移
            int data_len = conn_state->read_pos - conn_state->process_pos;
            memmove(conn_state->recv_buf, conn_state->recv_buf + conn_state->process_pos, data_len);
            conn_state->process_pos = 0;
            conn_state->read_pos = data_len;
        }
    }

    while(1){
        char* haystack = conn_state->recv_buf + conn_state->process_pos;
        int haystack_len = conn_state->read_pos - conn_state->process_pos;
        char* flag = memmem(haystack, haystack_len, "\r\n", 2);
        if(flag == NULL) break; //分包
        else{ // 可能粘包
            int cmd_len = flag - (conn_state->recv_buf + conn_state->process_pos) + 2; // /r/n
            char* send_data = malloc(SEND_BUFFER_SIZE);
            memset(send_data, 0, SEND_BUFFER_SIZE);
            int send_length = g_service_handle(conn_state->recv_buf + conn_state->process_pos, cmd_len, send_data);
            send_queue_enqueue(conn_state->send_queue, send_data, send_length);
            conn_state->process_pos = flag - conn_state->recv_buf + 2;
        }
    }

    int is_empty = send_queue_is_empty(conn_state->send_queue);
    if(is_empty == 0) // sendqueue empty
        set_event(clientfd, EPOLLIN, 0);
    else if(is_empty > 0)
        set_event(clientfd, EPOLLOUT, 0);


    return 0;

#else
    int recv_len = recv(clientfd, conn_list[clientfd].r_buffer, BUFFER_SIZE, 0);
    if(recv_len < 0){
        perror("recv");
        struct epoll_event ev = {0};
        epoll_ctl(epollfd, EPOLL_CTL_DEL, clientfd, &ev);
        close(clientfd);
        return -1;
    }
    else if(recv_len == 0){
        struct epoll_event ev = {0};
        epoll_ctl(epollfd, EPOLL_CTL_DEL, clientfd, &ev);
        close(clientfd);
        return 0;
    }
    conn_list[clientfd].r_length = recv_len;
#if ECHO
    conn_list[clientfd].w_length = conn_list[clientfd].r_length;
    memcpy(conn_list[clientfd].w_buffer, conn_list[clientfd].r_buffer, conn_list[clientfd].w_length);
#endif

#if ENABLE_THREADPOOL

    // 任务封装
    reactor_task_t* task = (reactor_task_t*)malloc(sizeof(reactor_task_t));
    if(task == NULL){
        printf("malloc task failed.\n");
        exit(1);
    }
    task->task_func = protocol_handle_entry;
    task->user_date = &clientfd;

    struct epoll_event ev = {0};
    set_event(clientfd, 0, 0);

    reactor_threadpool_pushtask(&g_threadpool, task);


#else

        // 后面可以考虑将业务处理交给线程池
    conn_list[clientfd].w_length = g_service_handle(conn_list[clientfd].r_buffer, conn_list[clientfd].r_length, conn_list[clientfd].w_buffer);
    memset(conn_list[clientfd].r_buffer, 0 ,BUFFER_SIZE);

    set_event(clientfd, EPOLLOUT, 0); // 读完关注可写事件

#endif

    return 0;
#endif

}

int send_callback(int clientfd){

#if TCP_PKT_SPLIT_PASTE

    struct conn* conn_state = &conn_list[clientfd];
 
    send_queue_node_t* send_info = send_queue_dequeue(conn_state->send_queue);
    if(send_info == NULL) set_event(clientfd, EPOLLIN, 0);

    else{
        int send_len = send(clientfd, send_info->data, send_info->len, 0);
        if(send_len < 0){
            perror("send");
        }
        free(send_info->data);
        free(send_info);
        if(send_queue_is_empty(conn_state->send_queue) == 0){
            set_event(clientfd, EPOLLIN, 0);
        }
        else{
            set_event(clientfd, EPOLLOUT, 0);
        }
    }

    return 0;
#else

    int send_len = send(clientfd, conn_list[clientfd].w_buffer, conn_list[clientfd].w_length, 0);
    if(send_len < 0){
        perror("send");
    }
    memset(conn_list[clientfd].w_buffer, 0, BUFFER_SIZE);
    set_event(clientfd, EPOLLIN, 0); // 写完关注可读事件

    return 0;

#endif

}



// reactor 入口 -> port service_handle
int reactor_entry(uint16_t port, protocol_handle handle){

    g_service_handle = handle;

    memset(conn_list, 0, sizeof(struct conn) * CONN_LIST_SIZE);

#if ENABLE_THREADPOOL

    reactor_threadpool_init(&g_threadpool, THREADPOOL_WORKER_COUNT);

#endif

    epollfd = epoll_create(1);
    struct epoll_event events[EPOLL_EVENT_SIZE] = {0};

    int listenfd = create_listenfd(port);
    listenfd_event_register(listenfd);

    while(1){
        int nready = epoll_wait(epollfd, events, EPOLL_EVENT_SIZE, -1);
        for(int i = 0; i < nready; ++i){
            int fd = events[i].data.fd;
            if(events[i].events & EPOLLIN){
                conn_list[fd].r_action.recv_cb(fd);
            }
            if(events[i].events & EPOLLOUT){
                conn_list[fd].send_cb(fd);
            }
        }
    }

#if TCP_PKT_SPLIT_PASTE
end:
    for(int fd = 0; fd < CONN_LIST_SIZE; ++fd){
        struct conn* conn_state = &conn_list[fd];
        conn_cleanup(conn_state);
    }
#endif
    return 0;
}