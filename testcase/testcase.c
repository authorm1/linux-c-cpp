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

#define MAX_EVENTS 1024
#define MAX_BUFFER 1024

// 全局统计
static long g_total_responses = 0;
static long g_active_connections = 0;
static int g_test_running = 1;
static long g_peak_active_connections = 0;


#define MAX_FD_LIMIT 65536 

static unsigned char *conn_counted = NULL;

typedef struct {
    int seq;
    char buf[MAX_BUFFER];
} SockState;

static SockState *sock_states = NULL;


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


static void handle_read(int epoll_fd, int fd, int concurrency, int *all_sockets) {
    char recv_buf[MAX_BUFFER];

    ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
    
    if (fd < 0 || fd >= MAX_FD_LIMIT) return; // 安全检查

    if (n > 0) {

        if (sock_states && sock_states[fd].buf[0]) {
            if (n != (ssize_t)strlen(sock_states[fd].buf) ||
                memcmp(recv_buf, sock_states[fd].buf, n) != 0) {
                fprintf(stderr, "WARNING: response mismatch for fd %d seq %d\n", fd, sock_states[fd].seq);
            }
        }

        // 成功收到响应
        g_total_responses++;

        // 生成并发送下一个请求
        if (g_test_running) {
            sock_states[fd].seq++;
            int len = snprintf(sock_states[fd].buf, MAX_BUFFER, "req:%d:%d\n", fd, sock_states[fd].seq);
            if (len < 0 || len >= MAX_BUFFER) {
                fprintf(stderr, "snprintf error fd %d\n", fd);
                return;
            }
            if (send(fd, sock_states[fd].buf, len, 0) < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    // 发送缓冲区满，切换到EPOLLOUT等待可写
                    struct epoll_event ev;
                    ev.data.fd = fd;
                    ev.events = EPOLLOUT;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                } else {
                     perror("send error (ignored during test)");
                }
            }
            // 如果send成功，我们什么也不做，保持EPOLLIN (LT模式)
            // 等待服务器下一次响应
        }
    } else if (n == 0) {

    } else {
        // 发生错误
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            // recv error, log but keep socket open for test duration
            // perror("recv error (ignored during test)");
        }
    }
}

/**
 * @brief 处理EPOLLOUT事件（连接成功建立或待发送）
 */
