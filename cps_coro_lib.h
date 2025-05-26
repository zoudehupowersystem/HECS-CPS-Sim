// cps_coro_lib.h
// 轻量级的、仅包含头文件的 C++20 协程库
// 作者：zoudehu
// 用于多任务处理和事件驱动编程。它专为需要显式控制任务执行和时间进展的模拟或应用程序而设计
// 如何使用:
// -------------
// 1. 定义返回 `cps_coro::Task` 的函数以创建协程。
// 2. 在这些协程内部，使用 `co_await` 配合 `cps_coro::delay` 进行基于时间的挂起，
//    或配合 `cps_coro::wait_for_event` 等待特定事件。
// 3. 创建一个 `cps_coro::Scheduler` 实例。
// 4. 实例化你的协程 `Task`。当在调度器上下文中使用可等待对象时，调度器通过
//    线程局部指针隐式关联。
// 5. 调用调度器的方法，如 `run_one_step()` 或 `run_until(time_point)` 来执行已调度的任务。
// 6. 使用 `scheduler.trigger_event(event_id, data)` 或
//    `scheduler.trigger_event(event_id)` 来触发事件。
//
// 为构建离散事件模拟或其他合作式协程的系统提供一个简单而灵活的框架

#ifndef CPS_CORO_LIB_H
#define CPS_CORO_LIB_H

#include <chrono> // 用于时间和持续时间
#include <coroutine> // 用于协程支持
#include <cstdint> // 用于固定宽度的整数类型，如 uint64_t
#include <exception> // 用于异常处理，如 std::terminate
#include <functional> // 用于 std::function
#include <map> // 用于 std::multimap
#include <memory> // 用于智能指针 (虽然在此文件中未直接使用，但常与协程库一起使用)
#include <queue> // 用于 std::queue
#include <utility> // 用于 std::pair, std::move 等
#include <variant> // 用于 std::variant (虽然在此文件中未直接使用)
#include <vector> // 用于 std::vector

namespace cps_coro { // 协程相关的命名空间

// 前向声明 Scheduler 类
class Scheduler;

// 协程等待体的基类
// AwaiterBase 是所有具体等待体 (Awaiter) 的基类。
// 它提供了一个静态线程局部变量 active_scheduler_，用于让等待体能够访问当前的调度器实例。
// 友元类 Scheduler 可以访问其成员。
class AwaiterBase {
protected:
    // 当前线程活动的调度器指针，由 Scheduler 构造时设置
    inline static thread_local Scheduler* active_scheduler_ = nullptr;
    // 允许 Scheduler 类访问 AwaiterBase 的保护成员
    friend class Scheduler;
    // 默认构造函数
    AwaiterBase() = default;
};

// 事件ID类型定义，使用64位无符号整数
using EventId = uint64_t;

// Task 类代表一个协程任务
// Task 是一个无返回值的协程封装。它提供了协程的句柄管理，包括创建、销毁、移动和恢复执行。
// promise_type 是协程的约定类型，定义了协程如何创建、如何处理返回值和异常等。
class Task {
public:
    // 协程的 promise_type，用于与编译器交互
    struct promise_type {
        // 当协程启动时，返回一个 Task 对象
        Task get_return_object()
        {
            return Task { std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        // 协程初始挂起策略：不挂起，立即执行
        std::suspend_never initial_suspend() { return {}; }
        // 协程最终挂起策略：不挂起，协程结束后自动清理
        std::suspend_always final_suspend() noexcept { return {}; }
        // 协程返回 void 时的处理
        void return_void() { }
        // 未处理异常的处理：终止程序
        void unhandled_exception() { std::terminate(); }
    };

    // 默认构造函数
    Task() = default;
    // 通过协程句柄构造 Task
    explicit Task(std::coroutine_handle<promise_type> handle)
        : handle_(handle)
    {
    }

    // 析构函数：如果协程句柄有效且未完成，则销毁它
    ~Task()
    {
        if (handle_ && !handle_.done()) {
            handle_.destroy();
        }
    }

    // 禁止拷贝构造
    Task(const Task&) = delete;
    // 禁止拷贝赋值
    Task& operator=(const Task&) = delete;

    // 移动构造函数
    Task(Task&& other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = nullptr; // 原对象的句柄置空，防止重复销毁
    }

    // 移动赋值运算符
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other) {
            // 如果当前 Task 持有有效且未完成的协程，先销毁它
            if (handle_ && !handle_.done()) {
                handle_.destroy();
            }
            handle_ = other.handle_; // 拥有新的协程句柄
            other.handle_ = nullptr; // 原对象的句柄置空
        }
        return *this;
    }

