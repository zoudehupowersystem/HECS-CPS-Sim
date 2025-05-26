#include <atomic>
#include <chrono> // 用于时间统计
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// 平台相关的头文件，用于内存统计
#include <sys/resource.h> // For getrusage
#include <unistd.h> // For sysconf (if needed for page size, though getrusage gives KB directly for ru_maxrss on Linux)

const int NUM_EV_STATIONS = 10; // 模拟的电动汽车充电站数量
const int PILES_PER_STATION = 5; // 每个充电站包含的充电桩数量
const int NUM_ESS_UNITS = 2; // 模拟的分布式储能单元数量
const double SIMULATION_DURATION_SECONDS = 10.0; // 仿真总时长（模拟的秒数）
const double FREQUENCY_UPDATE_INTERVAL_MS = 20.0; // 频率信息更新的时间间隔（毫秒），也即设备线程的响应周期
const double DISTURBANCE_START_TIME_S = 1.0; // 频率扰动开始的仿真时间点（秒）

// --- 共享频率数据结构 ---
struct SharedFrequencyData {
    double current_freq_deviation_hz = 0.0;
    std::mutex mtx;
    std::condition_variable cv;
    long long current_sim_time_ms = 0;
};

// --- 设备类型 ---
enum class DeviceType { EV_PILE,
    ESS_UNIT };

// --- 设备配置 ---
struct DeviceConfig {
    DeviceType type;
    double base_power_kW;
    double gain_kW_per_Hz;
    double deadband_Hz;
    double max_output_kW;
    double min_output_kW;
    double soc_min_threshold;
    double soc_max_threshold;
    double battery_capacity_kWh;
};

// --- 设备状态 ---
struct DeviceState {
    double current_power_kW = 0.0;
    double soc = 0.5;
};

// --- 全局共享变量 ---
std::atomic<double> g_total_vpp_power_kw(0.0);
std::atomic<bool> g_simulation_running(true);
std::ofstream g_data_logger("traditional_threaded_vpp_results.csv");

// --- 内存统计函数 ---
long get_peak_memory_usage_kb_traditional()
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss; // Peak resident set size in kilobytes
    }
    return -1;
}

// --- 频率计算函数 (与您项目中一致) ---
const double P_f_coeff_fs_trad = 0.0862; // Renamed to avoid potential ODR violations if linked with other files
const double M_f_coeff_fs_trad = 0.1404;
const double M1_f_coeff_fs_trad = 0.1577;
const double M2_f_coeff_fs_trad = 0.0397;
const double N_f_coeff_fs_trad = 0.125;

double calculate_frequency_deviation_traditional(double t_relative)
{ // Renamed
    if (t_relative < 0)
        return 0.0;
    double f_dev = -(M_f_coeff_fs_trad + (M1_f_coeff_fs_trad * std::sin(M_f_coeff_fs_trad * t_relative) - M_f_coeff_fs_trad * std::cos(M_f_coeff_fs_trad * t_relative)))
        / M2_f_coeff_fs_trad * std::exp(-N_f_coeff_fs_trad * t_relative) * P_f_coeff_fs_trad;
    return f_dev;
}

