// timer_heap.cpp — binary expiry heap with (deadline, kind priority,
// insertion_id) tie-break and lazy generation invalidation (ADR-0010).

#include "rudp_rt/runtime.hpp"

#include <algorithm>

namespace rudp_rt {

namespace {
int kind_priority(TimerKind k) { return static_cast<int>(k); }
}  // namespace

bool TimerHeap::less(const TimerEntry& a, const TimerEntry& b) const {
  if (a.deadline != b.deadline) return a.deadline < b.deadline;
  if (kind_priority(a.kind) != kind_priority(b.kind)) {
    return kind_priority(a.kind) < kind_priority(b.kind);
  }
  return a.insertion_id < b.insertion_id;
}

bool TimerHeap::push(TimerEntry e) {
  if (static_cast<int>(heap_.size()) >= capacity_) {
    return false;  // RESOURCE_EXHAUSTED path handled by the caller
  }
  e.insertion_id = ++insertion_counter_;
  heap_.push_back(e);
  std::push_heap(heap_.begin(), heap_.end(),
                 [this](const TimerEntry& x, const TimerEntry& y) {
                   return less(y, x);
                 });
  hwm_ = std::max<std::uint64_t>(hwm_, heap_.size());
  return true;
}

std::optional<TimerEntry> TimerHeap::pop_due(MonoNs now) {
  if (heap_.empty() || heap_.front().deadline > now) return std::nullopt;
  std::pop_heap(heap_.begin(), heap_.end(),
                [this](const TimerEntry& x, const TimerEntry& y) {
                  return less(y, x);
                });
  TimerEntry e = heap_.back();
  heap_.pop_back();
  return e;
}

std::optional<TimerEntry> TimerHeap::peek() const {
  if (heap_.empty()) return std::nullopt;
  return heap_.front();
}

void TimerHeap::invalidate_generation(std::uint32_t session_id,
                                      std::uint32_t gen) {
  heap_.erase(std::remove_if(heap_.begin(), heap_.end(),
                             [&](const TimerEntry& e) {
                               return e.session_id == session_id &&
                                      e.generation != gen;
                             }),
              heap_.end());
  std::make_heap(heap_.begin(), heap_.end(),
                 [this](const TimerEntry& x, const TimerEntry& y) {
                   return less(y, x);
                 });
}

}  // namespace rudp_rt
