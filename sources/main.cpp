#include <iostream>
using namespace std;
#include "Connection.h"
#include "CommonConnectionPool.h"

int main() {
    // 获取连接池实例（会自动加载 mysql.cnf）
    ConnectionPool *pool = ConnectionPool::getConnectionPool();

    // 测试多线程使用连接池
    thread t1([pool]() {
        for (int i = 0; i < 2500; ++i) {
            shared_ptr<Connection> conn = pool->getConnection();
            char sql[1024] = {0};
            sprintf(sql, "insert into user(name,age,sex) values('%s',%d,'%s')",
                "zhang san", 20, "male");
            conn->update(sql);
        }
    });

    thread t2([pool]() {
        for (int i = 0; i < 2500; ++i) {
            shared_ptr<Connection> conn = pool->getConnection();
            char sql[1024] = {0};
            sprintf(sql, "insert into user(name,age,sex) values('%s',%d,'%s')",
                "zhang san", 20, "male");
            conn->update(sql);
        }
    });

    t1.join();
    t2.join();

    return 0;
}