// --- 设备线程函数 ---
void device_thread_func(int device_id, DeviceConfig config, SharedFrequencyData& freq_data)
{
    DeviceState state;
    // 使用随机数初始化SOC，使其更具多样性
    std::random_device rd;
    std::mt19937 gen(rd() + device_id); // 为每个线程使用不同的种子
    std::uniform_real_distribution<> distrib_soc(0.3, 0.8);
    state.soc = distrib_soc(gen);

    state.current_power_kW = config.base_power_kW;
    g_total_vpp_power_kw += state.current_power_kW;

    long long last_update_sim_time_ms = 0;

    while (g_simulation_running.load()) {
        double freq_dev_to_use;
        long long current_sim_time_ms_local;

        {
            std::unique_lock<std::mutex> lock(freq_data.mtx);
            freq_data.cv.wait(lock, [&]() {
                return freq_data.current_sim_time_ms > last_update_sim_time_ms || !g_simulation_running.load();
            });

            if (!g_simulation_running.load()) {
                break;
            }
            freq_dev_to_use = freq_data.current_freq_deviation_hz;
            current_sim_time_ms_local = freq_data.current_sim_time_ms;
        }

        double old_power_kw = state.current_power_kW;
        double new_calculated_power_kW = config.base_power_kW;
        double abs_freq_dev = std::abs(freq_dev_to_use);

        if (abs_freq_dev > config.deadband_Hz) {
            if (freq_dev_to_use < 0) {
                double effective_df_drop = freq_dev_to_use + config.deadband_Hz;
                if (config.type == DeviceType::EV_PILE) {
                    if (state.soc >= config.soc_min_threshold) {
                        new_calculated_power_kW = -config.gain_kW_per_Hz * effective_df_drop;
                    } else {
                        if (config.base_power_kW < 0)
                            new_calculated_power_kW = 0.0;
                    }
                } else {
                    new_calculated_power_kW = -config.gain_kW_per_Hz * effective_df_drop;
                }
            } else {
                double effective_df_rise = freq_dev_to_use - config.deadband_Hz;
                double power_change = -config.gain_kW_per_Hz * effective_df_rise;
                new_calculated_power_kW = config.base_power_kW + power_change;
            }
        }

        new_calculated_power_kW = std::max(config.min_output_kW, std::min(config.max_output_kW, new_calculated_power_kW));

        if (config.type == DeviceType::EV_PILE) {
            if (new_calculated_power_kW < 0 && state.soc >= config.soc_max_threshold)
                new_calculated_power_kW = 0.0;
            if (new_calculated_power_kW > 0 && state.soc <= config.soc_min_threshold)
                new_calculated_power_kW = 0.0;
        }

        g_total_vpp_power_kw += (new_calculated_power_kW - old_power_kw);
        state.current_power_kW = new_calculated_power_kW;

        double dt_hours = (FREQUENCY_UPDATE_INTERVAL_MS / 1000.0) / 3600.0;
        double energy_change_kWh = state.current_power_kW * dt_hours;
        if (config.battery_capacity_kWh > 1e-6) {
            state.soc -= (energy_change_kWh / config.battery_capacity_kWh);
        }
        state.soc = std::max(0.0, std::min(1.0, state.soc));

        last_update_sim_time_ms = current_sim_time_ms_local;
    }
    g_total_vpp_power_kw -= state.current_power_kW; // 线程结束前，把自己贡献的功率减掉
}

// --- 频率神谕线程函数 ---
void frequency_oracle_thread_func(SharedFrequencyData& freq_data)
{
    long long current_sim_time_ms = 0;
    g_data_logger << "# SimTime_ms\tSimTime_s\tRelativeTime_s\tFreqDeviation_Hz\tTotalVppPower_kW\n";

    while (g_simulation_running.load()) {
        double sim_time_s = static_cast<double>(current_sim_time_ms) / 1000.0;
        double relative_time_s = sim_time_s - DISTURBANCE_START_TIME_S;
        double freq_dev = calculate_frequency_deviation_traditional(relative_time_s); // 使用重命名后的函数

        {
            std::lock_guard<std::mutex> lock(freq_data.mtx);
            freq_data.current_freq_deviation_hz = freq_dev;
            freq_data.current_sim_time_ms = current_sim_time_ms;
        }
        freq_data.cv.notify_all();

        g_data_logger << current_sim_time_ms << "\t"
                      << std::fixed << std::setprecision(3) << sim_time_s << "\t"
                      << std::fixed << std::setprecision(3) << relative_time_s << "\t"
                      << std::fixed << std::setprecision(5) << freq_dev << "\t"
                      << std::fixed << std::setprecision(2) << g_total_vpp_power_kw.load() << "\n";

        if (sim_time_s >= SIMULATION_DURATION_SECONDS) {
            g_simulation_running = false;
            freq_data.cv.notify_all();
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(FREQUENCY_UPDATE_INTERVAL_MS)));
        current_sim_time_ms += static_cast<long long>(FREQUENCY_UPDATE_INTERVAL_MS);
    }
    // std::cout << "[Oracle] Simulation finished notification." << std::endl; // 可以取消注释用于调试
    g_simulation_running = false; // 确保再次设置
    freq_data.cv.notify_all(); // 确保所有等待的线程都能退出
}