    // 分离协程句柄，调用后 Task 对象不再负责协程的生命周期
    // 注意：分离后需要外部机制确保协程最终被销毁，否则可能导致资源泄漏
    void detach()
    {
        handle_ = nullptr;
    }

    // 检查协程是否已完成
    bool is_done() const
    {
        return !handle_ || handle_.done();
    }

    // 恢复协程执行 (如果协程已挂起且未完成)
    void resume()
    {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }

private:
    // 协程句柄
    std::coroutine_handle<promise_type> handle_ = nullptr;
};

// 调度器类，负责管理和执行协程任务
// Scheduler 维护一个模拟的时间，以及待执行的任务队列、定时任务和事件处理器。
// 它提供了调度协程、处理延时、触发事件和运行协程的机制。
class Scheduler {
public:
    // 时间点类型，基于稳定时钟，精度为毫秒
    using time_point = std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds>;
    // 时间间隔类型，精度为毫秒
    using duration = std::chrono::milliseconds;
    // 事件处理器函数类型，接受一个 const void* 参数 (用于传递事件数据)
    using EventHandler = std::function<void(const void*)>;

    // 构造函数：初始化当前时间为0，并将自身设置为当前线程的活动调度器
    Scheduler()
        : current_time_(time_point { duration { 0 } })
    {
        AwaiterBase::active_scheduler_ = this; // 设置当前线程的活动调度器
    }

    // 析构函数：如果自身是当前线程的活动调度器，则清除该指针
    ~Scheduler()
    {
        if (AwaiterBase::active_scheduler_ == this) {
            AwaiterBase::active_scheduler_ = nullptr;
        }
    }

    // 获取调度器的当前模拟时间
    time_point now() const { return current_time_; }
    // 设置调度器的当前模拟时间
    void set_time(time_point new_time) { current_time_ = new_time; }
    // 将调度器的当前模拟时间向前推进指定的时长
    void advance_time(duration delta) { current_time_ += delta; }

    // 调度一个协程句柄，将其加入就绪队列等待立即执行
    void schedule(std::coroutine_handle<> handle)
    {
        ready_tasks_.push(handle);
    }

    // 调度一个协程句柄，在指定的延迟后执行
    void schedule_after(duration delay, std::coroutine_handle<> handle)
    {
        // timed_tasks_ 是一个 multimap，键是唤醒时间点，值是协程句柄
        timed_tasks_.emplace(current_time_ + delay, handle);
    }

    // 注册一个事件处理器
    // 当具有特定 EventId 的事件被触发时，相应的 EventHandler 将被调用。
    // 使用 multimap 允许多个处理器监听同一个事件ID。
    void register_event_handler(EventId event_id, EventHandler handler)
    {
        event_handlers_.emplace(event_id, std::move(handler));
    }

    // 触发一个带有数据的事件
    // EventData 是事件数据的类型。
    // 所有注册到此 event_id 的处理器都会被调用，并传入事件数据。
    // 处理器在被调用后会从 event_handlers_ 中移除 (一次性触发)。
    template <typename EventData>
    void trigger_event(EventId event_id, const EventData& data)
    {
        auto range = event_handlers_.equal_range(event_id); // 获取所有匹配此 event_id 的处理器
        std::vector<EventHandler> handlers_to_call; // 存储待调用的处理器，防止迭代器失效
        for (auto it = range.first; it != range.second; ++it) {
            handlers_to_call.push_back(it->second);
        }
        // 移除已找到的处理器 (实现一次性事件处理)
        // 如果希望事件处理器是持久的，应删除下面这行。
        // 根据 EventAwaiter 的行为 (它会重新注册)，这里的处理器是一次性的。
        if (range.first != range.second) {
            event_handlers_.erase(range.first, range.second);
        }
        // 调用处理器
        for (const auto& handler_func : handlers_to_call) {
            handler_func(static_cast<const void*>(&data)); // 将数据转换为 const void* 传递
        }
    }

    // 触发一个不带数据的事件
    // 所有注册到此 event_id 的处理器都会被调用，传入 nullptr 作为数据。
    // 处理器在被调用后会从 event_handlers_ 中移除 (一次性触发)。
    void trigger_event(EventId event_id)
    {
        auto range = event_handlers_.equal_range(event_id);
        std::vector<EventHandler> handlers_to_call;
        for (auto it = range.first; it != range.second; ++it) {
            handlers_to_call.push_back(it->second);
        }
        if (range.first != range.second) {
            event_handlers_.erase(range.first, range.second);
        }
        for (const auto& handler_func : handlers_to_call) {
            handler_func(nullptr); // 不带数据，传递 nullptr
        }
    }

