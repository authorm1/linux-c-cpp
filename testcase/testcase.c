
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>


#define MAX_MSG_LENGTH		1024
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)


int send_msg(int connfd, char *msg, int length) {

	int res = send(connfd, msg, length, 0);
	if (res < 0) {
		perror("send");
		exit(1);
	}
	return res;
}

int recv_msg(int connfd, char *msg, int length) {

	int res = recv(connfd, msg, length, 0);
	if (res < 0) {
		perror("recv");
		exit(1);
	}
	return res;

}


void testcase(int connfd, char *msg, char *pattern, char *casename) {

	if (!msg || !pattern || !casename) return ;

	send_msg(connfd, msg, strlen(msg));

	char result[MAX_MSG_LENGTH] = {0};
	recv_msg(connfd, result, MAX_MSG_LENGTH);

	if (strcmp(result, pattern) == 0) {
		// printf("==> PASS ->  %s\n", casename);
	} else {
		printf("==> FAILED -> %s, '%s' != '%s' \n", casename, result, pattern);
		exit(1);
	}

}

int connect_tcpserver(const char *ip, unsigned short port) {

	int connfd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(struct sockaddr_in));

	server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &server_addr.sin_addr.s_addr);
	server_addr.sin_port = htons(port);

	if (0 !=  connect(connfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in))) {
		perror("connect");
		return -1;
	}
	
	return connfd;
	
}

void kvs_test_10w(int connfd){

    struct timeval tv_beg = {0};
    gettimeofday(&tv_beg, NULL);

    for(int i = 0; i < 10000; ++i){
        testcase(connfd, "SET 1 111\r\n", "OK\r\n", "SET-1");
        testcase(connfd, "GET 1\r\n", "111\r\n", "GET-1");
        testcase(connfd, "MOD 1 222\r\n", "OK\r\n", "MOD-1");
        testcase(connfd, "GET 1\r\n", "222\r\n", "GET-1");
        testcase(connfd, "EXIST 1\r\n", "EXIST\r\n", "EXIST-1");
        testcase(connfd, "DEL 1\r\n", "OK\r\n", "DEL-1");
        testcase(connfd, "DEL 1\r\n", "NO EXIST\r\n", "DEL-1");
        testcase(connfd, "GET 1\r\n", "NO EXIST\r\n", "GET-1");
        testcase(connfd, "MOD 1 333\r\n", "NO EXIST\r\n", "MOD-1");
        testcase(connfd, "EXIST 1\r\n", "NO EXIST\r\n", "GET-1");
    }

    struct timeval tv_end = {0};
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_beg);
    printf("request 10w, time used: %d, qps: %d\n", time_used, 100000 * 1000 / time_used);

}

// ./testcase 192.168.181.128 8888
int main(int argc, char *argv[]) {
	if (argc != 3) {
		printf("arg error\n");
		return -1;
	}

	char *ip = argv[1];
	int port = atoi(argv[2]);
	// int mode = atoi(argv[3]);

	int connfd = connect_tcpserver(ip, port);
    printf("connect success.\n");

    kvs_test_10w(connfd);
#if 0
	if (mode == 0) {
		rbtree_testcase_1w(connfd);
	} else if (mode == 1) {
		rbtree_testcase_3w(connfd);
	} else if (mode == 2) {
		array_testcase_1w(connfd);
	} else if (mode == 3) {
		hash_testcase(connfd);
	}
#endif
	return 0;
	
}