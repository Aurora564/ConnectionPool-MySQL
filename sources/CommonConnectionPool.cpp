#include "CommonConnectionPool.h"
#include "public.h"
#define DEBUG
// 线程安全的懒汉单例函数接口
ConnectionPool* ConnectionPool::getConnectionPool()
{
	static ConnectionPool pool; // lock和unlock
	return &pool;
}

// 加载配置
// 加载配置文件
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

// 连接池的构造
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

// 运行在独立的线程中，专门负责生产新连接
void ConnectionPool::produceConnectionTask() {
    for (;;) {
        unique_lock<mutex> lock(_queueMutex);
        cv.wait(lock, [this] { 
            return _connectionQue.empty() || _connectionCnt < _maxSize; 
        });

        if (_connectionCnt < _maxSize) {
            try {
                Connection* p = new Connection();
                if (p->connect(_ip, _port, _username, _password, _dbname)) {
                    p->refreshAliveTime();
                    _connectionQue.push(p);
                    _connectionCnt++;
                } else {
                    delete p;  // 连接失败时清理
                }
            } catch (...) {
                LOG("创建连接异常");
            }
        }
        cv.notify_all();
    }
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


// 扫描超过maxIdleTime时间的空闲连接，进行对于的连接回收
void ConnectionPool::scannerConnectionTask() {
	for (;;)
	{
		// 通过sleep模拟定时效果
		this_thread::sleep_for(chrono::seconds(_maxIdleTime));

		// 扫描整个队列，释放多余的连接
		unique_lock<mutex> lock(_queueMutex);
		while (_connectionCnt > _initSize)
		{
			Connection *p = _connectionQue.front();
			if (p->getAliveeTime() >= (_maxIdleTime * 1000))
			{
				_connectionQue.pop();
				_connectionCnt--;
				lock.unlock();	// 释放锁后在进行销毁
				delete p; // 调用~Connection()释放连接
				lock.lock();
			}
			else
			{
				break; // 队头的连接没有超过_maxIdleTime，其它连接肯定没有
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


