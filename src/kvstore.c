#include "kvstore.h"

const char* kvs_command_list[]={
    "SET", "GET", "DEL", "MOD", "EXIST"
};

enum{
    KVS_CMD_START = 0,
    KVS_CMD_SET = KVS_CMD_START,
    KVS_CMD_GET,
    KVS_CMD_DEL,
    KVS_CMD_MOD,
    KVS_CMD_EXIST,
    KVS_CMD_COUNT
};

void* kvs_malloc(size_t size){
    return malloc(size);
}
void kvs_free(void* ptr){
    return free(ptr);
}

int kvs_split_token(char* request, int length,char* tokens[]){
    if(request == NULL || length <= 0 ||tokens == NULL) return -1;
    if(length < 2){
        printf("request length less than 2.\n");
        exit(1);
    }
    request[length - 2] = 0; // \r -> \0
    int index = 0;
    // 将找到的分隔符字符替换为 '\0’，从而将原字符串“切割”成多个小段，并且以原本的末尾'\0'作为结束
    tokens[index] = strtok(request, " ");
    while(tokens[index] != NULL){
        tokens[++index] = strtok(NULL, " ");
    } 

    return index;
}

// 处理 kvs 协议/业务
int kvs_protocol(char* request, int length, char* response){
    if(request == NULL || length <= 0 || response == NULL) return -1;

    char* tokens[KVS_MAX_TOKEN] = {0};
    if(kvs_split_token(request, length, tokens) <= 0) return -1;

    int cmd = 0;
    for(cmd = KVS_CMD_START; cmd < KVS_CMD_COUNT; ++cmd){
        if(strcmp(kvs_command_list[cmd], tokens[0]) == 0){
            break;
        }
    }
    if(cmd == KVS_CMD_COUNT) return -2;

    int res_length = 0;
    char* key = tokens[1];
    char* value = tokens[2];
    switch(cmd){
        case KVS_CMD_SET:{
            int ret = kvs_set(key, value);
            if (ret < 0) {
                res_length = sprintf(response, "ERROR\r\n");
            } 
            else if (ret == 0) {
                res_length = sprintf(response, "OK\r\n");
            } 
            else {
                res_length = sprintf(response, "EXIST\r\n");
            } 
            break;
        }
        case KVS_CMD_GET:{
            char* ret = kvs_get(key);
            if(ret == NULL){
                res_length = sprintf(response, "NO EXIST\r\n");
            }
            else{
                res_length = sprintf(response, "%s\r\n", ret);
            }
            break;
        }
        case KVS_CMD_DEL:{
            int ret = kvs_del(key);
            if (ret < 0) {
                res_length = sprintf(response, "ERROR\r\n");
            } 
            else if (ret == 0) {
                res_length = sprintf(response, "OK\r\n");
            } 
            else {
                res_length = sprintf(response, "NO EXIST\r\n");
            } 
            break;
        }
        case KVS_CMD_MOD:{
            int ret = kvs_mod(key, value);
            if (ret < 0) {
                res_length = sprintf(response, "ERROR\r\n");
            } 
            else if (ret == 0) {
                res_length = sprintf(response, "OK\r\n");
            } 
            else {
                res_length = sprintf(response, "NO EXIST\r\n");
            } 
            break;
        }
        case KVS_CMD_EXIST:{
            int ret = kvs_exist(key);
            if (ret < 0) {
                res_length = sprintf(response, "ERROR\r\n");
            } 
            else if (ret == 0) {
                res_length = sprintf(response, "EXIST\r\n");
            } 
            else {
                res_length = sprintf(response, "NO EXIST\r\n");
            } 
            break;
        }
        default:
            break;
    }


    return res_length;
}


int main(int argc, char* argv[]){
    if(argc != 2){
        printf("incorrect params.\n");
        return -1;
    }

    kvs_create();

    network_entry(atoi(argv[1]), kvs_protocol);

    kvs_destroy();

    return 0;
}