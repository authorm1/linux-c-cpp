
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/epoll.h>

#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<fcntl.h>
#include<errno.h>


#define BUFFER_SIZE         1024
#define EPOLL_EVENT_SIZE    1024
#define MAX_FDS             1024 * 128


#define HTTP 1

/*
* wrk -t2 -c10000 -d10s -> QPS = 123k
*/

struct conn;
typedef int(*RCALLBACK)(struct conn*);
typedef int(*WCALLBACK)(struct conn*);

// conn_state_t与fd在epoll中是一对一的关系
typedef struct conn{
    int fd;

    char r_buffer[BUFFER_SIZE];
    char w_buffer[BUFFER_SIZE];
    int r_length;
    int w_length;
    int w_offset; // 分段发送

    RCALLBACK r_cb;
    WCALLBACK w_cb;

    int current_events; 

}conn_state_t;

static conn_state_t conn_pool[MAX_FDS];

enum set_event_flags{
    FLAG_MOD,
    FLAG_ADD,
    FLAG_DEL,
};

int epollfd = 0;



int accept_callback(conn_state_t* conn_state);
int recv_callback(conn_state_t* conn_state);
int send_callback(conn_state_t* conn_state);

int set_nonblock(int fd);
int create_listenfd(char* port);
int listenfd_event_register(int listenfd);
int clientfd_event_register(int clientfd);
conn_state_t* create_connstate(int fd, RCALLBACK r_callback, WCALLBACK w_callback);
int destroy_connstate(conn_state_t* conn_state);
int set_event(conn_state_t* conn_state, int event, int flag);



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

int set_event(conn_state_t* conn_state, int event,int flag){
    if(conn_state == NULL) return -1;
    struct epoll_event ev = {0};

    ev.data.ptr = (void*)conn_state;
    ev.events = event;

    if(flag == FLAG_ADD){
        if(conn_state->current_events != 0){
            return 0;
        }
        if(epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_state->fd, &ev) < 0){
             return -1;
        }
        conn_state->current_events = event;
    }
    else if(flag == FLAG_MOD){
        if(conn_state->current_events == event){ // 如果和之前关注的事件一样，就不要再系统调用了
            return 0;
        }
        if(conn_state->current_events == 0){
            /* not registered yet -> add */
            if(epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_state->fd, &ev) < 0){
                return -1;
            }
        } else {
            if(epoll_ctl(epollfd, EPOLL_CTL_MOD, conn_state->fd, &ev) < 0){
                return -1;
            }
        }
        conn_state->current_events = event;
    }
    else if(flag == FLAG_DEL){
        if(conn_state->current_events == 0){ // 如果已经被删除了，就不要再系统调用了 
            return 0;
        }
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, conn_state->fd, &ev) < 0){
            return -1;
        }
        conn_state->current_events = 0;
    }
    return 0;
}

int listenfd_event_register(int listenfd){ //
    if(listenfd < 0) return -1;
    conn_state_t* conn_state = create_connstate(listenfd, accept_callback, NULL);
    if(conn_state == NULL){
        return -1;
    }

    set_event(conn_state, EPOLLIN, FLAG_ADD); // 可读 边缘触发
    return 0;
}

int clientfd_event_register(int clientfd){
    if(clientfd < 0) return -1;
    conn_state_t* conn_state = create_connstate(clientfd, recv_callback, send_callback);
    if(conn_state == NULL) return -2;

    if(set_event(conn_state, EPOLLIN, FLAG_ADD) < 0){
        printf("set_event failed. closing clientfd=%d\n", clientfd);
        destroy_connstate(conn_state);
        return -3;
    }
    return 0;
}

int accept_callback(conn_state_t* conn_state){
    struct sockaddr_in client_addr = {0};
    socklen_t len = 0;
    int clientfd = accept(conn_state->fd, (struct sockaddr*)&client_addr, &len);
    if(clientfd == -1){
        perror("accept");
        return -1;
    }

    // printf("create clientfd: %d\n", clientfd);
    clientfd_event_register(clientfd);
    return 0;

}

