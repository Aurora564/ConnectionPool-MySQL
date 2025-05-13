#include <iostream>
#include "Connection.h"
#include "CommonConnectionPool.h"
#include "public.h"
#include <chrono>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdlib>
using namespace std;

#define DEBUG_CONNECTION 0
#define DEBUG_POOL 1

#if DEBUG_POOL

// 测试获取连接的基本功能
void testBasic() {
    ConnectionPool* pool = ConnectionPool::getConnectionPool();
    auto conn = pool->getConnection();
    if (conn) {
        LOG("获取连接成功");
        
        // 测试查询
        MYSQL_RES* res = conn->query("show databases;");
        if (res) {
            LOG("查询成功，显示数据库:");
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                cout << row[0] << endl;
            }
            mysql_free_result(res);
        } else {
            LOG("查询失败");
        }
    } else {
        LOG("获取连接失败");
    }
    
    pool->printStats();
}

// 测试多线程环境下连接池的表现
void testMultiThread() {
    ConnectionPool* pool = ConnectionPool::getConnectionPool();
    const int threadCount = 10;
    vector<thread> threads;
    
    auto task = [pool](int id) {
        auto conn = pool->getConnection();
        if (conn) {
            LOG("线程" << id << "获取连接成功");
            
            // 模拟工作
            this_thread::sleep_for(chrono::milliseconds(100));
            
            // 执行简单查询
            string sql = "select * from user where id = " + to_string(id);
            MYSQL_RES* res = conn->query(sql.c_str());
            if (res) {
                mysql_free_result(res);
            }
            
            LOG("线程" << id << "工作完成");
        } else {
            LOG("线程" << id << "获取连接失败");
        }
    };
    
    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back(task, i + 1);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    pool->printStats();
}

// 测试连接回收功能
void testConnectionRecycling() {
    ConnectionPool* pool = ConnectionPool::getConnectionPool();
    
    // 获取并立即释放多个连接
    vector<shared_ptr<Connection>> conns;
    for (int i = 0; i < 5; ++i) {
        conns.push_back(pool->getConnection());
    }
    
    LOG("获取5个连接后:");
    pool->printStats();
    
    // 释放连接
    conns.clear();
    LOG("释放5个连接后:");
    pool->printStats();
    
    // 等待一段时间让回收线程工作
    this_thread::sleep_for(chrono::seconds(3));
    LOG("等待3秒后:");
    pool->printStats();
}

void testMultiThreadEnhanced() {
    ConnectionPool* pool = ConnectionPool::getConnectionPool();
    const int threadCount = 10;
    vector<thread> threads;

    auto task = [pool](int id) {
        try {
            auto conn = pool->getConnection();
            if (!conn) {
                LOG("线程" << id << " 获取连接失败");
                return;
            }

            string tableName = "test_table_" + to_string(id);

            // 1. 创建表
            string createSql = "CREATE TABLE IF NOT EXISTS " + tableName +
                " (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50));";
            if (!conn->update(createSql)) {
                LOG("线程" << id << " 创建表失败");
                return;
            }

            // 2. 插入数据
            for (int i = 0; i < 3; ++i) {
                string insertSql = "INSERT INTO " + tableName + " (name) VALUES ('thread_" +
                                   to_string(id) + "_entry_" + to_string(i) + "');";
                conn->update(insertSql);
            }

            // 3. 查询数据
            string querySql = "SELECT * FROM " + tableName + ";";
            MYSQL_RES* res = conn->query(querySql);
            if (res) {
                MYSQL_ROW row;
                LOG("线程" << id << " 查询结果:");
                while ((row = mysql_fetch_row(res))) {
                    cout << "线程" << id << " -> id: " << row[0] << ", name: " << row[1] << endl;
                }
                mysql_free_result(res);
            } else {
                LOG("线程" << id << " 查询失败");
            }

            // 4. 删除表
            string dropSql = "DROP TABLE IF EXISTS " + tableName + ";";
            conn->update(dropSql);

            LOG("线程" << id << " 完成所有数据库操作");
        } catch (const exception& ex) {
            LOG("线程" << id << " 异常: " << ex.what());
        }
    };

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back(task, i + 1);
    }

    for (auto& t : threads) {
        t.join();
    }

    LOG("多线程功能测试完成");
    pool->printStats();
}



// 并发压力测试（可传参控制线程数量）
#include <atomic>
#include <chrono>

// 并发压力测试：每个线程插入 N 次
void testConcurrentInsertPressure(int threadCount, int insertPerThread) {
    ConnectionPool* pool = ConnectionPool::getConnectionPool();
    vector<thread> threads;
    atomic<int> totalInserts(0);

    auto task = [&](int threadId) {
        for (int i = 0; i < insertPerThread; ++i) {
            auto conn = pool->getConnection();
            if (!conn) {
                LOG("线程" << threadId << " 第 " << i << " 次获取连接失败");
                continue;
            }

            // 构造插入语句
            string name = "user_t" + to_string(threadId) + "_i" + to_string(i);
            int age = 20 + (i % 10);
            string sql = "INSERT INTO user(name, age) VALUES('" + name + "', " + to_string(age) + ")";

            // 执行插入
            if (conn->update(sql.c_str())) {
                totalInserts++;
            } else {
                LOG("线程" << threadId << " 第 " << i << " 次插入失败");
            }

            // 模拟处理耗时（可选）
            // this_thread::sleep_for(chrono::milliseconds(5));
        }
        LOG("线程" << threadId << " 完成所有插入");
    };

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back(task, i + 1);
    }

    for (auto& t : threads) {
        t.join();
    }

    LOG("所有线程完成。总插入记录数: " << totalInserts.load());
    pool->printStats();
}