    // 执行一步调度循环
    // 优先执行就绪队列中的任务。如果就绪队列为空，则检查是否有到期的定时任务。
    // 如果有到期的定时任务，则将其移至就绪队列，并更新当前时间。
    // 返回 true 如果执行了任何操作 (恢复了一个任务或处理了定时任务)，否则返回 false。
    bool run_one_step()
    {
        // 1. 处理就绪任务
        if (!ready_tasks_.empty()) {
            auto h = ready_tasks_.front(); // 获取队首任务
            ready_tasks_.pop(); // 从队列移除
            if (!h.done()) { // 如果任务未完成
                h.resume(); // 恢复执行
            }
            return true; // 执行了一个就绪任务
        }

        // 2. 处理定时任务
        if (!timed_tasks_.empty()) {
            // 将当前时间推进到最早的定时任务的时间
            set_time(timed_tasks_.begin()->first);
            // 将所有到期或早于当前时间的定时任务移到就绪队列
            while (!timed_tasks_.empty() && timed_tasks_.begin()->first <= current_time_) {
                auto h = timed_tasks_.begin()->second; // 获取任务句柄
                timed_tasks_.erase(timed_tasks_.begin()); // 从定时任务中移除
                ready_tasks_.push(h); // 加入就绪队列
            }
            return true; // 处理了定时任务 (即使只是移动到就绪队列)
        }
        return false; // 没有任务可执行
    }

    // 运行调度循环直到指定的结束时间
    // 或者直到所有任务 (就绪和定时) 都已处理完毕。
    void run_until(time_point end_time)
    {
        // 当 当前时间 < 结束时间 并且 (有就绪任务 或 有定时任务) 时，持续运行
        while (current_time_ < end_time && (!ready_tasks_.empty() || !timed_tasks_.empty())) {
            // 优先处理所有当前就绪的任务
            while (!ready_tasks_.empty()) {
                auto h = ready_tasks_.front();
                ready_tasks_.pop();
                if (!h.done()) {
                    h.resume();
                }
            }

            // 如果就绪队列为空，但仍有定时任务
            if (ready_tasks_.empty() && !timed_tasks_.empty()) {
                time_point next_event_time = timed_tasks_.begin()->first; // 获取下一个定时任务的触发时间
                // 如果下一个事件的时间晚于或等于指定的结束时间
                if (next_event_time >= end_time) {
                    set_time(end_time); // 将当前时间设置为结束时间
                    break; // 退出循环
                }
                // 否则，将当前时间推进到下一个事件的时间
                set_time(next_event_time);
                // 将所有到期或早于当前时间的定时任务移到就绪队列
                while (!timed_tasks_.empty() && timed_tasks_.begin()->first <= current_time_) {
                    auto h = timed_tasks_.begin()->second;
                    timed_tasks_.erase(timed_tasks_.begin());
                    ready_tasks_.push(h);
                }
            }
        }
        // 如果循环结束后当前时间仍早于结束时间 (例如所有任务都已完成)
        if (current_time_ < end_time) {
            set_time(end_time); // 将当前时间设置为结束时间
        }
    }

    // 检查调度器是否为空 (没有就绪任务、定时任务或事件处理器)
    bool is_empty() const
    {
        return ready_tasks_.empty() && timed_tasks_.empty() && event_handlers_.empty();
    }

private:
    time_point current_time_; // 调度器的当前模拟时间
    std::queue<std::coroutine_handle<>> ready_tasks_; // 就绪任务队列 (FIFO)
    std::multimap<time_point, std::coroutine_handle<>> timed_tasks_; // 定时任务，按时间排序
    std::multimap<EventId, EventHandler> event_handlers_; // 事件处理器，按事件ID组织
};

// Delay 等待体，用于使协程暂停指定的时长
// co_await Delay(duration) 会使当前协程挂起，并在指定时长后由调度器恢复。
class Delay : public AwaiterBase {
public:
    // 构造函数，接受一个延迟时间
    explicit Delay(Scheduler::duration delay_time)
        : delay_time_(delay_time)
    {
    }

