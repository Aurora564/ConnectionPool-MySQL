#include "CommonConnectionPool.h"
#include "public.h"
#define DEBUG
/**
 * @brief 获取连接池单例实例（线程安全的懒汉模式)
 * @return ConnectionPool* 返回连接池单例对象的指针
 * @note 使用局部静态变量保证线程安全(C++11及以上)
 *       该方法在第一次调用时创建连接池实例，后续调用返回同一实例
 *       适用于多线程环境，确保只有一个连接池实例
 *       连接池实例在程序结束时自动释放
 */
ConnectionPool* ConnectionPool::getConnectionPool()
{
	static ConnectionPool pool; // lock和unlock
	return &pool;
}

/**
 * @brief 从配置文件加载连接池配置
 * @return bool 加载成功返回true，失败返回false
 * @note 配置文件格式为`key=value`, 支持#注释和[section]
 *       支持的配置项有：ip, port, username, password, dbname, initsize, maxsize, maxidletime
 *       连接池的初始大小、最大大小、最大空闲时间等 
 */
bool ConnectionPool::loadConfigFile() {
    FILE *pf = fopen("mysql.cnf", "r");
    if (pf == nullptr) {
        LOG("mysql.cnf file is not exist!");
        return false;
    }

    char line[1024];
    int lineNum = 0;
    bool hasError = false;

    while (fgets(line, sizeof(line), pf)) {
        lineNum++;
        string str = line;

        // 去除行尾换行符和回车符
        str.erase(str.find_last_not_of("\r\n") + 1);

        // 去除行首空白
        str.erase(0, str.find_first_not_of(" \t"));

        // 跳过空行和注释行
        if (str.empty() || str[0] == '#') {
            continue;
        }

        // 跳过段落标识符（例如 [client], [pool]）
        if (str.front() == '[' && str.back() == ']') {
            continue;
        }

        // 查找并去除行内注释
        size_t commentPos = str.find('#');
        if (commentPos != string::npos) {
            str = str.substr(0, commentPos);
            str.erase(str.find_last_not_of(" \t") + 1);
        }

        if (str.empty()) {
            continue;
        }

        // 解析键值对
        size_t eq_pos = str.find('=');
        if (eq_pos == string::npos) {
            LOG("Config syntax error at line " << lineNum << ": missing '='");
            hasError = true;
            continue;
        }

        string key = str.substr(0, eq_pos);
        string value = str.substr(eq_pos + 1);

        // 去除键和值的首尾空白
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        // 字段名统一转为小写
        transform(key.begin(), key.end(), key.begin(), ::tolower);

        // 处理配置项
        if (key == "ip" || key == "host") _ip = value;
        else if (key == "port") _port = stoi(value);
        else if (key == "username" || key == "user") _username = value;
        else if (key == "password") _password = value;
        else if (key == "dbname" || key == "database") _dbname = value;
        else if (key == "initsize" || key == "initial_size") _initSize = stoi(value);
        else if (key == "maxsize" || key == "max_size") _maxSize = stoi(value);
        else if (key == "maxidletime" || key == "max_idle_time") _maxIdleTime = stoi(value);
        else if (key == "connectiontimeout" || key == "connect_timeout") _connectionTimeout = stoi(value);
        else if (key == "test_on_borrow" || key == "validation_query") {
            // 忽略不影响主逻辑的配置项
        }
        else {
            LOG("Warning: Unknown config key '" << key << "' at line " << lineNum);
        }
    }

    fclose(pf);

    // 验证必要配置
    if (_ip.empty()) {
        LOG("Error: Missing required configuration 'ip' or 'host'");
        hasError = true;
    }
    if (_username.empty()) {
        LOG("Error: Missing required configuration 'username' or 'user'");
        hasError = true;
    }
    if (_dbname.empty()) {
        LOG("Error: Missing required configuration 'dbname' or 'database'");
        hasError = true;
    }

    // 验证数值范围
    if (_initSize <= 0 || _maxSize <= 0 || _initSize > _maxSize) {
        LOG("Error: Invalid pool size configuration");
        hasError = true;
    }

    if (hasError) {
        return false;
    }

    LOG("Configuration loaded successfully:");
    LOG("  MySQL Server: " << _ip << ":" << _port);
    LOG("  Username: " << _username);
    LOG("  Database: " << _dbname);
    LOG("  Pool init size: " << _initSize);
    LOG("  Pool max size: " << _maxSize);
    LOG("  Max idle time: " << _maxIdleTime << "s");
    LOG("  Connection timeout: " << _connectionTimeout << "s");

    return true;
}

