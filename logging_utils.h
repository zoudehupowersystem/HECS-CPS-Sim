#ifndef LOGGING_UTILS_H
#define LOGGING_UTILS_H

#include "spdlog/spdlog.h"
#include <memory>
#include <string>

// 全局日志记录器实例 (声明)
extern std::shared_ptr<spdlog::logger> g_console_logger;
extern std::shared_ptr<spdlog::logger> g_data_file_logger;

// 初始化日志记录器函数
// data_log_filename: 数据日志文件的名称
// truncate_data_log: 是否在启动时清空已存在的数据日志文件
void initialize_loggers(const std::string& data_log_filename = "simulation_output.csv", bool truncate_data_log = true);

// 函数用于在程序结束时刷新和关闭日志
void shutdown_loggers();

#endif // LOGGING_UTILS_H