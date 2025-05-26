// main.cpp
#include "cps_coro_lib.h"
#include "ecs_core.h"
#include "frequency_system.h"
#include "logging_utils.h"
#include "protection_system.h"
#include "simulation_events_and_data.h"

#include <chrono>
#include <iomanip> // For formatting output if needed
#include <iostream> // For fallback error messages if spdlog fails
#include <random>
#include <string>
#include <vector>

// Platform-specific headers for memory usage
#if defined(_WIN32)
#include <psapi.h>
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <unistd.h> // For sysconf
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

// Definition of the global scheduler pointer
cps_coro::Scheduler* g_scheduler = nullptr;

// Memory usage function
long get_peak_memory_usage_kb()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    ZeroMemory(&pmc, sizeof(pmc)); // Important to zero out the structure
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return static_cast<long>(pmc.PeakWorkingSetSize / 1024);
    }
    return -1;
#elif defined(__linux__)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss; // Peak resident set size in kilobytes
    }
    return -1;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t taskInfo;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&taskInfo, &infoCount) == KERN_SUCCESS) {
        return static_cast<long>(taskInfo.resident_size_max / 1024); // Peak resident size in KB
    }
    return -1;
#else
    if (g_console_logger)
        g_console_logger->warn("Peak memory usage statistics not readily available for this platform via this function.");
    return -1;
#endif
}

cps_coro::Task generatorTask()
{
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [Generator] Startup sequence initiated.", g_scheduler->now().time_since_epoch().count());
    co_await cps_coro::delay(cps_coro::Scheduler::duration(1000));
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [Generator] Online and stable.", g_scheduler->now().time_since_epoch().count());
    if (g_scheduler)
        g_scheduler->trigger_event(GENERATOR_READY_EVENT);

    while (true) {
        co_await cps_coro::wait_for_event<void>(POWER_ADJUST_REQUEST_EVENT);
        if (g_console_logger && g_scheduler)
            g_console_logger->info("[{}ms] [Generator] Received POWER_ADJUST_REQUEST_EVENT. Adjusting...", g_scheduler->now().time_since_epoch().count());
        co_await cps_coro::delay(cps_coro::Scheduler::duration(300));
        if (g_console_logger && g_scheduler)
            g_console_logger->info("[{}ms] [Generator] Power output adjusted.", g_scheduler->now().time_since_epoch().count());
    }
}

cps_coro::Task loadTask()
{
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [Load] Waiting for GENERATOR_READY_EVENT.", g_scheduler->now().time_since_epoch().count());
    co_await cps_coro::wait_for_event<void>(GENERATOR_READY_EVENT);
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [Load] Generator online. Initial load applied.", g_scheduler->now().time_since_epoch().count());
    co_await cps_coro::delay(cps_coro::Scheduler::duration(500));

    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [Load] Load increased. Triggering LOAD_CHANGE_EVENT.", g_scheduler->now().time_since_epoch().count());
    if (g_scheduler)
        g_scheduler->trigger_event(LOAD_CHANGE_EVENT);

    co_await cps_coro::delay(cps_coro::Scheduler::duration(10000));
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [Load] Load significantly increased. Triggering LOAD_CHANGE_EVENT & STABILITY_CONCERN_EVENT.", g_scheduler->now().time_since_epoch().count());
    if (g_scheduler) {
        g_scheduler->trigger_event(LOAD_CHANGE_EVENT);
        g_scheduler->trigger_event(STABILITY_CONCERN_EVENT);
    }
    co_return;
}

extern void avc_test();

