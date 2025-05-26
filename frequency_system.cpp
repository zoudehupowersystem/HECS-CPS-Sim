// frequency_system.cpp
#include "frequency_system.h"
#include "logging_utils.h" // For g_console_logger, g_data_file_logger
#include <chrono>
#include <cmath> // For std::abs
#include <iomanip> // For std::fixed, std::setprecision if still used by spdlog format indirectly

// Global scheduler pointer - tasks might need access if not passed explicitly
// This should be defined in main.cpp and accessed carefully.
// For simplicity here, we'll assume tasks that need it (like for now()) can get it.
// In a larger system, a context object passed to tasks is cleaner.
extern cps_coro::Scheduler* g_scheduler; // Assume it's accessible

PhysicalStateComponent::PhysicalStateComponent(double power, double s)
    : current_power_kW(power)
    , soc(s)
{
}

FrequencyControlConfigComponent::FrequencyControlConfigComponent(
    DeviceType t, double base_p, double gain, double db,
    double max_p, double min_p, double soc_min, double soc_max)
    : type(t)
    , base_power_kW(base_p)
    , gain_kW_per_Hz(gain)
    , deadband_Hz(db)
    , max_output_kW(max_p)
    , min_output_kW(min_p)
    , soc_min_threshold(soc_min)
    , soc_max_threshold(soc_max)
{
}

const double P_f_coeff_fs = 0.0862;
const double M_f_coeff_fs = 0.1404;
const double M1_f_coeff_fs = 0.1577;
const double M2_f_coeff_fs = 0.0397;
const double N_f_coeff_fs = 0.125;

double calculate_frequency_deviation(double t_relative)
{
    if (t_relative < 0)
        return 0.0;
    double f_dev = -(M_f_coeff_fs + (M1_f_coeff_fs * std::sin(M_f_coeff_fs * t_relative) - M_f_coeff_fs * std::cos(M_f_coeff_fs * t_relative)))
        / M2_f_coeff_fs * std::exp(-N_f_coeff_fs * t_relative) * P_f_coeff_fs;
    return f_dev;
}

cps_coro::Task frequencyOracleTask(Registry& registry,
    const std::vector<Entity>& ev_entities,
    const std::vector<Entity>& ess_entities,
    double disturbance_start_time_s,
    double simulation_step_ms)
{
    if (g_console_logger)
        g_console_logger->info("[{:.1f}ms] [FreqOracle] Active. Disturbance at {}s. Step: {}ms.",
            (g_scheduler ? g_scheduler->now().time_since_epoch().count() / 1.0 : 0.0),
            disturbance_start_time_s, simulation_step_ms);

    if (g_data_file_logger) {
        g_data_file_logger->info("# SimTime_ms\tSimTime_s\tRelativeTime_s\tFreqDeviation_Hz\tTotalVppPower_kW");
    }

    while (true) {
        co_await cps_coro::delay(cps_coro::Scheduler::duration(static_cast<long long>(simulation_step_ms)));

        double current_sim_time_ms = (g_scheduler ? g_scheduler->now().time_since_epoch().count() : 0.0);
        double current_sim_time_s = current_sim_time_ms / 1000.0;
        double relative_time_s = current_sim_time_s - disturbance_start_time_s;
        double freq_dev_hz = calculate_frequency_deviation(relative_time_s);

        FrequencyInfo freq_info;
        freq_info.current_sim_time_seconds = current_sim_time_s;
        freq_info.freq_deviation_hz = freq_dev_hz;

        if (g_scheduler) {
            g_scheduler->trigger_event(FREQUENCY_UPDATE_EVENT, freq_info);
        }

        double total_vpp_power_kw = 0;
        for (Entity entity_id : ev_entities) {
            if (auto state = registry.get<PhysicalStateComponent>(entity_id)) {
                total_vpp_power_kw += state->current_power_kW;
            }
        }
        for (Entity entity_id : ess_entities) {
            if (auto state = registry.get<PhysicalStateComponent>(entity_id)) {
                total_vpp_power_kw += state->current_power_kW;
            }
        }

        if (g_data_file_logger) {
            g_data_file_logger->info("{:.0f}\t{:.3f}\t{:.3f}\t{:.5f}\t{:.2f}",
                current_sim_time_ms,
                current_sim_time_s,
                relative_time_s,
                freq_dev_hz,
                total_vpp_power_kw);
        }
    }
}

