#include"kvstore.h"

#if (NETWORK_SELECT == USING_REACTOR)
int network_entry(uint16_t port, service_handle protocol_handle){
    return reactor_entry(port, protocol_handle);
}
#elif (NETWORK_SELECT == USING_PROACTOR)
int network_entry(uint16_t port, service_handle protocol_handle){
    return proactor_entry(port, protocol_handle);
}
#endif 

#if (DS_SELECT == USING_ARRAY)

kvs_array_t gloabal_kvs_array = {0};

int kvs_create(void){
    return kvs_array_create(&gloabal_kvs_array);
}

void kvs_destroy(void){
    return kvs_array_destroy(&gloabal_kvs_array);
}

// @return: <0, error; ==0, success; >0, exist
int kvs_set(char *key, char *value){
    return kvs_array_set(&gloabal_kvs_array, key, value);
}

// @return: NULL, error or not exist; not NULL, exist
char* kvs_get(char *key){
    return kvs_array_get(&gloabal_kvs_array, key);
}

// @return: <0, error; ==0, success; >0, notexist
int kvs_del(char *key){
    return kvs_array_del(&gloabal_kvs_array, key);
}

// @return: <0, error; ==0, success; >0, notexist
int kvs_mod(char *key, char *value){
    return kvs_array_mod(&gloabal_kvs_array, key, value);
}

//@return: <0, error; ==0, success; >0, notexist
int kvs_exist(char *key){
    return kvs_array_exist(&gloabal_kvs_array, key);
}


#elif (DS_SELECT == USING_RBTREE)

kvs_rbtree_t global_kvs_rbtree = {0};

int kvs_create(void){
    return kvs_rbtree_create(&global_kvs_rbtree);
}

void kvs_destroy(void){
    return kvs_rbtree_destroy(&global_kvs_rbtree);
}

// @return: <0, error; ==0, success; >0, exist
int kvs_set(char *key, char *value){
    return kvs_rbtree_set(&global_kvs_rbtree, key, value);
}

// @return: NULL, error or not exist; not NULL, exist
char* kvs_get(char *key){
    return kvs_rbtree_get(&global_kvs_rbtree, key);
}

// @return: <0, error; ==0, success; >0, notexist
int kvs_del(char *key){
    return kvs_rbtree_del(&global_kvs_rbtree, key);
}

// @return: <0, error; ==0, success; >0, notexist
int kvs_mod(char *key, char *value){
    return kvs_rbtree_mod(&global_kvs_rbtree, key, value);
}

//@return: <0, error; ==0, success; >0, notexist
int kvs_exist(char *key){
    return kvs_rbtree_exist(&global_kvs_rbtree, key);
}
    


#elif (DS_SELECT == USING_HASNTABLE)

#endif
