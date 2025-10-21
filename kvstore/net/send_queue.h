#ifndef _SEND_QUEUE_H
#define _SEND_QUEUE_H

#include<stdio.h>

struct node {
    char* data;
    size_t len;      
    struct node *next;   
};

struct queue {
    struct node *head;   
    struct node *tail;    
    unsigned int size;  
};

struct queue* send_queue_create();
void send_queue_destroy(struct queue *q);
int send_queue_enqueue(struct queue *q, char* data, size_t len);
struct node* send_queue_dequeue(struct queue *q);
int send_queue_is_empty(const struct queue *q);
unsigned int send_queue_get_size(const struct queue *q);

#endif