// simulation_events_and_data.h
#ifndef SIMULATION_EVENTS_AND_DATA_H
#define SIMULATION_EVENTS_AND_DATA_H

#include "cps_coro_lib.h" // For cps_coro::EventId
#include "ecs_core.h" // For Entity
#include <string>

// The global scheduler (g_scheduler) is defined in main.cpp.
// Tasks or functions needing access to it, if not through AwaiterBase,
// would typically have it passed as a parameter or through a context object.
// For this example, we'll assume tasks get it implicitly or it's passed where needed.
// extern cps_coro::Scheduler* g_scheduler; // REMOVED - better managed.

// --- 通用仿真事件ID ---
constexpr cps_coro::EventId GENERATOR_READY_EVENT = 1;
constexpr cps_coro::EventId LOAD_CHANGE_EVENT = 2;
constexpr cps_coro::EventId BREAKER_OPENED_EVENT = 6;
constexpr cps_coro::EventId STABILITY_CONCERN_EVENT = 7;
constexpr cps_coro::EventId LOAD_SHED_REQUEST_EVENT = 8;
constexpr cps_coro::EventId POWER_ADJUST_REQUEST_EVENT = 9;

// --- 保护系统专用事件ID ---
constexpr cps_coro::EventId FAULT_INFO_EVENT_PROT = 100;
constexpr cps_coro::EventId ENTITY_TRIP_EVENT_PROT = 101;

// --- 频率-有功响应系统专用事件ID ---
constexpr cps_coro::EventId FREQUENCY_UPDATE_EVENT = 200;

// --- 核心数据结构 ---
struct FaultInfo {
    double current_kA = 0.0;
    double voltage_kV = 220.0;
    double impedance_Ohm = 0.0;
    double distance_km = 0.0;
    Entity faulty_entity_id = 0;

    void calculate_impedance_if_needed()
    {
        if (impedance_Ohm == 0.0 && voltage_kV > 0 && current_kA > 0) {
            impedance_Ohm = (voltage_kV * 1000.0) / (current_kA * 1000.0);
        }
    }
};

struct FrequencyInfo {
    double current_sim_time_seconds;
    double freq_deviation_hz;
};

#endif // SIMULATION_EVENTS_AND_DATA_H