int main()
{
    // avc_test();

    initialize_loggers("vpp_freq_response_data.csv", true);

    cps_coro::Scheduler scheduler_instance;
    g_scheduler = &scheduler_instance; // Initialize global scheduler pointer
    Registry registry;

    if (g_console_logger)
        g_console_logger->info("--- CPS Simulation with spdlog, Event-Driven VPP, Stats ---");
    g_scheduler->set_time(cps_coro::Scheduler::time_point { std::chrono::milliseconds(0) });
    if (g_console_logger)
        g_console_logger->info("Initial Simulation Time: {} ms.", g_scheduler->now().time_since_epoch().count());

    ProtectionSystem protection_system(registry, scheduler_instance);
    Entity line1_prot = registry.create();
    registry.emplace<OverCurrentProtection>(line1_prot, 5.0, 200, "OC-L1P-Fast");
    registry.emplace<DistanceProtection>(line1_prot, 5.0, 0, 15.0, 300, 25.0, 700);
    Entity transformer1_prot = registry.create();
    registry.emplace<OverCurrentProtection>(transformer1_prot, 2.5, 300, "OC-T1P-Main");

    if (g_console_logger)
        g_console_logger->info("Protection entities: Line1_Prot #{}, Transformer1_Prot #{}", line1_prot, transformer1_prot);

    auto prot_sys_run_task = protection_system.run();
    prot_sys_run_task.detach();
    // MODIFICATION: Pass scheduler_instance as the last argument
    auto fault_inject_prot_task = faultInjectorTask_prot(protection_system, line1_prot, transformer1_prot, scheduler_instance);
    fault_inject_prot_task.detach();
    auto breaker_l1p_task = circuitBreakerAgentTask_prot(line1_prot, "Line1_P", scheduler_instance);
    breaker_l1p_task.detach();
    auto breaker_t1p_task = circuitBreakerAgentTask_prot(transformer1_prot, "T1_P", scheduler_instance);
    breaker_t1p_task.detach();
    if (g_console_logger)
        g_console_logger->info("Protection system tasks started.");

    std::vector<Entity> ev_pile_entities_freq;
    std::vector<Entity> ess_unit_entities_freq;
    std::random_device rd_freq;
    std::mt19937 rng_freq(rd_freq());
    std::uniform_real_distribution<double> soc_dist_freq(0.25, 0.9);

    int num_ev_stations = 10;
    int piles_per_station = 5;
    int total_ev_piles = num_ev_stations * piles_per_station;

    for (int i = 0; i < total_ev_piles; ++i) {
        Entity pile = registry.create();
        ev_pile_entities_freq.push_back(pile);
        double initial_soc = soc_dist_freq(rng_freq);
        double scheduled_charging_power_kW;
        if (i % 3 == 0)
            scheduled_charging_power_kW = -5.0;
        else if (i % 3 == 1)
            scheduled_charging_power_kW = -3.5;
        else
            scheduled_charging_power_kW = 0.0;
        registry.emplace<FrequencyControlConfigComponent>(pile, FrequencyControlConfigComponent::DeviceType::EV_PILE, scheduled_charging_power_kW, 4.0, 0.03, 5.0, -5.0, 0.1, 0.95);
        registry.emplace<PhysicalStateComponent>(pile, scheduled_charging_power_kW, initial_soc);
    }
    if (g_console_logger)
        g_console_logger->info("Initialized {} EV charging piles for frequency response.", total_ev_piles);

    int num_ess_units = 100;
    for (int i = 0; i < num_ess_units; ++i) {
        Entity ess = registry.create();
        ess_unit_entities_freq.push_back(ess);
        double ess_gain_kw_per_hz = (1000.0) / (0.03 * 50.0);
        registry.emplace<FrequencyControlConfigComponent>(ess, FrequencyControlConfigComponent::DeviceType::ESS_UNIT, 0.0, ess_gain_kw_per_hz, 0.03, 1000.0, -1000.0, 0.05, 0.95);
        registry.emplace<PhysicalStateComponent>(ess, 0.0, 0.7);
    }
    if (g_console_logger)
        g_console_logger->info("Initialized {} ESS units for frequency response.", num_ess_units);

    double freq_sim_step_ms = 20.0;
    auto freq_oracle_task_main = frequencyOracleTask(registry, ev_pile_entities_freq, ess_unit_entities_freq, 5.0, freq_sim_step_ms);
    freq_oracle_task_main.detach();

    auto ev_vpp_task_main = vppFrequencyResponseTask(registry, "EV_VPP", ev_pile_entities_freq, freq_sim_step_ms);
    ev_vpp_task_main.detach();
    auto ess_vpp_task_main = vppFrequencyResponseTask(registry, "ESS_VPP", ess_unit_entities_freq, freq_sim_step_ms);
    ess_vpp_task_main.detach();
    if (g_console_logger)
        g_console_logger->info("Frequency-power response system tasks started.");

    auto gen_task_main = generatorTask();
    gen_task_main.detach();
    auto load_task_main = loadTask();
    load_task_main.detach();
    if (g_console_logger)
        g_console_logger->info("General background tasks started.");

    auto real_time_sim_start = std::chrono::high_resolution_clock::now();

    cps_coro::Scheduler::duration simulation_duration = std::chrono::milliseconds(70000);
    cps_coro::Scheduler::time_point end_time = g_scheduler->now() + simulation_duration;

    if (g_console_logger)
        g_console_logger->info("\n--- Running Simulation until {} ms --- \n", end_time.time_since_epoch().count());
    g_scheduler->run_until(end_time);

    auto real_time_sim_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> real_time_elapsed_seconds = real_time_sim_end - real_time_sim_start;

    if (g_console_logger) {
        g_console_logger->info("\n--- Simulation Ended --- ");
        g_console_logger->info("Final Simulation Time: {} ms.", g_scheduler->now().time_since_epoch().count());
        g_console_logger->info("Real execution time: {:.3f} seconds.", real_time_elapsed_seconds.count());
    }

    long peak_mem_kb = get_peak_memory_usage_kb();
    if (peak_mem_kb != -1 && g_console_logger) {
        g_console_logger->info("Peak memory usage (approx.): {} KB ({:.2f} MB).", peak_mem_kb, peak_mem_kb / 1024.0);
    } else if (g_console_logger) {
        g_console_logger->warn("Could not retrieve peak memory usage for this platform.");
    }

    if (g_console_logger)
        g_console_logger->info("VPP frequency response data saved to configured file.");
    shutdown_loggers();

    return 0;
}