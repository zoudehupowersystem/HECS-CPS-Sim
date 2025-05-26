#include "cps_coro_lib.h" // 引入协程库
#include <iostream> // 用于标准输入输出

// 定义事件ID
constexpr cps_coro::EventId VOLTAGE_CHANGE_EVENT = 10000;

// 事件数据结构体
struct VoltageData {
    double voltage; // 电压值
    cps_coro::Scheduler::time_point timestamp; // 事件发生的时间戳
};

// Sensor 协程：模拟传感器检测电压变化并触发事件
cps_coro::Task sensor_coroutine(cps_coro::Scheduler& scheduler)
{
    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Sensor: Initializing." << std::endl;

    // 模拟10秒后电压跌落
    co_await cps_coro::delay(std::chrono::seconds(10));
    VoltageData low_voltage_data = { 0.92, scheduler.now() };
    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Sensor: Voltage drop detected. V = " << low_voltage_data.voltage << std::endl;
    scheduler.trigger_event(VOLTAGE_CHANGE_EVENT, low_voltage_data);

    // 模拟再过10秒后电压恢复 (总计20秒)
    co_await cps_coro::delay(std::chrono::seconds(10));
    VoltageData normal_voltage_data = { 1.01, scheduler.now() };
    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Sensor: Voltage rise detected. V = " << normal_voltage_data.voltage << std::endl;
    scheduler.trigger_event(VOLTAGE_CHANGE_EVENT, normal_voltage_data);

    co_await cps_coro::delay(std::chrono::seconds(5)); // 再等待一段时间确保AVC可以处理最后一个事件
    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Sensor: Shutting down." << std::endl;
}

// AVC (Automatic Voltage Control) 协程：等待电压变化事件并作出响应
cps_coro::Task avc_coroutine(cps_coro::Scheduler& scheduler)
{
    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] AVC: Initializing. Waiting for voltage events." << std::endl;
    int event_count = 0;
    try {
        while (event_count < 2) { // 演示处理多个事件
            // 等待 VOLTAGE_CHANGE_EVENT 事件，并期望接收 VoltageData 类型的数据
            VoltageData data = co_await cps_coro::wait_for_event<VoltageData>(VOLTAGE_CHANGE_EVENT);
            event_count++;

            std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] AVC: Received VOLTAGE_CHANGE_EVENT. Voltage = "
                      << data.voltage << " (Event timestamp: " << data.timestamp.time_since_epoch().count() << "ms)" << std::endl;

            if (data.voltage < 0.95) {
                std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] AVC: Action -> 投入无功补偿设备指令 (Capacitor bank IN)" << std::endl;
            } else if (data.voltage > 1.05) {
                std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] AVC: Action -> 切除无功补偿设备指令 (Capacitor bank OUT)" << std::endl;
            } else {
                std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] AVC: Action -> 电压在正常范围，无需调整。" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "AVC Coroutine Exception: " << e.what() << std::endl;
    }
    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] AVC: Processed " << event_count << " events. Shutting down." << std::endl;
}

void avc_test()
{
    std::cout << "--- AVC Voltage Control Simulation ---" << std::endl;

    cps_coro::Scheduler scheduler; // 创建调度器

    // 启动 Sensor 协程
    // Task 对象在作用域结束时会自动管理协程句柄的销毁
    // 如果希望协程在 Task 对象销毁后继续运行 (例如，如果 Task 是局部变量)，
    // 则需要调用 task.detach()，并确保协程有其他方式结束或被管理。
    // 在这个例子中，Task 对象生命周期与 main 函数相同，所以不需要 detach。
    cps_coro::Task sensor_task = sensor_coroutine(scheduler);

    // 启动 AVC 协程
    cps_coro::Task avc_task = avc_coroutine(scheduler);

    // 运行调度器直到指定的时间点 (例如30秒)
    // 或者直到所有任务完成。
    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Main: Starting scheduler for 30 seconds." << std::endl;
    scheduler.run_until(scheduler.now() + std::chrono::seconds(30));

    std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Main: Scheduler finished or ran until time limit." << std::endl;

    // 检查协程是否都已完成
    if (sensor_task.is_done()) {
        std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Main: Sensor task completed." << std::endl;
    } else {
        std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Main: Sensor task NOT completed." << std::endl;
    }
    if (avc_task.is_done()) {
        std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Main: AVC task completed." << std::endl;
    } else {
        std::cout << "[" << scheduler.now().time_since_epoch().count() << "ms] Main: AVC task NOT completed." << std::endl;
    }

    std::cout << "--- Simulation End ---" << std::endl;
}
