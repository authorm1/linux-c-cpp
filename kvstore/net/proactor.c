#define _GNU_SOURCE
#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include<stdlib.h>

#include"proactor.h"

int memmove_num = 0;

struct conn connfd_list[CONNFD_LIST_SIZE] = {0};

int init_server(unsigned short port) {	

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);	
	struct sockaddr_in serveraddr;	
	memset(&serveraddr, 0, sizeof(struct sockaddr_in));	
	serveraddr.sin_family = AF_INET;	
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);	
	serveraddr.sin_port = htons(port);	

	if (-1 == bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {		
		perror("bind");		
		return -1;	
	}	

	listen(sockfd, 10);
	
	return sockfd;
}




int set_event_recv(struct io_uring *ring, int sockfd,
				      void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_READ,
	};
	
	
	io_uring_prep_recv(sqe, sockfd, buf, len, flags); 
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}


int set_event_send(struct io_uring *ring, int sockfd,
				      void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_WRITE,
	};
	
	io_uring_prep_send(sqe, sockfd, buf, len, flags);
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}



int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr,
					socklen_t *addrlen, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring); // 返回下一个可以插入请求的 SQE（Submission Queue Entry）的指针s -> ring 的 tail 指针

	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_ACCEPT,
	};
	
	io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags); // 为此 SQE 填充信息
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}

void cleanup_connection(int fd) {
    if (fd < 0 || fd >= CONNFD_LIST_SIZE) return;


#if TCP_PKT_SPLIT_PASTE
	struct conn* conn_state = &connfd_list[fd];
    if(conn_state == NULL) return;

    free(conn_state->recv_buf);
    while(1){
        send_queue_node_t* send_info = send_queue_dequeue(conn_state->send_queue);
        if(send_info == NULL) break;
        if(send_info->data != NULL) free(send_info->data);
        free(send_info);
    }

    close(fd);
    memset(connfd_list + fd, 0, sizeof(struct conn));

#else

    if (connfd_list[fd].request != NULL) {
        free(connfd_list[fd].request);
        connfd_list[fd].request = NULL;
    }

	if (connfd_list[fd].response != NULL) {
        free(connfd_list[fd].response);
        connfd_list[fd].response = NULL;
    }
#endif
    close(fd);
}

