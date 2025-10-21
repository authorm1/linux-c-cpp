#include"../kvstore.h"

int kvs_array_create(kvs_array_t *inst){
    if(inst == NULL) return -1;
    if(inst->arr != NULL){
        printf("kvs_array has existed.\n");
        return -1;
    }

    inst->arr = kvs_malloc(KVS_ARRAY_INIT_SIZE * sizeof(kvs_array_item_t));
    if(inst->arr == NULL) return -2;

    inst->last = 0;
    inst->capacity = KVS_ARRAY_INIT_SIZE;
    return 0;
}

void kvs_array_destroy(kvs_array_t *inst){
    if(inst != NULL && inst->arr != NULL){
        for(int i = 0;i < inst->last; ++i){
            if(inst->arr[i].key == NULL && inst->arr[i].value == NULL) continue;
            else{
                kvs_free(inst->arr[i].key);
                kvs_free(inst->arr[i].value);
            }
        }
        kvs_free(inst->arr);
    }
}

// @return: <0, error; ==0, success; >0, exist
int kvs_array_set(kvs_array_t *inst, char *key, char *value){
    if(inst == NULL || inst->arr == NULL || key == NULL || value == NULL) return -1;
    if(kvs_array_get(inst, key) != NULL) return 1; // this key already exist
    int key_len = strlen(key);
    char* key_copy = kvs_malloc(key_len + 1);
    memset(key_copy, 0, key_len + 1);
    memcpy(key_copy, key, key_len);

    int value_len = strlen(value);
    char* value_copy = kvs_malloc(value_len + 1);
    memset(value_copy, 0, value_len + 1);
    memcpy(value_copy, value, value_len); 

    int i = 0;
    for(;i < inst->last; ++i){
        if(inst->arr[i].key == NULL){
            inst->arr[i].key = key_copy;
            inst->arr[i].value = value_copy;
        }
    }
    
    if(i == inst->last){
        if(inst->last == inst->capacity){ // 扩容
            int new_capacity = 2 * inst->capacity;
            kvs_array_item_t* new_array = kvs_malloc(sizeof(kvs_array_item_t) * new_capacity);
            if(new_array == NULL){
                printf("malloc new_array failed.\n");
                return -1;
            }
            memset(new_array, 0, new_capacity);

            for(int j = 0; j < inst->last; ++j){
                new_array[j].key = inst->arr[j].key;
                new_array[j].value = inst->arr[j].value;
            }
            kvs_free(inst->arr);
            inst->arr = new_array;
            inst->capacity = new_capacity;
        }
        
        inst->arr[i].key = key_copy;
        inst->arr[i].value = value_copy;
        ++inst->last;
    }

    return 0;
}

// @return: NULL, error or not exist; not NULL, exist
char* kvs_array_get(kvs_array_t *inst, char *key){
    if(inst == NULL || inst->arr == NULL || key == NULL) return NULL;
    for(int i = 0;i < inst->last; ++i){
        if(inst->arr[i].key == NULL) continue;

        if(strcmp(inst->arr[i].key, key) == 0){
            if(inst->arr[i].value){
                return inst->arr[i].value;
            }
        }
    }
    return NULL;
}

// @return: <0, error; ==0, success; >0, notexist
int kvs_array_del(kvs_array_t *inst, char *key){
    if(inst == NULL || inst->arr == NULL || key == NULL) return -1;
    for(int i = 0;i < inst->last; ++i){
        if(strcmp(inst->arr[i].key, key) == 0){
            kvs_free(inst->arr[i].key);
            kvs_free(inst->arr[i].value);
            inst->arr[i].key = inst->arr[i].value = NULL;
            
            if(i == inst->last -1) --inst->last;
            return 0;
        }
    }
    return 1;
}

// @return: <0, error; ==0, success; >0, notexist
int kvs_array_mod(kvs_array_t *inst, char *key, char *value){
    if(inst == NULL || inst->arr == NULL || key == NULL || value == NULL) return -1;
    for(int i = 0;i < inst->last; ++i){
        if(strcmp(inst->arr[i].key, key) == 0){
            int value_len = strlen(value);
            char* value_copy = kvs_malloc(value_len + 1);
            if(value_copy == NULL) return -2;
            memset(value_copy, 0 ,value_len + 1);
            memcpy(value_copy, value, value_len);

            kvs_free(inst->arr[i].value);
            inst->arr[i].value = value_copy;
            return 0;
        }
    }

    return 1;
}

//@return: <0, error; ==0, success; >0, notexist
int kvs_array_exist(kvs_array_t *inst, char *key){
    if(inst == NULL || inst->arr == NULL || key == NULL) return -1;
    if(kvs_array_get(inst, key) != NULL) return 0;

    return 1;
}