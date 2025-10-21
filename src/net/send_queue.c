#include <stdlib.h>
#include <stdio.h>
#include "send_queue.h"



struct queue* send_queue_create() {
    struct queue *q = (struct queue*)malloc(sizeof(struct queue));
    if (!q) {
        perror("Failed to allocate memory for queue");
        return NULL;
    }
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    return q;
}

void send_queue_destroy(struct queue *q) {
    if (!q) return;

    struct node *current = q->head;
    while (current != NULL) {
        struct node *next = current->next;
        // 注意：我们只释放队列节点本身。
        // current->data 的内存由创建它的用户负责。
        free(current);
        current = next;
    }
    free(q);
}

int send_queue_enqueue(struct queue *q, char* data, size_t len) {
    if (!q) return -1;

    struct node *newNode = (struct node*)malloc(sizeof(struct node));
    if (!newNode) {
        perror("Failed to allocate memory for queue node");
        return -1;
    }
    newNode->data = data;
    newNode->len = len;
    newNode->next = NULL;
    if (q->tail == NULL) { // 如果队列为空
        q->head = newNode;
        q->tail = newNode;
    } else { // 如果队列不为空
        q->tail->next = newNode;
        q->tail = newNode;
    }

    q->size++;
    return 0;
}

// @return: return nodeToRemove
struct node* send_queue_dequeue(struct queue *q) {
    if (!q || q->head == NULL) { // 检查队列是否为空
        return NULL;
    }

    struct node *nodeToRemove = q->head;
    q->head = q->head->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    q->size--;
    return nodeToRemove;
}

// @return: 0, empty; !0, not empty
int send_queue_is_empty(const struct queue* q) {
    if (!q) return 0;
    return q->size;
}

unsigned int send_queue_get_size(const struct queue *q) {
    if (!q) return 0;
    return q->size;
}
