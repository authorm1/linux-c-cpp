#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define MAX_EVENTS 64
#define MAX_BUFFER 1024

static long g_total_responses = 0;
static long g_active_connections = 0;
static int g_test_running = 1;


static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) return -1;
    return 0;
}


static long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}


static void handle_read(int fd, const char* req_buf, size_t req_len) {
    char recv_buf[MAX_BUFFER];
    

    ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);

    if (n > 0) {

        g_total_responses++;
        

        if (g_test_running) {
            if (send(fd, req_buf, req_len, 0) < 0) {
                if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    perror("send error");
                    close(fd);
                    g_active_connections--;
                }
            }
        }
    } else if (n == 0) {
        close(fd);
        g_active_connections--;
    } else {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            close(fd);
            g_active_connections--;
        }
    }
}


static void handle_write(int fd, int epoll_fd, const char* req_buf, size_t req_len) {
    int err = 0;
    socklen_t len = sizeof(err);
    
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        fprintf(stderr, "Connection failed on fd %d\n", fd);
        close(fd);
        return;
    }


    g_active_connections++;


    if (send(fd, req_buf, req_len, 0) < 0) {
        perror("send (first)");
        close(fd);
        g_active_connections--;
        return;
    }

    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <ip> <port> <concurrency> <duration_sec>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 8080 10000 10\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int concurrency = atoi(argv[3]);
    int duration_sec = atoi(argv[4]);

    char request_buf[] = "This is a QPS test request.\n";
    size_t request_len = strlen(request_buf);

    int epoll_fd = epoll_create(1);
    if (epoll_fd < 0) {
        perror("epoll_create");
        return 1;
    }
    struct epoll_event events[MAX_EVENTS];
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_aton(ip, &server_addr.sin_addr) == 0) {
        perror("inet_aton");
        return 1;
    }

    printf("Starting connection ramp-up for %d connections...\n", concurrency);


    for (int i = 0; i < concurrency; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            continue; 
        }
         set_nonblock(sock);
        
        int ret = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        struct epoll_event ev;
        ev.data.fd = sock;

        if (ret == 0) {

            g_active_connections++;

            send(sock, request_buf, request_len, 0);

            ev.events = EPOLLIN;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
            
        } else if (errno == EINPROGRESS) {
            ev.events = EPOLLOUT; 
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
        } else {
            close(sock);
        }
    }

    printf("Ramp-up complete. Starting %d-second QPS test...\n", duration_sec);
    long long start_time = get_time_ms();
    long long end_time = start_time + (duration_sec * 1000);

    while (g_test_running) {
        long long current_time = get_time_ms();
        if (current_time >= end_time) {
            g_test_running = 0;
            break;
        }
        int timeout_ms = (int)(end_time - current_time);
        if (timeout_ms < 0) timeout_ms = 0;
        if (timeout_ms > 100) timeout_ms = 100; 

        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout_ms);
        if (nfds < 0) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                close(fd);
                g_active_connections--;
                continue;
            }
            
            if (events[i].events & EPOLLOUT) {
                handle_write(fd, epoll_fd, request_buf, request_len);
            } 
            
            if (events[i].events & EPOLLIN) {
                handle_read(fd, request_buf, request_len);
            }
        }
    } // end while

    long long actual_duration_ms = get_time_ms() - start_time;
    double duration_s = actual_duration_ms / 1000.0;
    double qps = (double)g_total_responses / duration_s;

    printf("\n--- Test Complete ---\n");
    printf("Concurrency Level:    %ld / %d\n", g_active_connections, concurrency);
    printf("Test Duration:        %.2f sec\n", duration_s);
    printf("Total Responses:      %ld\n", g_total_responses);
    printf("QPS (Queries/Sec):  %.2f\n", qps);

    close(epoll_fd);
    
    return 0;
}