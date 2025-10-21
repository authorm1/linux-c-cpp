
cmake_minimum_required(VERSION 3.10)

# 项目名称
project(kvs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SOURCES
    kvstore/kvstore.c
    kvstore/net/proactor.c
    kvstore/net/reactor.c
    kvstore/net/send_queue.c
    kvstore/kvs_hook.c
    kvstore/store_structure/kvs_array.c
    kvstore/store_structure/kvs_rbtree.c
)

set(LIBS
    uring  # 对应 -luring
)

# 添加可执行文件
add_executable(kvs ${SOURCES})

# 链接库
target_link_libraries(kvs ${LIBS})