static void handle_write(int epoll_fd, int fd, int concurrency, int *all_sockets) {
    // 检查非阻塞connect完成状态
    int err = 0;
    socklen_t len = sizeof(err);
    int gs = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);

    if (gs < 0 || err != 0) {
        // 连接失败
        fprintf(stderr, "Connection failed on fd %d (getsockopt=%d err=%d)\n", fd, gs, err);
        if (fd >= 0 && fd < MAX_FD_LIMIT && conn_counted && conn_counted[fd]) {
            conn_counted[fd] = 0;
            if (g_active_connections > 0) g_active_connections--;
        }
        close(fd);
        return;
    }
    

    if (fd < 0 || fd >= MAX_FD_LIMIT) return; // 安全检查

    // 检查是否是第一次（即连接刚建立）
    if (fd >= 0 && fd < MAX_FD_LIMIT && conn_counted && !conn_counted[fd]) {
        conn_counted[fd] = 1;
        g_active_connections++;
        
        // [FIX 3] 修复 peak_active_connections 统计的 typo
        if (g_active_connections > g_peak_active_connections) {
             g_peak_active_connections = g_active_connections;
        }

        // 生成第一个请求
        sock_states[fd].seq = 1;
        int req_len = snprintf(sock_states[fd].buf, MAX_BUFFER, "req:%d:%d\n", fd, sock_states[fd].seq);
        if (req_len < 0 || req_len >= MAX_BUFFER) {
            fprintf(stderr, "snprintf error fd %d\n", fd);
            close(fd);
            return;
        }
    }

    // 发送当前请求 (无论是第一个请求，还是之前EAGAIN的重试)
    int s_len = strlen(sock_states[fd].buf);
    if (s_len == 0) {
        // 缓冲区中没有要发送的内容 (可能是一个逻辑错误，或者刚send完)
        // 切换到读取模式
        struct epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        return;
    }
    
    if (send(fd, sock_states[fd].buf, s_len, 0) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 缓冲区满，保持EPOLLOUT，等待下次可写
            struct epoll_event ev;
            ev.data.fd = fd;
            ev.events = EPOLLOUT;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
            return;
        } else {
            perror("send (retry)");
            if (fd >= 0 && fd < MAX_FD_LIMIT && conn_counted && conn_counted[fd]) {
                conn_counted[fd] = 0;
                if (g_active_connections > 0) g_active_connections--;
            }
            close(fd);
            return;
        }
    }

    // 发送成功，转为读取模式
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

    conn_counted = malloc(MAX_FD_LIMIT);
    if (!conn_counted) {
        perror("malloc conn_counted");
        return 1;
    }
    memset(conn_counted, 0, MAX_FD_LIMIT);

    int *all_sockets = malloc(sizeof(int) * concurrency);
    if (!all_sockets) {
        perror("malloc all_sockets");
        free(conn_counted);
        return 1;
    }
    for (int ai = 0; ai < concurrency; ++ai) all_sockets[ai] = -1;

    sock_states = malloc(sizeof(SockState) * MAX_FD_LIMIT);
    if (!sock_states) {
        perror("malloc sock_states");
        free(all_sockets);
        free(conn_counted);
        return 1;
    }

    memset(sock_states, 0, sizeof(SockState) * MAX_FD_LIMIT);


    // --- 阶段 1: 建立连接 ---
    for (int i = 0; i < concurrency; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            continue; // 继续尝试下一个
        }
        if (sock >= MAX_FD_LIMIT) {
             fprintf(stderr, "Socket fd %d exceeds MAX_FD_LIMIT %d\n", sock, MAX_FD_LIMIT);
             close(sock);
             continue;
        }
        
        all_sockets[i] = sock;
        
        set_nonblock(sock);
        
        int ret = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        struct epoll_event ev;
        ev.data.fd = sock;

        if (ret == 0) {
            // 立即连接成功
            ev.events = EPOLLOUT;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
        } else if (errno == EINPROGRESS) {
            // 连接正在进行中
            ev.events = EPOLLOUT;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
        } else {
            // 连接出错
            // perror("connect");
            close(sock);
            all_sockets[i] = -1;
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

        int timeout_ms = (int)(end_time - current_time);
        if (timeout_ms < 0) timeout_ms = 0;
        if (timeout_ms > 100) timeout_ms = 100; // 最小100ms超时

        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout_ms);
        if (nfds < 0) {
            if (errno == EINTR) continue; 
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            
            if (fd < 0 || fd >= MAX_FD_LIMIT) continue;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {

                 epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                 if (conn_counted[fd]) {
                     conn_counted[fd] = 0;
                     if (g_active_connections > 0) g_active_connections--;
                 }
                 close(fd);
                 continue;
            }
            
            if (events[i].events & EPOLLOUT) {
                // 连接成功建立 或 缓冲区可写
                handle_write(epoll_fd, fd, concurrency, all_sockets);
            }

            if (events[i].events & EPOLLIN) {
                // 收到响应
                handle_read(epoll_fd, fd, concurrency, all_sockets);
            }
        }
    } 

    // --- 阶段 3: 报告结果 ---
    long long actual_duration_ms = get_time_ms() - start_time;
    double duration_s = actual_duration_ms / 1000.0;
    
    // 避免除以0
    double qps = 0;
    if (duration_s > 0) {
        qps = (double)g_total_responses / duration_s;
    }


    printf("\n--- Test Complete ---\n");
    printf("Concurrency Level:    %ld / %d\n", g_active_connections, concurrency);
    printf("Peak Concurrency:     %ld\n", g_peak_active_connections);
    printf("Test Duration:        %.2f sec\n", duration_s);
    printf("Total Responses:      %ld\n", g_total_responses);
    printf("QPS (Queries/Sec):  %.2f\n", qps);

    // --- 阶段 4: 清理 ---
    for (int ai = 0; ai < concurrency; ++ai) {
        if (all_sockets && all_sockets[ai] >= 0) {
            // 从epoll中移除(如果还在的话)
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, all_sockets[ai], NULL);
            close(all_sockets[ai]);
        }
    }
    free(all_sockets);
    free(conn_counted);
    free(sock_states);

    close(epoll_fd);
    
    return 0;
}
