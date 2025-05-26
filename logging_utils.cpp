#include "logging_utils.h"
#include "spdlog/sinks/basic_file_sink.h" // 用于文件日志
#include "spdlog/sinks/stdout_color_sinks.h" // 用于彩色控制台日志
#include <iostream> // 用于在日志初始化失败时输出错误

// 定义全局日志记录器实例
std::shared_ptr<spdlog::logger> g_console_logger;
std::shared_ptr<spdlog::logger> g_data_file_logger;

void initialize_loggers(const std::string& data_log_filename, bool truncate_data_log)
{
    try {
        // 1. 控制台日志记录器
        g_console_logger = spdlog::stdout_color_mt("console");
        // 设置日志级别 (例如: trace, debug, info, warn, error, critical, off)
        g_console_logger->set_level(spdlog::level::info);
        // 设置日志格式: [时间戳精确到毫秒] [日志记录器名称] [级别] 消息内容
        g_console_logger->set_pattern("[%H:%M:%S.%e] [%n] [%^%l%$] %v");

        // 2. 数据文件日志记录器
        // 使用 basic_file_sink_mt，它会缓冲日志，我们将在程序结束时手动刷新
        g_data_file_logger = spdlog::basic_logger_mt("data_file", data_log_filename, truncate_data_log);
        g_data_file_logger->set_level(spdlog::level::info);
        // 对于数据文件，通常只记录消息本身，以便后续处理 (如CSV)
        g_data_file_logger->set_pattern("%v");
        // spdlog 默认情况下，在缓冲区满或遇到高级别日志（如 error, critical）时可能会自动刷新。
        // 为了确保仅在最后刷新，主要依赖于程序结束时的显式 flush 调用。
        // 对于普通 info 级别的日志，它会主要依赖内部缓冲。

        spdlog::set_default_logger(g_console_logger); // 将控制台记录器设为默认，这样spdlog::info()等会用它
        spdlog::info("Loggers initialized. Data will be written to '{}'.", data_log_filename);

    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        // 可以在这里决定是否中止程序
    }
}

void shutdown_loggers()
{
    if (g_console_logger) {
        g_console_logger->info("Flushing all logs before shutdown...");
    }
    if (g_data_file_logger) {
        g_data_file_logger->flush();
    }
    spdlog::shutdown(); // 关闭spdlog，释放资源，并确保所有异步日志已写入
}