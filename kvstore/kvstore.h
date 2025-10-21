#ifndef _KVSTORE_H
#define _KVSTORE_H

#include<stdio.h>
#include<stdint.h>
#include<string.h>
#include<stdlib.h>
#include<pthread.h>


#define USING_REACTOR   0
#define USING_PROACTOR  1
#define NETWORK_SELECT USING_PROACTOR

#define USING_ARRAY 0
#define USING_RBTREE 1
#define USING_HASHTABLE 2
#define DS_SELECT USING_RBTREE


#define KVS_MAX_TOKEN 128   

typedef int(*service_handle)(char* request, int length, char* response);

extern int reactor_entry(uint16_t port, service_handle handle);
extern int proactor_entry(uint16_t port, service_handle handle);

// \r\n 是为了解决 TCP 分包/粘包
/*
* SET KEY VALUE\r\n -> OK\r\n
* GET KEY\r\n -> VALUE\r\n
* DEL KEY\r\n -> OK\r\n
* MOD KEY VALUE\r\n -> OK\r\n
* EXIST KEY\r\n -> YES\r\n
*/

int network_entry(uint16_t port, service_handle protocol_handle);

int kvs_create(void);
void kvs_destroy(void);

int kvs_set(char *key, char *value);
char* kvs_get(char *key);
int kvs_del(char *key);
int kvs_mod(char *key, char *value);
int kvs_exist(char *key);

void* kvs_malloc(size_t size);
void kvs_free(void* ptr);



typedef struct kvs_array_item_s{
    char* key;
    char* value;
}kvs_array_item_t;

#define KVS_ARRAY_INIT_SIZE  1024

typedef struct kvs_array_s{
    kvs_array_item_t* arr;
    int last; // 用到的最后位置加 1
	int capacity;
}kvs_array_t;

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destroy(kvs_array_t *inst);

int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);




enum{
    RED,
    BLACK
};

#define ENABLE_KEY_CHAR   1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE;
#endif

typedef struct _rbtree_node {
	unsigned char color;
	struct _rbtree_node *right;
	struct _rbtree_node *left;
	struct _rbtree_node *parent;
	KEY_TYPE key;
	void *value;
} rbtree_node;

typedef struct _rbtree {
	rbtree_node *root;
	rbtree_node *nil;
} rbtree;

typedef struct _rbtree kvs_rbtree_t;

int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destroy(kvs_rbtree_t *inst);

int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key);


#endif