// 清空
void clearUserTable() {
    auto conn = ConnectionPool::getConnectionPool()->getConnection();
    if (conn) {
        conn->update("DELETE FROM user");
        LOG("user 表已清空");
    }
}

// 查询
void selectUserTable() {
    auto conn = ConnectionPool::getConnectionPool()->getConnection();
    if (conn) {
        conn->query("select * from user;");
        LOG("user 查询成功");
    }
}

// 不使用连接池插入
void testWithoutConnectionPool(int insertTimes) {
    LOG("Test: Without Connection Pool");
    auto start = chrono::high_resolution_clock::now();

    for (int i = 0; i < insertTimes; ++i) {
        Connection conn;
        // 构造插入语句
        string name = "user_t" + to_string(i);
        int age = 20 + (i % 10);
        string sql = "INSERT INTO user(name, age) VALUES('" + name + "', " + to_string(age) + ")";
        conn.connect("127.0.0.1", 3306, "test", "mypassword", "mydatabase");
        conn.update(sql);
    }

    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    LOG("Without Connection Pool, Total Time: " << duration.count() << " ms");
}

// 使用连接池插入
void testWithConnectionPool(int insertTimes) {
    LOG("Test: With Connection Pool");
    ConnectionPool* pool = ConnectionPool::getConnectionPool();
    auto start = chrono::high_resolution_clock::now();

    for (int i = 0; i < insertTimes; ++i) {
        shared_ptr<Connection> conn = pool->getConnection();
        string name = "user_t" + to_string(i);
        int age = 20 + (i % 10);
        string sql = "INSERT INTO user(name, age) VALUES('" + name + "', " + to_string(age) + ")";
        conn->update(sql);
    }

    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    LOG("With Connection Pool, Total Time: " << duration.count() << " ms");
}

int main() {

    if  (0) {
        LOG("开始基本功能测试...");
        testBasic();
        
        LOG("\n开始多线程测试...");
        testMultiThread();
        
        LOG("\n开始连接回收测试...");
        testConnectionRecycling();
        
        LOG("\n[TEST] 开始测试增强版");
        testMultiThreadEnhanced();
        LOG("[TEST] 增强版测试结束\n");
    } else  if(0) {



        int threadCount = 10;        // 可设为命令行参数
        int insertPerThread = 1000;   // 每个线程插入次数
        clearUserTable();
        LOG("开始高压插入测试...");
        testConcurrentInsertPressure(threadCount, insertPerThread);
        LOG("测试完毕")
        //selectUserTable();
    } else {
        const int insertTimes = 10000;
        testWithoutConnectionPool(insertTimes);
        cout << "----------------------------" << endl;
        testWithConnectionPool(insertTimes);
    }


    return 0;
}


#elif DEBUG_CONNECTION

// 测试配置 - 修改为你的测试数据库参数
const string TEST_HOST = "127.0.0.1";
const unsigned short TEST_PORT = 3306;
const string TEST_USER = "test";
const string TEST_PWD = "mypassword";
const string TEST_DB = "mydatabase";

void test_connection() {
    try {
        // 1. 测试连接建立
        Connection conn;
        cout << "测试1：连接数据库...";
        if (conn.connect(TEST_HOST, TEST_PORT, TEST_USER, TEST_PWD, TEST_DB)) {
            cout << "成功" << endl;
        } else {
            cout << "失败" << endl;
            return;
        }

        // 2. 测试创建测试表
        cout << "测试2：创建测试表...";
        if (conn.update("CREATE TABLE IF NOT EXISTS test_tb ("
                       "id INT AUTO_INCREMENT PRIMARY KEY,"
                       "name VARCHAR(20) NOT NULL,"
                       "create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP)")) {
            cout << "成功" << endl;
        } else {
            cout << "失败" << endl;
        }

        // 3. 测试插入数据
        cout << "测试3：插入测试数据...";
        if (conn.update("INSERT INTO test_tb(name) VALUES('测试1'), ('测试2')")) {
            cout << "成功" << endl;
        } else {
            cout << "失败" << endl;
        }

        // 4. 测试查询数据
        cout << "测试4：查询数据...";
        MYSQL_RES* res = conn.query("SELECT * FROM test_tb");
        if (res) {
            int num_fields = mysql_num_fields(res);
            MYSQL_ROW row;
            cout << "成功，结果：" << endl;
            while ((row = mysql_fetch_row(res))) {
                for (int i = 0; i < num_fields; i++) {
                    cout << (row[i] ? row[i] : "NULL") << "\t";
                }
                cout << endl;
            }
            mysql_free_result(res);
        } else {
            cout << "失败" << endl;
        }

        // 5. 测试更新数据
        cout << "测试5：更新数据...";
        if (conn.update("UPDATE test_tb SET name='修改测试' WHERE id=1")) {
            cout << "成功" << endl;
        } else {
            cout << "失败" << endl;
        }

        // 6. 测试删除数据
        cout << "测试6：删除数据...";
        if (conn.update("DELETE FROM test_tb WHERE id=2")) {
            cout << "成功" << endl;
        } else {
            cout << "失败" << endl;
        }

        // 7. 测试无效SQL
        cout << "测试7：执行无效SQL...";
        if (!conn.update("INVALID SQL STATEMENT")) {
            cout << "正确处理错误" << endl;
        }

    } catch (const exception& e) {
        cerr << "测试异常: " << e.what() << endl;
    }
}

int main() {
    test_connection();
    return 0;
}


#endif