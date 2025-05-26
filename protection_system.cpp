// protection_system.cpp
#include "protection_system.h"
#include "logging_utils.h" // For g_console_logger
#include <chrono>

extern cps_coro::Scheduler* g_scheduler; // Assuming main.cpp defines this and it's accessible

OverCurrentProtection::OverCurrentProtection(double pickup_current_kA, int delay_ms, std::string stage_name)
    : pickup_current_kA_(pickup_current_kA)
    , fixed_delay_ms_(delay_ms)
    , stage_name_(std::move(stage_name))
{
}

bool OverCurrentProtection::pick_up(const FaultInfo& fault_data, Entity /*self_entity_id*/)
{
    return fault_data.current_kA >= pickup_current_kA_;
}
int OverCurrentProtection::trip_delay_ms(const FaultInfo& /*fault_data*/) const
{
    return fixed_delay_ms_;
}
const char* OverCurrentProtection::name() const
{
    return stage_name_.c_str();
}

DistanceProtection::DistanceProtection(double z1_ohm, int t1_ms, double z2_ohm, int t2_ms, double z3_ohm, int t3_ms)
    : z_set_ { z1_ohm, z2_ohm, z3_ohm }
    , t_ms_ { t1_ms, t2_ms, t3_ms }
{
}

bool DistanceProtection::pick_up(const FaultInfo& fault_data, Entity self_entity_id)
{
    if (fault_data.faulty_entity_id != self_entity_id && fault_data.faulty_entity_id != 0) {
        return fault_data.impedance_Ohm <= z_set_[2];
    }
    return fault_data.impedance_Ohm <= z_set_[0] || fault_data.impedance_Ohm <= z_set_[1] || fault_data.impedance_Ohm <= z_set_[2];
}
int DistanceProtection::trip_delay_ms(const FaultInfo& fault_data) const
{
    if (fault_data.impedance_Ohm <= z_set_[0])
        return t_ms_[0];
    if (fault_data.impedance_Ohm <= z_set_[1])
        return t_ms_[1];
    if (fault_data.impedance_Ohm <= z_set_[2])
        return t_ms_[2];
    return 99999;
}
const char* DistanceProtection::name() const { return "DIST"; }

ProtectionSystem::ProtectionSystem(Registry& reg, cps_coro::Scheduler& sch)
    : registry_(reg)
    , scheduler_(sch)
{
}

void ProtectionSystem::inject_fault(const FaultInfo& info)
{
    scheduler_.trigger_event(FAULT_INFO_EVENT_PROT, info);
}

cps_coro::Task ProtectionSystem::run()
{
    if (g_console_logger)
        g_console_logger->info("[{}ms] [ProtectionSystem] ECS Protection System active, awaiting FAULT_INFO_EVENT_PROT.",
            scheduler_.now().time_since_epoch().count());
    while (true) {
        auto fault_data = co_await cps_coro::wait_for_event<FaultInfo>(FAULT_INFO_EVENT_PROT);
        fault_data.calculate_impedance_if_needed();

        if (g_console_logger)
            g_console_logger->info("[{}ms] [ProtectionSystem] Received FAULT_INFO_EVENT_PROT. Fault on Entity #{} (Current: {}kA, Impedance: {}Ohm, Dist: {}km).",
                scheduler_.now().time_since_epoch().count(),
                fault_data.faulty_entity_id, fault_data.current_kA,
                fault_data.impedance_Ohm, fault_data.distance_km);

        registry_.for_each<ProtectiveComp>([&](ProtectiveComp& comp, Entity entity_id) {
            if (comp.pick_up(fault_data, entity_id)) {
                int delay_ms = comp.trip_delay_ms(fault_data);
                if (g_console_logger)
                    g_console_logger->info("[{}ms] [Prot-{}] Entity#{} PICKED UP. Calculated trip delay: {} ms.",
                        scheduler_.now().time_since_epoch().count(),
                        comp.name(), entity_id, delay_ms);
                auto sub_task = trip_later(entity_id, delay_ms, comp.name(), fault_data.faulty_entity_id);
                sub_task.detach();
            }
        });
    }
}

