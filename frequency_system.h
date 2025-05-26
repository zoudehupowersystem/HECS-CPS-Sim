// frequency_system.h
#ifndef FREQUENCY_SYSTEM_H
#define FREQUENCY_SYSTEM_H

#include "cps_coro_lib.h"
#include "ecs_core.h"
#include "simulation_events_and_data.h"
#include <cmath>
#include <string>
#include <vector>
// #include <fstream> // No longer needed for ofstream

struct PhysicalStateComponent : public IComponent {
    double current_power_kW;
    double soc;
    PhysicalStateComponent(double power = 0.0, double s = 0.5);
};

struct FrequencyControlConfigComponent : public IComponent {
    enum class DeviceType { EV_PILE,
        ESS_UNIT };
    DeviceType type;
    double base_power_kW;
    double gain_kW_per_Hz;
    double deadband_Hz;
    double max_output_kW;
    double min_output_kW;
    double soc_min_threshold;
    double soc_max_threshold;

    FrequencyControlConfigComponent(DeviceType t, double base_p, double gain, double db,
        double max_p, double min_p,
        double soc_min = 0.0, double soc_max = 1.0);
};

double calculate_frequency_deviation(double t_relative);

cps_coro::Task frequencyOracleTask(Registry& registry,
    const std::vector<Entity>& ev_entities,
    const std::vector<Entity>& ess_entities,
    double disturbance_start_time_s,
    double simulation_step_ms // Removed std::ofstream& data_logger
);

cps_coro::Task vppFrequencyResponseTask(Registry& registry,
    const std::string& vpp_name,
    const std::vector<Entity>& managed_entities,
    double simulation_step_ms_parameter);

#endif // FREQUENCY_SYSTEM_H