int recv_callback(conn_state_t* conn_state) {
    int recv_len = recv(conn_state->fd, conn_state->r_buffer, BUFFER_SIZE, 0);
    if (recv_len < 0) {
        destroy_connstate(conn_state);
        return -1;
    } else if (recv_len == 0) {
        destroy_connstate(conn_state);
        return 0;
    }

    conn_state->r_length = recv_len;
#if HTTP
    // 简单判断是否是 GET 请求（可扩展）
    if (strncmp(conn_state->r_buffer, "GET ", 4) == 0) {
        // 构造标准 HTTP 响应
        const char *http_response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 12\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "Hello World\n";

        // 复制到写缓冲区
        memcpy(conn_state->w_buffer, http_response, BUFFER_SIZE);
        conn_state->w_length = strlen(http_response);

        // 清空读缓冲区
        memset(conn_state->r_buffer, 0, BUFFER_SIZE);

        // 切换关注可写事件
        set_event(conn_state, EPOLLOUT, FLAG_MOD); // MOD
    } else {
        // 不支持的请求
        const char *bad = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        strncpy(conn_state->w_buffer, bad, BUFFER_SIZE);
        conn_state->w_length = strlen(bad);
        set_event(conn_state, EPOLLOUT, FLAG_MOD);
    }
#else
    memcpy(conn_list[clientfd].w_buffer, conn_list[clientfd].r_buffer, conn_list[clientfd].r_length);
    conn_list[clientfd].w_length = conn_list[clientfd].r_length;
    set_event(clientfd, EPOLLOUT, 0);
#endif
    return 0;
}


int send_callback(conn_state_t* conn_state){

    int send_len = send(conn_state->fd, conn_state->w_buffer, conn_state->w_length, 0);
    if(send_len < 0){
        // perror("send");
        destroy_connstate(conn_state);
    }
    memset(conn_state->w_buffer, 0, BUFFER_SIZE);
    set_event(conn_state, EPOLLIN, FLAG_MOD); // 写完关注可读事件

    return 0;
}

conn_state_t* create_connstate(int fd, RCALLBACK r_callback, WCALLBACK w_callback){
    if(fd < 0 || fd >= MAX_FDS){
        printf("fd out of pool range: %d\n", fd);
        return NULL;
    }
    conn_state_t* conn_state = &conn_pool[fd]; // 根据fd作为索引

    conn_state->fd = fd;
    conn_state->r_cb = r_callback;
    conn_state->w_cb = w_callback;
    conn_state->r_length = 0;
    conn_state->w_length = 0;
    conn_state->w_offset = 0;
    conn_state->current_events = 0;
    memset(conn_state->r_buffer, 0, BUFFER_SIZE);
    memset(conn_state->w_buffer, 0, BUFFER_SIZE);
    return conn_state;
}

int destroy_connstate(conn_state_t* conn_state){
    if(conn_state != NULL){
        set_event(conn_state, 0, FLAG_DEL);
        if(conn_state->fd >= 0){
            close(conn_state->fd);
        }
        conn_state->current_events = 0;

        conn_state->fd = -1;
        conn_state->r_length = conn_state->w_length = conn_state->w_offset = 0;
        memset(conn_state->r_buffer, 0, BUFFER_SIZE);
        memset(conn_state->w_buffer, 0, BUFFER_SIZE);
    }
    return 0;
}

int set_nonblock(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1){
        perror("fcntl F_GETFL");
        return -1;
    }

    flags |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flags) == -1){
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

int main(int argc, char** argv){
    if(argc < 2){
        printf("args not enough.\n");
        return -1;
    }

    epollfd = epoll_create(1);
    struct epoll_event events[EPOLL_EVENT_SIZE] = {0};

    int listenfd = create_listenfd(argv[1]);
    if(set_nonblock(listenfd) == -1){
        printf("set listenfd nonblock failed.\n");
        return -1;
    }
    if(listenfd_event_register(listenfd) < 0){
        printf("listenfd_event_register failed.\n");
        return -1;
    }

    while(1){
        int nready = epoll_wait(epollfd, events, EPOLL_EVENT_SIZE, -1);
        if(nready < 0){
            perror("epoll_wait");
            continue;
        }
        for(int i = 0; i < nready; ++i){
            // struct epoll_event的data字段是一个union类型，同一时刻只能使用一个字段，这里一直使用ptr
            conn_state_t* conn_state = (conn_state_t*)events[i].data.ptr;
            if(events[i].events & EPOLLIN){
                conn_state->r_cb(conn_state);
            }
            if(events[i].events & EPOLLOUT){
                conn_state->w_cb(conn_state);
            }
        }
    }
    return 0;
}