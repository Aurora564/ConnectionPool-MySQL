cmake_minimum_required(VERSION 3.10)
project(MySQLConnectionPool)

# 复制 mysql.cnf 到 build 目录
configure_file(
    ${CMAKE_SOURCE_DIR}/sources/mysql.cnf
    ${CMAKE_BINARY_DIR}/mysql.cnf
    COPYONLY
)


# 查找 MySQL
set(MYSQL_INCLUDE_DIR "/usr/include/mysql")      # 头文件路径
set(MYSQL_LIBRARY "/usr/lib/x86_64-linux-gnu/libmysqlclient.so")  # 库文件路径

include_directories(
    ${MYSQL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/header
)

# 编译可执行文件
add_executable(connection_pool
    sources/CommonConnectionPool.cpp
    sources/Connection.cpp
    sources/main.cpp
)

# 链接 MySQL 库
target_link_libraries(connection_pool ${MYSQL_LIBRARY})