/**
 * @brief 连接池的构造函数
 * @details 1. 该构造函数为支持单例模式声明为私有成员
 *          2. 该函数在第一次调用`getConnectionPool()`时被调用
 *          3. 该函数在调用时，调用`loadConfigFile()`，并检查配置文件是否被正确读取
 *             3.1 当未被正确读取，直接返回则会导致连接池中连接空置，连接池未被正确启动
 *             3.2 当被正确读取，参考 （4.）  
 *          4. 函数创建初始数量的连接，并将连接放入连接池中
 *          5. 启动一个新的线程`produceConnectionTask`，作为连接的生产者
 *             5.1 该线程会在连接池中连接不足时，自动创建新的连接
 *          6. 启动一个新的线程`scannerConnectionTask`，作为连接的回收者
 *             6.1 该线程会定时扫描连接池中空闲连接，回收超过最大空闲时间的连接
 */
ConnectionPool::ConnectionPool()
{
	// 加载配置项了
	if (!loadConfigFile())
	{
		return;
	}

	// 创建初始数量的连接
	for (int i = 0; i < _initSize; ++i)
	{
		Connection *p = new Connection();
		p->connect(_ip, _port, _username, _password, _dbname);
		p->refreshAliveTime(); // 刷新一下开始空闲的起始时间
		_connectionQue.push(p);
		_connectionCnt++;
	}

	// 启动一个新的线程，作为连接的生产者 linux thread => pthread_create
	thread produce(std::bind(&ConnectionPool::produceConnectionTask, this));
	produce.detach();

	// 启动一个新的定时线程，扫描超过maxIdleTime时间的空闲连接，进行对于的连接回收
	thread scanner(std::bind(&ConnectionPool::scannerConnectionTask, this));
	scanner.detach();
}


// 给外部提供接口，从连接池中获取一个可用的空闲连接
shared_ptr<Connection> ConnectionPool::getConnection() {
    unique_lock<mutex> lock(_queueMutex);
    while (_connectionQue.empty()) {
        if (cv_status::timeout == cv.wait_for(lock, chrono::milliseconds(_connectionTimeout))) {
            LOG("获取连接超时");
            return nullptr;
        }
    }

    // 检查连接有效性
    while (!_connectionQue.empty()) {
        auto pcon = _connectionQue.front();
        _connectionQue.pop();
        if (pcon->isValid()) {  // 假设Connection类已实现isValid()
            pcon->refreshAliveTime();
            return shared_ptr<Connection>(pcon, [this](Connection* p) {
                unique_lock<mutex> lock(_queueMutex);
                if (p->isValid()) {  // 只有有效连接才放回队列
                    p->refreshAliveTime();
                    _connectionQue.push(p);
                } else {
                    _connectionCnt--;
                    delete p;  // 销毁无效连接
                }
                cv.notify_all();
            });
        } else {
            _connectionCnt--;
            delete pcon;  // 清理无效连接
        }
    }
    return nullptr;
}

/**
 * @brief 连接生产线程主函数
 * @details 在独立后台线程中运行，负责动态创建并维护数据库连接：
 *          1. 当连接池为空或连接数未达上限时，创建新连接
 *          2. 确保连接总数不超过配置的最大限制(_maxSize)
 *          3. 自动处理连接创建失败的情况
 *          4. 通过条件变量实现高效等待/通知机制
 * 
 * @note 关键实现细节：
 *       - 使用unique_lock配合条件变量实现线程安全等待
 *       - 等待条件：连接池空或未达最大连接数(_connectionCnt < _maxSize)
 *       - 创建新连接过程：
 *         * 加锁状态下创建Connection对象
 *         * 尝试建立实际数据库连接
 *         * 成功则刷新时间戳并加入队列
 *         * 失败则立即释放资源
 *       - 捕获所有异常避免线程意外退出
 *       - 每次操作后通过notify_all通知可能等待的消费者
 * 
 * @warning 注意事项：
 *          - 必须保证线程安全（所有共享数据访问加锁）
 *          - 连接创建失败时应妥善释放资源
 *          - 死循环确保线程持续运行
 *          - 异常处理避免线程崩溃
 * 
 * @param[in] void 无显式参数（通过类成员访问配置）
 * @return void 无返回值（无限循环执行）
 */
