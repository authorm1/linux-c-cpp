
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/epoll.h>

#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

#define BUFFER_SIZE                 1024
#define CONN_LIST_SIZE              1024 * 1024
#define EPOLL_EVENT_SIZE            1024

#define ENABLE_SERVICE_SIMULATION   1

typedef int(*RCALLBACK)(int);

struct conn{
    int fd;

    char r_buffer[BUFFER_SIZE];
    char w_buffer[BUFFER_SIZE];
    long r_length;
    long w_length;

    union{
        RCALLBACK accept_cb;
        RCALLBACK recv_cb;
    }r_action;
    RCALLBACK send_cb;

    int status;
};


int accept_callback(int fd);
int recv_callback(int fd);
int send_callback(int fd);

int epollfd = 0;
struct conn conn_list[CONN_LIST_SIZE] = {0};

int create_listenfd(char* port){
    if(port == NULL) return -1;

    int socketfd = socket(AF_INET, SOCK_STREAM, 0);
    if(socket < 0) return -2;

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

    set_event(clientfd, EPOLLIN, 1); // 可读
    return 0;
}

int accept_callback(int listenfd){
    struct sockaddr_in client_addr = {0};
    socklen_t len = 0;
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
        perror("recv");
        close(clientfd);
        return -1;
    }
    else if(recv_len == 0){
        close(clientfd);
        return 0;
    }
    conn_list[clientfd].r_length = recv_len;

    conn_list[clientfd].w_length = conn_list[clientfd].r_length;
#if ENABLE_SERVICE_SIMULATION
    #include<time.h>

    struct timespec sleep_time = {0};
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 10 * 1000 * 1000; // 10ms

    nanosleep(&sleep_time, NULL);

    for(int i = 0; i < 1000; ++i);    

#endif
    memcpy(conn_list[clientfd].w_buffer, conn_list[clientfd].r_buffer, conn_list[clientfd].w_length);

    memset(conn_list[clientfd].r_buffer, 0 ,BUFFER_SIZE);

    set_event(clientfd, EPOLLOUT, 0); // 读完关注可写事件

    return 0;
}

int send_callback(int clientfd){
  
    int send_len = send(clientfd, conn_list[clientfd].w_buffer, conn_list[clientfd].w_length, 0);
    if(send_len < 0){
        perror("send");
    }
    memset(conn_list[clientfd].w_buffer, 0, BUFFER_SIZE);
    set_event(clientfd, EPOLLIN, 0); // 写完关注可读事件


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
    return 0;
}