cps_coro::Task vppFrequencyResponseTask(Registry& registry,
    const std::string& vpp_name,
    const std::vector<Entity>& managed_entities,
    double /*simulation_step_ms_parameter*/) // This parameter is less directly used now for SOC dt
{
    if (g_console_logger)
        g_console_logger->info("[{:.1f}ms] [VPP-{}] Active with event-driven updates. Awaiting FREQUENCY_UPDATE_EVENT.",
            (g_scheduler ? g_scheduler->now().time_since_epoch().count() / 1.0 : 0.0), vpp_name);

    double last_processed_event_time_s = -1.0;
    double vpp_instance_last_full_update_time_s = -1.0;
    double vpp_instance_last_full_update_freq_dev_hz = 0.0;

    const double FREQUENCY_CHANGE_THRESHOLD_HZ = 0.01;
    const double TIME_THRESHOLD_SECONDS = 1.0;

    while (true) {
        FrequencyInfo current_freq_info = co_await cps_coro::wait_for_event<FrequencyInfo>(FREQUENCY_UPDATE_EVENT);

        if (current_freq_info.current_sim_time_seconds <= last_processed_event_time_s) {
            continue;
        }
        last_processed_event_time_s = current_freq_info.current_sim_time_seconds;

        bool perform_full_update = false;
        double dt_since_last_full_update = 0.0;

        if (vpp_instance_last_full_update_time_s < 0) {
            perform_full_update = true;
        } else {
            dt_since_last_full_update = current_freq_info.current_sim_time_seconds - vpp_instance_last_full_update_time_s;
            if (dt_since_last_full_update < 0)
                dt_since_last_full_update = 0; // Safety

            double freq_diff_abs = std::abs(current_freq_info.freq_deviation_hz - vpp_instance_last_full_update_freq_dev_hz);

            if (freq_diff_abs > FREQUENCY_CHANGE_THRESHOLD_HZ) {
                perform_full_update = true;
            }
            if (dt_since_last_full_update >= TIME_THRESHOLD_SECONDS) {
                perform_full_update = true;
            }
        }

        if (perform_full_update) {
            // if(g_console_logger) g_console_logger->debug( // Example debug log
            //     "[{:.1f}ms] [VPP-{}] Criteria met. Full update. SimTime: {:.3f}s. FreqDev: {:.5f}Hz. dt_full: {:.3f}s.",
            //     (g_scheduler ? g_scheduler->now().time_since_epoch().count() / 1.0 : 0.0),
            //     vpp_name, current_freq_info.current_sim_time_seconds,
            //     current_freq_info.freq_deviation_hz, dt_since_last_full_update);

            for (Entity entity_id : managed_entities) {
                auto config = registry.get<FrequencyControlConfigComponent>(entity_id);
                auto state = registry.get<PhysicalStateComponent>(entity_id);

                if (!config || !state)
                    continue;

                if (vpp_instance_last_full_update_time_s >= 0 && dt_since_last_full_update > 1e-6) { // Avoid zero dt if not first update
                    double power_during_last_interval = state->current_power_kW;
                    double energy_change_kWh = power_during_last_interval * (dt_since_last_full_update / 3600.0);

                    if (config->type == FrequencyControlConfigComponent::DeviceType::EV_PILE) {
                        double typical_battery_capacity_kWh = 50.0;
                        if (typical_battery_capacity_kWh > 0)
                            state->soc -= (energy_change_kWh / typical_battery_capacity_kWh);
                    } else if (config->type == FrequencyControlConfigComponent::DeviceType::ESS_UNIT) {
                        double typical_ess_capacity_kWh = 2000.0;
                        if (typical_ess_capacity_kWh > 0)
                            state->soc -= (energy_change_kWh / typical_ess_capacity_kWh);
                    }
                    state->soc = std::max(0.0, std::min(1.0, state->soc));
                }

                double new_calculated_power_kW = config->base_power_kW;
                double current_actual_freq_dev = current_freq_info.freq_deviation_hz;
                double current_abs_actual_freq_dev = std::abs(current_actual_freq_dev);

                if (current_abs_actual_freq_dev > config->deadband_Hz) {
                    if (current_actual_freq_dev < 0) { // Frequency DROPPED
                        double effective_df_drop = current_actual_freq_dev + config->deadband_Hz;
                        if (config->type == FrequencyControlConfigComponent::DeviceType::EV_PILE) {
                            if (state->soc >= config->soc_min_threshold) {
                                new_calculated_power_kW = -config->gain_kW_per_Hz * effective_df_drop;
                            } else if (config->base_power_kW < 0) {
                                new_calculated_power_kW = 0.0;
                            } // else stays base_power_kW
                        } else if (config->type == FrequencyControlConfigComponent::DeviceType::ESS_UNIT) {
                            new_calculated_power_kW = -config->gain_kW_per_Hz * effective_df_drop;
                        }
                    } else { // Frequency RISEN
                        double effective_df_rise = current_actual_freq_dev - config->deadband_Hz;
                        double power_change_due_to_freq = -config->gain_kW_per_Hz * effective_df_rise;
                        new_calculated_power_kW = config->base_power_kW + power_change_due_to_freq;
                    }
                } // else: new_calculated_power_kW remains config->base_power_kW

                new_calculated_power_kW = std::max(config->min_output_kW, std::min(config->max_output_kW, new_calculated_power_kW));

                if (config->type == FrequencyControlConfigComponent::DeviceType::EV_PILE) {
                    if (new_calculated_power_kW < 0 && state->soc >= config->soc_max_threshold)
                        new_calculated_power_kW = 0.0;
                    if (new_calculated_power_kW > 0 && state->soc <= config->soc_min_threshold)
                        new_calculated_power_kW = 0.0;
                }
                state->current_power_kW = new_calculated_power_kW;
            }

            vpp_instance_last_full_update_time_s = current_freq_info.current_sim_time_seconds;
            vpp_instance_last_full_update_freq_dev_hz = current_freq_info.freq_deviation_hz;
        }
    }
}