// void* -> 8 bytes	
int proactor_entry(uint16_t port, protocol_handle handle) {
	int sockfd = init_server(port);
	struct io_uring_params params;
	memset(&params, 0, sizeof(params));

	struct io_uring ring;
	io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params); // io_uring 初始化

	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
	

	while (1) {

		io_uring_submit(&ring); // 将当前 SQ 中的 SQE 提交给内核处理


		struct io_uring_cqe *cqe;
		io_uring_wait_cqe(&ring, &cqe); // 

		struct io_uring_cqe *cqes[128];
		int nready = io_uring_peek_batch_cqe(&ring, cqes, 128);  // epoll_wait

		int i = 0;
		for (i = 0;i < nready;i ++) {

			struct io_uring_cqe *entries = cqes[i];
			struct conn_info result;
			memcpy(&result, &entries->user_data, sizeof(struct conn_info));

			if (result.event == EVENT_ACCEPT) {

				set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
#if ENABLE_PRINT
				printf("set_event_accept\n"); //
#endif
				int connfd = entries->res; // accept 返回的结果为新连接的 fd

#if TCP_PKT_SPLIT_PASTE
				// INIT
				struct conn* conn_state = &connfd_list[connfd];
				conn_state->recv_buf = (char*)malloc(RECV_BUFFER_SIZE);
				conn_state->send_queue = send_queue_create();
				if(conn_state->recv_buf == NULL || conn_state->send_queue == NULL){
					printf("malloc recv buf, send queue failed.\n");
					goto end;
				}
				memset(conn_state->recv_buf, 0, RECV_BUFFER_SIZE);
				conn_state->process_pos = conn_state->read_pos = 0;
				conn_state->capacity = RECV_BUFFER_SIZE;
				conn_state->is_sending = 0;
				set_event_recv(&ring, connfd, conn_state->recv_buf + conn_state->read_pos, 
					conn_state->capacity - conn_state->read_pos, 0);
				

#else
				connfd_list[connfd].request = (char*)malloc(BUFFER_LENGTH);
				if(connfd_list[connfd].request == NULL){
					printf("malloc request buf failed.\n");
					goto end;
				}
				memset(connfd_list[connfd].request, 0, BUFFER_LENGTH);

				connfd_list[connfd].response = (char*)malloc(BUFFER_LENGTH);
				if(connfd_list[connfd].response == NULL){
					printf("malloc response buf failed.\n");
					goto end;
				}
				memset(connfd_list[connfd].response, 0, BUFFER_LENGTH);

				set_event_recv(&ring, connfd, connfd_list[connfd].request, BUFFER_LENGTH, 0);
#endif
				
			} else if (result.event == EVENT_READ) {  //

				int length = entries->res;

#if TCP_PKT_SPLIT_PASTE
				if(length < 0){
					cleanup_connection(result.fd);
					perror("recv");
					goto end;
				}
				else if(length == 0){
					cleanup_connection(result.fd);
					continue;
				}

				struct conn* conn_state = &connfd_list[result.fd];
				conn_state->read_pos +=  length;
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
						int send_length = handle(conn_state->recv_buf + conn_state->process_pos, cmd_len, send_data);
						send_queue_enqueue(conn_state->send_queue, send_data, send_length);
						conn_state->process_pos = flag - conn_state->recv_buf + 2;
					}
				}

				int is_empty = send_queue_is_empty(conn_state->send_queue);
				// 
				if (is_empty != 0 && conn_state->is_sending == 0) {
					conn_state->is_sending = 1; // 标记为“正在发送”
					send_queue_node_t* send_info = conn_state->send_queue->head;
					set_event_send(&ring, result.fd, send_info->data, send_info->len, 0);
				} 
				else {
					// 如果发送队列为空，或者已经在发送中，那么就继续提交读请求
					set_event_recv(&ring, result.fd, conn_state->recv_buf + conn_state->read_pos, conn_state->capacity - conn_state->read_pos, 0);
				}
					

#else				
				if (length == 0) {
					free(connfd_list[result.fd].request);
					connfd_list[result.fd].request = NULL;
					free(connfd_list[result.fd].response);
					connfd_list[result.fd].response = NULL;
					close(result.fd);
				} else if (length > 0) { // 读取的数据长度为 ret
					int res_length = handle(connfd_list[result.fd].request, length, connfd_list[result.fd].response); // \r\n
					set_event_send(&ring, result.fd, connfd_list[result.fd].response, res_length, 0);
				}

#endif
			}  else if(result.event == EVENT_WRITE) {
  //

				int ret = entries->res;
#if TCP_PKT_SPLIT_PASTE
				if(ret < 0){
					perror("send");
					goto end;
				}
				struct conn* conn_state = &connfd_list[result.fd];
				if(send_queue_is_empty(conn_state->send_queue) == 0){
					printf("send_queue should be not empty but be empty.\n");
					goto end;
				}

				send_queue_node_t* send_info =  send_queue_dequeue(conn_state->send_queue); // not null
				if(send_info->data != NULL){
					free(send_info->data);
					free(send_info);
				}
				// 检查队列中是否还有待发送的数据
				if (send_queue_is_empty(conn_state->send_queue) == 0) {
					// 队列空了，说明所有待发数据都发出去了
					conn_state->is_sending = 0; // 解除“正在发送”状态
					// 继续提交读请求，接收新数据
					set_event_recv(&ring, result.fd, conn_state->recv_buf + conn_state->read_pos, conn_state->capacity - conn_state->read_pos, 0);
				} else {
					// 队列不为空，继续发送下一个
					// is_sending 状态保持为 1
					send_queue_node_t* next_send_info = conn_state->send_queue->head;
					set_event_send(&ring, result.fd, next_send_info->data, next_send_info->len, 0);
				}
				
#else
				// memset(connfd_list[result.fd].request, 0, BUFFER_LENGTH);
				set_event_recv(&ring, result.fd, connfd_list[result.fd].request, BUFFER_LENGTH, 0);
#endif

				
			}
			
		}

		io_uring_cq_advance(&ring, nready); // 推进 CQ 头指针 -> 将处理完的 CQE 清除
	}

end:
	for(int fd = 0;fd < CONNFD_LIST_SIZE; ++fd){
		
		cleanup_connection(fd);
	}

	return -1;

}


