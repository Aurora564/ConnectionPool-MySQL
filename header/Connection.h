#pragma once
#include <mysql.h>
#include <string>
#include <ctime>
using namespace std;

/**
 * @class Connection
 * @brief MySQL数据库连接封装类
 * @details 封装MySQL C API，提供更安全的数据库连接和操作接口
 *          管理连接生命周期，自动维护连接状态和时间戳
 */
class Connection
{
public:
    /**
     * @brief 构造函数
     * @note 初始化MySQL连接对象，但不建立实际连接
     */
    Connection();

    /**
     * @brief 析构函数 
     * @note 自动关闭数据库连接并释放资源
     */
    ~Connection();

    /**
     * @brief 获取底层MYSQL连接指针
     * @return MYSQL* 原生MySQL连接句柄
     * @warning 暴露原生指针需谨慎使用，主要用于错误处理等特殊场景
     */
    MYSQL* getMYSQL() const { return _conn; }

    /**
     * @brief 建立实际数据库连接
     * @param ip 数据库服务器IP地址
     * @param port 数据库服务器端口
     * @param user 登录用户名
     * @param password 登录密码
     * @param dbname 要连接的数据库名
     * @param connect_timeout 连接超时时间(秒)，默认30秒
     * @return bool 连接成功返回true，失败返回false
     */
    bool connect(string ip, 
        unsigned short port, 
        string user, 
        string password,
        string dbname,
        unsigned int connect_timeout = 30
    );

    /**
     * @brief 检查连接是否有效
     * @return bool 连接有效且可ping通返回true
     * @note 会实际发送ping命令测试连接活性
     */
    bool isValid() const;

    /**
     * @brief 执行更新操作(INSERT/UPDATE/DELETE)
     * @param sql SQL语句
     * @return bool 执行成功返回true
     * @throw 可能抛出数据库异常
     */
    bool update(string sql);

    /**
     * @brief 执行查询操作(SELECT)
     * @param sql 查询SQL语句
     * @return MYSQL_RES* 查询结果集指针
     * @note 调用者需负责释放结果集资源
     * @code
     * MYSQL_RES* res = conn.query("SELECT * FROM table");
     * mysql_free_result(res); // 必须手动释放
     * @endcode
     */
    MYSQL_RES* query(string sql);

    /**
     * @brief 刷新连接空闲时间戳
     * @note 将_alivetime设置为当前时钟时间
     *       用于连接池管理连接存活时间
     */
    void refreshAliveTime() { _alivetime = clock(); }

    /**
     * @brief 获取连接空闲时长
     * @return clock_t 毫秒级的空闲时间
     */
    clock_t getAliveeTime()const { return clock() - _alivetime; }

private:
    MYSQL *_conn;        ///< MySQL原生连接句柄
    clock_t _alivetime;  ///< 记录最后活动时间戳(用于连接池超时管理)
};

// 实现文件建议添加的注释示例：

