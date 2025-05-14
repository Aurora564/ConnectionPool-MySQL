/**
 * @brief Connection类实现要点：
 * 1. 构造函数初始化MySQL连接对象
 * 2. 析构函数确保连接关闭
 * 3. 连接有效性检查通过mysql_ping实现
 * 4. 所有数据库操作都包含错误处理
 * 5. 线程安全注意事项：非线程安全类，应在连接池中管理
 */

#include "Connection.h"
#include "public.h"
#include <stdexcept>
#include <cstring>
#include <iostream>
using namespace std;

/**
 * @brief Connection类构造函数
 * @details 初始化MySQL连接对象并设置基本连接选项，包括：
 *          1. 初始化MySQL连接句柄
 *          2. 配置自动重连选项
 *          3. 设置连接超时时间
 * 
 * @note 关键实现细节：
 *       - 使用mysql_init初始化连接对象
 *       - 失败时抛出runtime_error异常并记录日志
 *       - 默认启用自动重连(MYSQL_OPT_RECONNECT)
 *       - 设置30秒连接超时(MYSQL_OPT_CONNECT_TIMEOUT)
 * 
 * @warning 注意事项：
 *          - 构造函数仅初始化对象，不建立实际连接
 *          - 抛出异常表示对象构造失败
 *          - 自动重连仅在MySQL 5.1.6+版本有效
 * 
 * @throw std::runtime_error 当mysql_init失败时抛出
 * 
 * @param[in] void 无参数
 * @return 无返回值
 * 
 * @par 初始化流程：
 * @code
 * 1. 初始化成员变量(_conn=nullptr, _alivetime=0)
 * 2. 调用mysql_init创建连接对象
 * 3. 设置自动重连选项
 * 4. 设置连接超时选项
 * @endcode
 */
Connection::Connection() : _conn(nullptr), _alivetime(0) {
    // 初始化MySQL连接对象
    _conn = mysql_init(nullptr);
    if (!_conn) {
        // 记录错误日志并抛出异常
        LOG("MySQL initialization failed: " << mysql_error(nullptr));
        throw std::runtime_error("MySQL initialization failed: " + 
                               std::string(mysql_error(nullptr)));
    }

    // 配置自动重连选项(连接断开后自动尝试重连)
    bool auto_reconnect = true;
    mysql_options(_conn, MYSQL_OPT_RECONNECT, &auto_reconnect);
    
    // 设置连接超时时间(秒)
    unsigned int timeout_sec = 30;
    mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
}

/**
 * @brief Connection类析构函数
 * @details 安全释放MySQL连接资源，主要完成：
 *          1. 记录连接存活时间日志（用于调试和性能分析）
 *          2. 关闭MySQL数据库连接
 *          3. 重置连接指针避免野指针
 * 
 * @note 关键实现细节：
 *       - 仅当_conn非空时才执行释放操作
 *       - 计算并记录连接从最后一次活动到销毁的总时长
 *       - 使用mysql_close安全关闭连接
 *       - 将_conn置为nullptr符合最佳实践
 * 
 * @warning 注意事项：
 *          - 必须在所有数据库操作完成后才会调用
 *          - 线程不安全，不应在多线程环境下直接调用
 *          - 连接关闭后不应再使用该对象
 * 
 * @par 典型调用场景：
 * 1. 连接对象离开作用域时自动调用
 * 2. 被连接池销毁闲置连接时调用
 * 3. 发生异常时的栈回滚过程
 * 
 * @param[in] void 无参数
 * @return 无返回值
 */
Connection::~Connection() {
    if (_conn) {
        // 记录连接存活时间（秒级精度）
        LOG("Releasing connection (alive time: " 
           << (clock() - _alivetime)/CLOCKS_PER_SEC << "s)");
        
        // 安全关闭MySQL连接
        mysql_close(_conn);
        
        // 重置指针避免重复释放
        _conn = nullptr;
    }
}


