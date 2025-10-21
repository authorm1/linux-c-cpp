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

// 全局统计
static long g_total_responses = 0;
static long g_active_connections = 0;
static int g_test_running = 1;

/**
 * @brief 将fd设置为非阻塞
 */
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) return -1;
    return 0;
}

/**
 * @brief 获取当前时间的毫秒数
 */
static long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * @brief 处理EPOLLIN事件（收到响应，发送下一个请求）
 */
static void handle_read(int fd, const char* req_buf, size_t req_len) {
    char recv_buf[MAX_BUFFER];
    
    // 我们使用Level-Triggered，所以只读一次
    ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);

    if (n > 0) {
        // 成功收到响应
        g_total_responses++;
        
        // 立即发送下一个请求
        if (g_test_running) {
            if (send(fd, req_buf, req_len, 0) < 0) {
                if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    perror("send error");
                    close(fd);
                    g_active_connections--;
                }
                // 如果EAGAIN，epoll会因为缓冲区可写而再次触发
                // 但我们这里主要关心读，所以简单处理
            }
        }
    } else if (n == 0) {
        // 服务器关闭了连接
        // printf("Server closed connection on fd %d\n", fd);
        close(fd);
        g_active_connections--;
    } else {
        // 发生错误
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            // perror("recv error");
            close(fd);
            g_active_connections--;
        }
    }
}

/**
 * @brief 处理EPOLLOUT事件（连接成功建立）
 */
static void handle_write(int fd, int epoll_fd, const char* req_buf, size_t req_len) {
    int err = 0;
    socklen_t len = sizeof(err);
    
    // 检查连接是否真的成功
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        fprintf(stderr, "Connection failed on fd %d\n", fd);
        close(fd);
        return;
    }

    // 连接成功
    g_active_connections++;

    // 发送第一个请求
    if (send(fd, req_buf, req_len, 0) < 0) {
        perror("send (first)");
        close(fd);
        g_active_connections--;
        return;
    }

    // 修改epoll注册，现在只关心读事件
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN; // 改为Level-Triggered, 只关心读
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

    // 这是我们要发送的请求，你的服务器会echo它
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

    // --- 阶段 1: 建立连接 ---
    for (int i = 0; i < concurrency; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            continue; // 继续尝试下一个
        }
        
        set_nonblock(sock);
        
        int ret = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        struct epoll_event ev;
        ev.data.fd = sock;

        if (ret == 0) {
            // 立即连接成功 (很少见)
            g_active_connections++;
            // 发送第一个请求
            send(sock, request_buf, request_len, 0);
            // 添加到epoll等待读
            ev.events = EPOLLIN;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
            
        } else if (errno == EINPROGRESS) {
            // 连接正在进行中
            // 添加到epoll，等待EPOLLOUT事件（表示连接完成）
            ev.events = EPOLLOUT; 
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
        } else {
            // 连接出错
            // perror("connect");
            close(sock);
        }
    }

    printf("Ramp-up complete. Starting %d-second QPS test...\n", duration_sec);
    long long start_time = get_time_ms();
    long long end_time = start_time + (duration_sec * 1000);

    // --- 阶段 2: 运行测试 ---
    while (g_test_running) {
        long long current_time = get_time_ms();
        if (current_time >= end_time) {
            g_test_running = 0; // 时间到，停止测试
            break;
        }

        // 设置一个超时，以便能及时检查时间
        int timeout_ms = (int)(end_time - current_time);
        if (timeout_ms < 0) timeout_ms = 0;
        if (timeout_ms > 100) timeout_ms = 100; // 最小100ms超时

        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout_ms);
        if (nfds < 0) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                // 发生错误
                close(fd);
                g_active_connections--;
                continue;
            }
            
            if (events[i].events & EPOLLOUT) {
                // 连接成功建立
                handle_write(fd, epoll_fd, request_buf, request_len);
            } 
            
            if (events[i].events & EPOLLIN) {
                // 收到响应
                handle_read(fd, request_buf, request_len);
            }
        }
    } // end while

    // --- 阶段 3: 报告结果 ---
    long long actual_duration_ms = get_time_ms() - start_time;
    double duration_s = actual_duration_ms / 1000.0;
    double qps = (double)g_total_responses / duration_s;

    printf("\n--- Test Complete ---\n");
    printf("Concurrency Level:    %ld / %d\n", g_active_connections, concurrency);
    printf("Test Duration:        %.2f sec\n", duration_s);
    printf("Total Responses:      %ld\n", g_total_responses);
    printf("QPS (Queries/Sec):  %.2f\n", qps);

    // 清理 (在实际程序中，你应该遍历并关闭所有fd)
    close(epoll_fd);
    
    return 0;
}