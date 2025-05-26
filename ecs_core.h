// ecs_core.h
#ifndef ECS_CORE_H
#define ECS_CORE_H

#include <cstdint>
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>

using Entity = uint64_t;

class IComponent {
public:
    virtual ~IComponent() = default;
};

class Registry {
public:
    Entity create() { return ++last_id_; }

    template <typename Comp, typename... Args>
    Comp& emplace(Entity e, Args&&... args)
    {
        static_assert(std::is_base_of<IComponent, Comp>::value, "Component must inherit from IComponent");
        auto ptr = std::make_unique<Comp>(std::forward<Args>(args)...);
        Comp* raw_ptr = ptr.get();
        components_[typeid(Comp).hash_code()][e] = std::move(ptr);
        return *raw_ptr;
    }

    template <typename Comp>
    Comp* get(Entity e)
    {
        static_assert(std::is_base_of<IComponent, Comp>::value, "Component must inherit from IComponent");
        auto comp_map_it = components_.find(typeid(Comp).hash_code());
        if (comp_map_it == components_.end()) {
            return nullptr;
        }
        auto entity_map_it = comp_map_it->second.find(e);
        if (entity_map_it == comp_map_it->second.end()) {
            return nullptr;
        }
        return static_cast<Comp*>(entity_map_it->second.get());
    }

    template <typename Comp, typename Fn>
    void for_each(Fn&& fn)
    {
        static_assert(std::is_base_of<IComponent, Comp>::value, "Component must inherit from IComponent");
        auto comp_map_it = components_.find(typeid(Comp).hash_code());
        if (comp_map_it != components_.end()) {
            for (auto const& [entity, component_ptr] : comp_map_it->second) {
                fn(static_cast<Comp&>(*component_ptr), entity);
            }
        }
    }

private:
    Entity last_id_ { 0 };
    std::unordered_map<size_t, std::unordered_map<Entity, std::unique_ptr<IComponent>>> components_;
};

#endif // ECS_CORE_H