/**
 * @brief 建立实际数据库连接
 * @param ip 数据库服务器IP地址
 * @param port 数据库服务器端口号
 * @param user 数据库用户名
 * @param password 数据库密码
 * @param dbname 要连接的数据库名称
 * @param connect_timeout 连接超时时间(秒)
 * @return bool 连接成功返回true，失败抛出异常
 * @throw std::runtime_error 连接失败时抛出包含错误信息的异常
 * 
 * @details 该方法执行完整的数据库连接流程：
 * 1. 清理现有连接(如果存在)
 * 2. 设置连接和I/O超时参数
 * 3. 尝试建立实际TCP连接
 * 4. 配置UTF8MB4字符集(支持完整Unicode)
 * 5. 更新连接活跃时间戳
 * 
 * @note 关键实现细节：
 * - 自动处理已有连接的清理和重新初始化
 * - 同时设置连接超时和读写超时
 * - 启用多语句执行支持(CLIENT_MULTI_STATEMENTS)
 * - 强制使用utf8mb4字符集(兼容4字节Unicode)
 * - 连接成功后自动刷新活跃时间
 * 
 * @warning 注意事项：
 * - 该方法非幂等，失败后连接状态不可用
 * - 会修改成员变量_conn的状态
 * - 所有错误都转换为异常抛出
 * - 超时时间同时影响TCP连接和后续查询
 * 
 * @par 典型错误处理：
 * @code
 * try {
 *     conn.connect("127.0.0.1", 3306, "user", "pass", "db");
 * } catch (const std::exception& e) {
 *     // 处理连接失败
 * }
 * @endcode
 */
bool Connection::connect(string ip, unsigned short port,
                       string user, string password, string dbname,
                       unsigned int connect_timeout) {
    LOG("Connecting to " << ip << ":" << port << "...");
    
    // 清理现有连接(如果存在)
    if (_conn) {
        mysql_close(_conn);
        _conn = mysql_init(nullptr);
        if (!_conn) {
            LOG("Re-initialization failed after connection reset");
            throw std::runtime_error("Re-initialization failed after reset");
        }
    }

    // 设置超时参数(秒转毫秒)
    unsigned int timeout_ms = connect_timeout * 1000;
    mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    mysql_options(_conn, MYSQL_OPT_READ_TIMEOUT, &timeout_ms);
    mysql_options(_conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout_ms);

    // 尝试建立实际连接
    MYSQL* conn = mysql_real_connect(
        _conn, ip.c_str(), user.c_str(), password.c_str(),
        dbname.c_str(), port, nullptr, CLIENT_MULTI_STATEMENTS
    );

    // 连接失败处理
    if (!conn) {
        std::string err = mysql_error(_conn);
        LOG("Connection failed: " << err);
        mysql_close(_conn);
        _conn = nullptr;
        throw std::runtime_error("Connection failed to " + ip + ":" + 
                               std::to_string(port) + " - " + err);
    }

    // 配置字符集(宽松处理错误)
    if (mysql_set_character_set(_conn, "utf8mb4") != 0) {
        LOG("Character set warning: " << mysql_error(_conn));
    }

    // 更新连接活跃时间
    refreshAliveTime();
    LOG("Connection established successfully");
    return true;
}



/**
 * @brief 执行数据库更新操作（INSERT/UPDATE/DELETE）
 * @param sql 要执行的SQL语句
 * @return bool 执行成功返回true，失败返回false
 * @throw std::runtime_error 当连接无效时抛出异常
 * 
 * @details 该方法执行非查询类SQL语句，主要功能：
 * 1. 验证连接有效性
 * 2. 执行SQL语句
 * 3. 检查受影响行数（针对INSERT/UPDATE/DELETE）
 * 4. 记录详细的错误日志
 * 
 * @note 关键实现细节：
 * - 使用mysql_real_query执行原始SQL
 * - 自动识别INSERT/UPDATE/DELETE语句
 * - 通过mysql_affected_rows验证操作影响
 * - SQL日志截断前200字符防止日志过载
 * - 错误日志包含SQL片段和详细错误信息
 * 
 * @warning 注意事项：
 * - 必须先建立有效连接
 * - 不会自动提交事务（需显式commit）
 * - 长SQL会被截断记录
 * - 返回值仅表示执行是否成功，不表示业务影响
 * 
 * @par 典型使用示例：
 * @code
 * if (conn.update("UPDATE users SET status=1 WHERE id=123")) {
 *     // 处理成功逻辑
 * } else {
 *     // 处理失败逻辑
 * }
 * @endcode
 */
