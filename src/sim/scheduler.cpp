#include "sim/scheduler.hpp"

namespace dm_sim {

bool Scheduler::EventCompare::operator()(const Event& lhs,
                                         const Event& rhs) const noexcept {
    if (lhs.time != rhs.time) {
        return lhs.time > rhs.time;
    }

    return lhs.sequence() > rhs.sequence();
}

SimTime Scheduler::now() const noexcept {
    return current_time_;
}

bool Scheduler::empty() const noexcept {
    return events_.empty();
}

std::size_t Scheduler::size() const noexcept {
    return events_.size();
}

bool Scheduler::schedule(Event event) {
    if (event.time < current_time_) {
        return false;
    }

    event.sequence_ = next_sequence_++;
    events_.push(event);
    return true;
}

std::optional<Event> Scheduler::pop_next() {
    if (events_.empty()) {
        return std::nullopt;
    }

    Event next = events_.top();
    events_.pop();
    current_time_ = next.time;
    return next;
}

void Scheduler::run_until_empty(const Handler& handler) {
    while (const std::optional<Event> event = pop_next()) {
        handler(*event, *this);
    }
}

}  // namespace dm_sim
