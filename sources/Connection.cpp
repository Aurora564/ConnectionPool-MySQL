#include "Connection.h"
#include "public.h"
#include <stdexcept>
#include <cstring>
#include <iostream>
using namespace std;

Connection::Connection() : _conn(nullptr), _alivetime(0) {
    _conn = mysql_init(nullptr);
    if (!_conn) {
        LOG("MySQL initialization failed: " << mysql_error(nullptr));
        throw std::runtime_error("MySQL initialization failed: " + 
                               std::string(mysql_error(nullptr)));
    }

    bool auto_reconnect = true;
    mysql_options(_conn, MYSQL_OPT_RECONNECT, &auto_reconnect);
    
    unsigned int timeout_sec = 30;
    mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
}

Connection::~Connection() {
    if (_conn) {
        LOG("Releasing connection (alive time: " 
           << (clock() - _alivetime)/CLOCKS_PER_SEC << "s)");
        mysql_close(_conn);
        _conn = nullptr;
    }
}

bool Connection::connect(string ip, unsigned short port,
                       string user, string password, string dbname,
                       unsigned int connect_timeout) {
    LOG("Connecting to " << ip << ":" << port << "...");
    
    if (_conn) {
        mysql_close(_conn);
        _conn = mysql_init(nullptr);
        if (!_conn) {
            LOG("Re-initialization failed after connection reset");
            throw std::runtime_error("Re-initialization failed after reset");
        }
    }

    unsigned int timeout_ms = connect_timeout * 1000;
    mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    mysql_options(_conn, MYSQL_OPT_READ_TIMEOUT, &timeout_ms);
    mysql_options(_conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout_ms);

    MYSQL* conn = mysql_real_connect(
        _conn, ip.c_str(), user.c_str(), password.c_str(),
        dbname.c_str(), port, nullptr, CLIENT_MULTI_STATEMENTS
    );

    if (!conn) {
        std::string err = mysql_error(_conn);
        LOG("Connection failed: " << err);
        mysql_close(_conn);
        _conn = nullptr;
        throw std::runtime_error("Connection failed to " + ip + ":" + 
                               std::to_string(port) + " - " + err);
    }

    if (mysql_set_character_set(_conn, "utf8mb4") != 0) {
        LOG("Character set warning: " << mysql_error(_conn));
    }

    refreshAliveTime();
    LOG("Connection established successfully");
    return true;
}

bool Connection::update(string sql) {
    if (!_conn) {
        LOG("Update attempted on null connection");
        throw std::runtime_error("Null connection in update");
    }

    if (mysql_real_query(_conn, sql.c_str(), sql.size())) {
        LOG("Update failed: " << mysql_error(_conn) << "\nSQL: " 
           << sql.substr(0, 200) << (sql.length() > 200 ? "..." : ""));
        return false;
    }

    if (strncasecmp(sql.c_str(), "insert", 6) == 0 ||
        strncasecmp(sql.c_str(), "update", 6) == 0 ||
        strncasecmp(sql.c_str(), "delete", 6) == 0) {
        if (mysql_affected_rows(_conn) == -1) {
            LOG("No rows affected by: " << sql.substr(0, 200));
        }
    }
    return true;
}

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


bool Connection::isValid() const {
    return _conn && 0 == mysql_ping(_conn);  // 更可靠的检查方式
}
