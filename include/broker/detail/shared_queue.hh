#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "broker/data.hh"
#include "broker/message.hh"
#include "broker/topic.hh"

#include "broker/detail/flare.hh"

namespace broker::detail {

/// Base class for `shared_publisher_queue` and `shared_subscriber_queue`.
template <class ValueType = data_message>
class shared_queue {
public:
  using value_type = ValueType;

  using guard_type = std::unique_lock<std::mutex>;

  virtual ~shared_queue() {
    // nop
  }

  // --- accessors -------------------------------------------------------------

  auto fd() const {
    return fx_.fd();
  }

  long pending() const {
    return pending_.load();
  }

  size_t rate() const {
    return rate_.load();
  }

  size_t buffer_size() const {
    guard_type guard{mtx_};
    return xs_.size();
  }

  // --- mutators --------------------------------------------------------------

  void pending(long x) {
    pending_ = x;
  }

  void rate(size_t x) {
    rate_ = x;
  }

  void wait_on_flare() {
    fx_.await_one();
  }

  template<class Duration>
  bool wait_on_flare(Duration timeout) {
    if (timeout == infinite) {
      fx_.await_one();
      return true;
    }
    auto abs_timeout = std::chrono::high_resolution_clock::now();
    abs_timeout += timeout;
    return fx_.await_one(abs_timeout);
  }

  template <class T>
  bool wait_on_flare_abs(T abs_timeout) {
    return fx_.await_one(abs_timeout);
  }

protected:
  shared_queue() : pending_(0) {
    // nop
  }

  /// Guards access to `xs`.
  mutable std::mutex mtx_;

  /// Signals to users when data can be read or written.
  mutable flare fx_;

  /// Buffers values received by the worker.
  std::deque<value_type> xs_;

  /// Stores what demand the worker has last signaled to the core or vice
  /// versa, depending on the message direction.
  std::atomic<long> pending_;

  /// Stores consumption or production rate.
  std::atomic<size_t> rate_;
};

} // namespace broker::detail