int main()
{
    std::cout << "--- Simplified Traditional Threaded VPP Simulation (with Stats) ---" << std::endl;
    std::cout << "WARNING: Creating many threads ("
              << (NUM_EV_STATIONS * PILES_PER_STATION + NUM_ESS_UNITS)
              << ") can be very slow and resource-intensive." << std::endl;

    // 记录开始时间
    auto real_time_sim_start = std::chrono::high_resolution_clock::now();

    SharedFrequencyData shared_freq_data;
    std::vector<std::thread> device_threads;

    std::thread oracle_thread(frequency_oracle_thread_func, std::ref(shared_freq_data));

    int device_id_counter = 0;
    for (int i = 0; i < NUM_EV_STATIONS; ++i) {
        for (int j = 0; j < PILES_PER_STATION; ++j) {
            DeviceConfig ev_config;
            ev_config.type = DeviceType::EV_PILE;
            ev_config.base_power_kW = (device_id_counter % 3 == 0) ? 0.0 : ((device_id_counter % 3 == 1) ? -3.5 : -5.0); // 更多样的基础功率
            ev_config.gain_kW_per_Hz = 4.0;
            ev_config.deadband_Hz = 0.03;
            ev_config.max_output_kW = 5.0;
            ev_config.min_output_kW = -5.0;
            ev_config.soc_min_threshold = 0.1; // 与HECS版本一致
            ev_config.soc_max_threshold = 0.95; // 与HECS版本一致
            ev_config.battery_capacity_kWh = 50.0;

            device_threads.emplace_back(device_thread_func, device_id_counter++, ev_config, std::ref(shared_freq_data));
        }
    }

    for (int i = 0; i < NUM_ESS_UNITS; ++i) {
        DeviceConfig ess_config;
        ess_config.type = DeviceType::ESS_UNIT;
        ess_config.base_power_kW = 0.0;
        ess_config.gain_kW_per_Hz = 1000.0 / (0.03 * 50.0);
        ess_config.deadband_Hz = 0.03;
        ess_config.max_output_kW = 1000.0;
        ess_config.min_output_kW = -1000.0;
        ess_config.soc_min_threshold = 0.05; // 与HECS版本一致
        ess_config.soc_max_threshold = 0.95; // 与HECS版本一致
        ess_config.battery_capacity_kWh = 2000.0;

        device_threads.emplace_back(device_thread_func, device_id_counter++, ess_config, std::ref(shared_freq_data));
    }

    std::cout << "Launched " << device_threads.size() << " device threads." << std::endl;
    std::cout << "Simulation running for " << SIMULATION_DURATION_SECONDS << " seconds (simulated time)..." << std::endl;

    if (oracle_thread.joinable()) {
        oracle_thread.join();
    }
    // std::cout << "Oracle thread joined." << std::endl; // 可以取消注释用于调试

    for (auto& th : device_threads) {
        if (th.joinable()) {
            th.join();
        }
    }
    // std::cout << "All device threads joined." << std::endl; // 可以取消注释用于调试

    if (g_data_logger.is_open()) {
        g_data_logger.close();
    }

    // 记录结束时间并计算耗时
    auto real_time_sim_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> real_time_elapsed_seconds = real_time_sim_end - real_time_sim_start;

    std::cout << "\n--- Traditional Threaded Simulation Ended --- " << std::endl;
    std::cout << "Simulated duration: " << SIMULATION_DURATION_SECONDS << " s." << std::endl;
    std::cout << "Real execution time: " << std::fixed << std::setprecision(3) << real_time_elapsed_seconds.count() << " seconds." << std::endl;

    long peak_mem_kb = get_peak_memory_usage_kb_traditional();
    if (peak_mem_kb != -1) {
        std::cout << "Peak memory usage (approx.): " << peak_mem_kb << " KB ("
                  << std::fixed << std::setprecision(2) << peak_mem_kb / 1024.0 << " MB)." << std::endl;
    }

    std::cout << "Results saved to traditional_threaded_vpp_results.csv" << std::endl;

    return 0;
}
