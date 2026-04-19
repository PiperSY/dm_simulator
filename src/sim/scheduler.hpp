#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <queue>
#include <vector>

#include "sim/event.hpp"

namespace dm_sim {

class Scheduler {
public:
    using Handler = std::function<void(const Event&, Scheduler&)>;

    [[nodiscard]] SimTime now() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    bool schedule(Event event);
    std::optional<Event> pop_next();
    void run_until_empty(const Handler& handler);

private:
    struct EventCompare {
        bool operator()(const Event& lhs, const Event& rhs) const noexcept;
    };

    SimTime current_time_ = 0;
    std::uint64_t next_sequence_ = 0;
    std::priority_queue<Event, std::vector<Event>, EventCompare> events_;
};

}  // namespace dm_sim
