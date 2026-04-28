#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <queue>
#include <vector>

#include "sim/event.hpp"

namespace dm_sim {

/*********************************** 
 * Scheduler class manages the scheduling and execution of events in the simulation. It maintains a priority queue of events, 
 * ordered by their scheduled time and sequence number for tie-breaking. The scheduler provides methods to schedule new events, 
 * pop the next event for execution, and run the simulation until there are no more events left to process.
 ***********************************/
class Scheduler {
public:
    // Type alias for the event handler function, which takes an event and a reference to the scheduler for scheduling new events.
    using Handler = std::function<void(const Event&, Scheduler&)>;

    [[nodiscard]] SimTime now() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // Schedules a new event. Returns true if the event was successfully scheduled, or false if the event time is in the past.
    bool schedule(Event event);
    // Pops the next event from the priority queue. Returns an optional containing the event if the queue is not empty, or std::nullopt if it is empty.
    std::optional<Event> pop_next();
    // Runs the simulation by continuously popping and handling events until the event queue is empty.
    void run_until_empty(const Handler& handler);

private:
    // Comparator for ordering events in the priority queue. Events are ordered first by their scheduled time, and then by their sequence number for tie-breaking.
    struct EventCompare {
        bool operator()(const Event& lhs, const Event& rhs) const noexcept;
    };

    SimTime current_time_ = 0;
    std::uint64_t next_sequence_ = 0;
    std::priority_queue<Event, std::vector<Event>, EventCompare> events_;
};

}  // namespace dm_sim