void ConnectionPool::produceConnectionTask() {
    // 无限循环保持线程持续运行
    for (;;) {
        // 加锁并等待生产条件（自动释放锁等待，唤醒后重新获取）
        unique_lock<mutex> lock(_queueMutex);
        // 等待条件：连接池空或连接数未达最大连接数
        cv.wait(lock, [this] { 
            return _connectionQue.empty() || _connectionCnt < _maxSize; 
        });

        // 检查是否允许创建新连接（可能被虚假唤醒）
        if (_connectionCnt < _maxSize) {
            try {
                // 创建新连接对象
                Connection* p = new Connection();
                
                // 尝试建立实际数据库连接
                if (p->connect(_ip, _port, _username, _password, _dbname)) {
                    // 连接成功：记录时间戳并加入队列
                    p->refreshAliveTime();
                    _connectionQue.push(p);
                    _connectionCnt++; // 原子计数器递增
                } else {
                    // 连接失败：立即释放资源
                    delete p;  
                }
            } catch (...) {
                // 捕获所有异常，防止线程退出
                LOG("创建连接异常");
            }
        }
        
        // 通知可能等待的消费者线程
        cv.notify_all();
    }
}


/**
 * @brief 空闲连接回收线程主函数
 * @details 定期扫描并回收空闲超时的数据库连接，保持连接池健康状态：
 *          1. 定时扫描（间隔=_maxIdleTime）
 *          2. 只回收超过最大空闲时间的连接
 *          3. 保证连接池至少保持_initSize个连接
 *          4. 采用安全的方式销毁连接（避免长时间持锁）
 * 
 * @note 关键实现细节：
 *       - 扫描周期与_maxIdleTime相同，保证及时回收
 *       - 先检查连接数是否大于_initSize，避免过度回收
 *       - 队列有序性：队头是最早创建的连接，只需检查队头
 *       - 销毁连接时临时释放锁，减少锁的持有时间
 *       - 使用while循环保证持续运行
 * 
 * @warning 注意事项：
 *          - 必须保证线程安全（所有共享数据访问加锁）
 *          - 连接销毁时要先释放锁，避免阻塞其他线程
 *          - 保持至少_initSize个连接是硬性要求
 *          - 死循环确保线程持续运行
 */
void ConnectionPool::scannerConnectionTask() {
    // 无限循环保持线程持续运行
    for (;;) {
        // 定时扫描（间隔=maxIdleTime）
        this_thread::sleep_for(chrono::seconds(_maxIdleTime));

        // 加锁扫描空闲连接
        unique_lock<mutex> lock(_queueMutex);
        
        // 只回收超出初始数量的连接（保持最小连接数）
        while (_connectionCnt > _initSize) {
            Connection* p = _connectionQue.front();
            
            // 检查队头连接是否超时（毫秒比较）
            if (p->getAliveeTime() >= (_maxIdleTime * 1000)) {
                _connectionQue.pop();  // 从队列移除
                _connectionCnt--;     // 总数减1
                
                // 关键！释放锁后再销毁连接
                lock.unlock();
                delete p;  // 实际销毁连接（可能耗时）
                lock.lock(); // 继续处理前重新加锁
            } else {
                // 队头未超时则后续连接也不会超时（队列有序）
                break; 
            }
        }
    }
}






// 析构函数
ConnectionPool::~ConnectionPool() {
    unique_lock<mutex> lock(_queueMutex);
    while (!_connectionQue.empty()) {
        delete _connectionQue.front();
        _connectionQue.pop();
    }
    _connectionCnt = 0;
}


// 打印线程池信息
void ConnectionPool::printStats() const {
    unique_lock<mutex> lock(_queueMutex);
    cout << "连接池状态: " 
         << "总数=" << _connectionCnt 
         << ", 空闲=" << _connectionQue.size()
         << endl;
}