    // await_ready: 检查是否需要挂起
    // 如果延迟时间小于或等于0，则不需要挂起，协程继续执行。
    bool await_ready() const noexcept
    {
        return delay_time_.count() <= 0;
    }

    // await_suspend: 挂起协程
    // 如果有活动的调度器，则将协程句柄和延迟时间交给调度器处理。
    // 否则 (例如在调度器上下文之外 co_await Delay)，立即恢复协程 (相当于无延迟)。
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        Scheduler* scheduler = AwaiterBase::active_scheduler_;
        if (scheduler) {
            scheduler->schedule_after(delay_time_, handle); // 通知调度器在延迟后恢复此协程
        } else {
            handle.resume(); // 没有调度器，立即恢复
        }
    }
    // await_resume: 协程恢复后执行的操作
    // 对于 Delay，恢复时不需要做任何特殊操作或返回任何值。
    void await_resume() const noexcept { }

private:
    Scheduler::duration delay_time_; // 延迟的时长
};

// EventAwaiter 等待体 (模板版本，用于等待带有数据的事件)
// co_await EventAwaiter<DataType>(eventId) 会使当前协程挂起，直到具有指定 eventId 的事件被触发。
// 恢复时，await_resume 会返回事件附带的数据。
template <typename EventData = void> // 默认为 void，但主模板处理非 void情况
class EventAwaiter : public AwaiterBase {
public:
    // 构造函数，接受一个事件ID
    explicit EventAwaiter(EventId event_id)
        : event_id_(event_id)
    {
    }

    // await_ready: 始终返回 false，表示总是需要挂起以等待事件
    bool await_ready() const noexcept { return false; }

    // await_suspend: 挂起协程并注册事件处理器
    // 如果有活动的调度器，则向调度器注册一个处理器。
    // 该处理器在事件触发时被调用，它会存储事件数据并恢复协程。
    // 否则，立即恢复协程。
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        Scheduler* scheduler = AwaiterBase::active_scheduler_;
        if (scheduler) {
            scheduler->register_event_handler(event_id_,
                // Lambda作为事件处理器
                [this, h = handle](const void* data) mutable {
                    if (data) { // 如果事件带有数据
                        // 将 const void* 转换为具体的事件数据类型并存储
                        event_data_ = *static_cast<const EventData*>(data);
                    }
                    h.resume(); // 恢复等待此事件的协程
                });
        } else {
            handle.resume(); // 没有调度器，立即恢复
        }
    }

    // await_resume: 协程恢复后返回事件数据
    EventData await_resume() const noexcept { return event_data_; }

private:
    EventId event_id_; // 等待的事件ID
    EventData event_data_ {}; // 用于存储接收到的事件数据 (如果事件有数据)
};

// EventAwaiter 等待体 (void 特化版本，用于等待不带数据的事件)
// co_await EventAwaiter<void>(eventId) 或 co_await wait_for_event(eventId)
// 会使当前协程挂起，直到具有指定 eventId 的事件被触发。
// 恢复时，await_resume 不返回任何值。
template <>
class EventAwaiter<void> : public AwaiterBase {
public:
    // 构造函数，接受一个事件ID
    explicit EventAwaiter(EventId event_id)
        : event_id_(event_id)
    {
    }
    // await_ready: 始终返回 false，表示总是需要挂起以等待事件
    bool await_ready() const noexcept { return false; }
    // await_suspend: 挂起协程并注册事件处理器
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        Scheduler* scheduler = AwaiterBase::active_scheduler_;
        if (scheduler) {
            scheduler->register_event_handler(event_id_,
                // Lambda作为事件处理器
                [h = handle](const void* /*data*/) mutable { // data 参数被忽略，因为是 void 事件
                    h.resume(); // 恢复等待此事件的协程
                });
        } else {
            handle.resume(); // 没有调度器，立即恢复
        }
    }
    // await_resume: 协程恢复后不返回任何值
    void await_resume() const noexcept { }

private:
    EventId event_id_; // 等待的事件ID
};

// 便捷函数，用于创建 Delay 等待体
inline Delay delay(Scheduler::duration duration)
{
    return Delay(duration);
}

// 便捷函数，用于创建 EventAwaiter 等待体 (可推断 EventData 类型)
// 如果不指定模板参数，默认为 EventAwaiter<void>
template <typename EventData = void>
inline EventAwaiter<EventData> wait_for_event(EventId event_id)
{
    return EventAwaiter<EventData>(event_id);
}

} // namespace cps_coro

#endif // CPS_CORO_LIB_H