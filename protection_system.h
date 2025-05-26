// protection_system.h
#ifndef PROTECTION_SYSTEM_H
#define PROTECTION_SYSTEM_H

#include "cps_coro_lib.h"
#include "ecs_core.h"
#include "simulation_events_and_data.h"
// #include <iostream> // Replaced by spdlog
#include <string>
#include <vector>

// Forward declare Scheduler if passed as parameter, or use AwaiterBase for implicit access
// extern cps_coro::Scheduler* g_scheduler; // Assuming g_scheduler is accessible globally or passed

class ProtectiveComp : public IComponent {
public:
    virtual bool pick_up(const FaultInfo& fault_data, Entity self_entity_id) = 0;
    virtual int trip_delay_ms(const FaultInfo& fault_data) const = 0;
    virtual const char* name() const = 0;
    virtual ~ProtectiveComp() = default;
};

class OverCurrentProtection : public ProtectiveComp {
public:
    OverCurrentProtection(double pickup_current_kA, int delay_ms, std::string stage_name = "OC");
    bool pick_up(const FaultInfo& fault_data, Entity self_entity_id) override;
    int trip_delay_ms(const FaultInfo& fault_data) const override;
    const char* name() const override;

private:
    double pickup_current_kA_;
    int fixed_delay_ms_;
    std::string stage_name_;
};

class DistanceProtection : public ProtectiveComp {
public:
    DistanceProtection(double z1_ohm, int t1_ms, double z2_ohm, int t2_ms, double z3_ohm, int t3_ms);
    bool pick_up(const FaultInfo& fault_data, Entity self_entity_id) override;
    int trip_delay_ms(const FaultInfo& fault_data) const override;
    const char* name() const override;

private:
    std::vector<double> z_set_;
    std::vector<int> t_ms_;
};

class ProtectionSystem {
public:
    ProtectionSystem(Registry& reg, cps_coro::Scheduler& sch); // Pass scheduler explicitly
    cps_coro::Task run();
    void inject_fault(const FaultInfo& info);

private:
    cps_coro::Task trip_later(Entity protected_entity_id, int delay_ms, const char* protection_name, Entity actual_faulty_entity_id);
    Registry& registry_;
    cps_coro::Scheduler& scheduler_; // Store reference to scheduler
};

cps_coro::Task faultInjectorTask_prot(ProtectionSystem& protSystem, Entity line1_id, Entity transformer1_id, cps_coro::Scheduler& scheduler); // Pass scheduler
cps_coro::Task circuitBreakerAgentTask_prot(Entity associated_entity_id, const std::string& entity_name, cps_coro::Scheduler& scheduler); // Pass scheduler

#endif // PROTECTION_SYSTEM_H