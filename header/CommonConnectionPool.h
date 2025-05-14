#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <iostream>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <memory>
#include <functional>
using namespace std;
#include "Connection.h"

/**
 * @class ConnectionPool
 * @brief MySQL数据库连接池管理类
 * 
 * @details 该类实现了一个线程安全的MySQL连接池，用于管理和复用数据库连接。
 *          采用单例模式设计，确保整个应用程序中只有一个连接池实例。
 *          包含连接的生产、回收、超时管理等功能。
 */
class ConnectionPool
{
public:
    /**
     * @brief 获取连接池的单例实例
     * @return ConnectionPool* 返回连接池单例对象的指针
     * @note 线程安全的单例实现，使用局部静态变量保证线程安全(C++11及以上)
     */
    static ConnectionPool* getConnectionPool();
    
    /**
     * @brief 从连接池获取一个可用连接
     * @return shared_ptr<Connection> 指向Connection对象的共享指针
     * @throw std::runtime_error 如果获取连接超时或失败
     * @note 使用shared_ptr的自定义删除器实现连接的自动回收
     *       当shared_ptr析构时，连接会自动返回到连接池
     */
    shared_ptr<Connection> getConnection();

    /**
     * @brief 打印连接池的当前统计信息
     * @note 线程安全的方法，可以随时调用获取连接池状态
     *       输出包括：当前连接总数、空闲连接数、等待线程数等
     */
    void printStats() const;

private:
    // 单例模式：禁止构造函数和析构函数公开访问
    ConnectionPool();
    ~ConnectionPool();
    
    /**
     * @brief 从配置文件加载连接池配置
     * @return bool 加载成功返回true，失败返回false
     * @note 配置文件通常为JSON或INI格式，包含数据库连接参数和池大小等设置
     */
    bool loadConfigFile(); 

    /**
     * @brief 连接生产线程的主函数
     * @details 独立线程中运行，根据需求动态创建新连接
     *          当连接不足时创建新连接，达到上限后等待
     */
    void produceConnectionTask();

    /**
     * @brief 连接回收扫描线程的主函数
     * @details 定期扫描空闲连接队列，回收超过最大空闲时间的连接
     *          保持连接池在合理大小，避免资源浪费
     */
    void scannerConnectionTask();

    // 数据库连接配置参数
    string _ip;                 // MySQL服务器IP地址
    unsigned short _port;       // MySQL服务器端口号(默认3306)
    string _username;           // 数据库用户名
    string _password;           // 数据库密码
    string _dbname;             // 默认连接的数据库名称

    // 连接池配置参数
    int _initSize;              // 连接池初始连接数量
    int _maxSize;              // 连接池允许的最大连接数量
    int _maxIdleTime;          // 连接最大空闲时间(毫秒)
    int _connectionTimeout;    // 获取连接的超时时间(毫秒)

    // 连接池状态管理
    queue<Connection*> _connectionQue;  // 空闲连接队列(FIFO)
    mutable mutex _queueMutex;         // 保护连接队列的互斥锁
    atomic_int _connectionCnt;         // 当前总连接数(包括在使用和空闲的)
    condition_variable cv;             // 生产-消费模型的条件变量
};