cps_coro::Task ProtectionSystem::trip_later(Entity protected_entity_id, int delay_ms, const char* protection_name, Entity actual_faulty_entity_id)
{
    co_await cps_coro::delay(cps_coro::Scheduler::duration(delay_ms));
    if (g_console_logger)
        g_console_logger->info("[{}ms] [Prot-{}] Entity#{} => TRIPPING! (Due to fault on Entity#{})",
            scheduler_.now().time_since_epoch().count(),
            protection_name, protected_entity_id, actual_faulty_entity_id);
    scheduler_.trigger_event(ENTITY_TRIP_EVENT_PROT, protected_entity_id);
}

// If these tasks need scheduler access and g_scheduler is not preferred, pass it.
// For now, assuming g_scheduler is accessible or scheduler_ passed to ProtectionSystem is used.
cps_coro::Task faultInjectorTask_prot(ProtectionSystem& protSystem, Entity line1_id, Entity transformer1_id, cps_coro::Scheduler& /*scheduler_ref_for_logging_time*/)
{
    // Using g_scheduler for logging time for simplicity, or pass scheduler if g_scheduler is to be avoided
    co_await cps_coro::delay(cps_coro::Scheduler::duration(6000));
    FaultInfo fault1;
    fault1.faulty_entity_id = line1_id;
    fault1.current_kA = 15.0; // ... rest of fault1 setup
    fault1.voltage_kV = 220.0;
    fault1.distance_km = 10.0;
    fault1.impedance_Ohm = (220.0 / 15.0) * 0.8; // Example impedance
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [FaultInjector_PROT] Injecting Fault #1 on Line Entity#{}.",
            g_scheduler->now().time_since_epoch().count(), line1_id);
    protSystem.inject_fault(fault1);

    co_await cps_coro::delay(cps_coro::Scheduler::duration(7000));
    FaultInfo fault2;
    fault2.faulty_entity_id = transformer1_id;
    fault2.current_kA = 3.0; // ... rest of fault2 setup
    fault2.voltage_kV = 220.0;
    fault2.calculate_impedance_if_needed(); // Calculate if not manually set
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [FaultInjector_PROT] Injecting Fault #2 on Transformer Entity#{}.",
            g_scheduler->now().time_since_epoch().count(), transformer1_id);
    protSystem.inject_fault(fault2);
    co_return;
}

cps_coro::Task circuitBreakerAgentTask_prot(Entity associated_entity_id, const std::string& entity_name, cps_coro::Scheduler& /*scheduler_ref_for_logging_time*/)
{
    if (g_console_logger && g_scheduler)
        g_console_logger->info("[{}ms] [BreakerAgent_PROT-{}-#{}] Active, awaiting ENTITY_TRIP_EVENT_PROT.",
            g_scheduler->now().time_since_epoch().count(), entity_name, associated_entity_id);
    while (true) {
        Entity tripped_entity_id = co_await cps_coro::wait_for_event<Entity>(ENTITY_TRIP_EVENT_PROT);
        if (tripped_entity_id == associated_entity_id) {
            if (g_console_logger && g_scheduler)
                g_console_logger->info("[{}ms] [BreakerAgent_PROT-{}-#{}] Received TRIP for self.",
                    g_scheduler->now().time_since_epoch().count(), entity_name, associated_entity_id);
            co_await cps_coro::delay(cps_coro::Scheduler::duration(100)); // Breaker operating time
            if (g_console_logger && g_scheduler)
                g_console_logger->info("[{}ms] [BreakerAgent_PROT-{}-#{}] Breaker OPENED.",
                    g_scheduler->now().time_since_epoch().count(), entity_name, associated_entity_id);
            if (g_scheduler) {
                g_scheduler->trigger_event(BREAKER_OPENED_EVENT, associated_entity_id);
            }
        }
    }
}