bool Connection::update(string sql) {
    // 验证连接状态
    if (!_conn) {
        LOG("Update attempted on null connection");
        throw std::runtime_error("Null connection in update");
    }

    // 执行SQL语句
    if (mysql_real_query(_conn, sql.c_str(), sql.size())) {
        // 记录详细错误日志（截断长SQL）
        LOG("Update failed: " << mysql_error(_conn) << "\nSQL: " 
           << sql.substr(0, 200) << (sql.length() > 200 ? "..." : ""));
        return false;
    }

    // 针对DML语句检查影响行数
    if (strncasecmp(sql.c_str(), "insert", 6) == 0 ||
        strncasecmp(sql.c_str(), "update", 6) == 0 ||
        strncasecmp(sql.c_str(), "delete", 6) == 0) {
        if (mysql_affected_rows(_conn) == -1) {
            LOG("No rows affected by: " << sql.substr(0, 200));
        }
    }
    
    return true;
}


/**
 * @brief 执行数据库查询操作（SELECT）
 * @param sql 查询SQL语句
 * @return MYSQL_RES* 查询结果集指针
 * @throw std::runtime_error 当连接无效或查询失败时抛出异常
 * 
 * @details 该方法执行查询类SQL语句，主要功能：
 * 1. 验证连接有效性
 * 2. 执行SQL查询
 * 3. 检查结果集有效性
 * 4. 记录详细的错误日志
 * 
 * @note 关键实现细节：
 * - 使用mysql_real_query执行原始SQL
 * - 自动处理结果集存储和释放
 * - 错误日志包含SQL片段和详细错误信息
 * 
 * @warning 注意事项：
 * - 必须先建立有效连接
 * - 调用者需负责释放结果集资源
 * - 长SQL会被截断记录
 * 
 * @par 典型使用示例：
 * @code
 * MYSQL_RES* res = conn.query("SELECT id, name FROM users");
 * if (res) {
 *     // 处理结果集...
 *     mysql_free_result(res); // 手动释放结果集资源
 * }
 * @endcode
 */
MYSQL_RES* Connection::query(string sql) {
    if (!_conn) {
        LOG("Query attempted on null connection");
        throw std::runtime_error("Null connection in query");
    }

    if (mysql_real_query(_conn, sql.c_str(), sql.size())) {
        LOG("Query failed: " << mysql_error(_conn) << "\nSQL: " 
           << sql.substr(0, 200) << (sql.length() > 200 ? "..." : ""));
        return nullptr;
    }

    MYSQL_RES* result = mysql_store_result(_conn);
    if (!result && mysql_field_count(_conn) > 0) {
        LOG("Result storage failed: " << mysql_error(_conn));
        throw std::runtime_error("Result storage failed: " + 
                               std::string(mysql_error(_conn)));
    }
    return result;
}


/**
 * @brief 检测连接是否存活
 * @details 通过检测连接句柄和mysql_ping实现
 * 
 * @note 关键实现细节：
 * - 使用mysql_ping检查连接状态
 * 
 * @warning 注意事项：
 * - 该方法不影响连接状态
 * 
 * @param[in] void 无参数
 * @return bool 连接有效返回true，无效返回false
 */
bool Connection::isValid() const {
    return _conn && 0 == mysql_ping(_conn);  // 更可靠的检查方式
}
