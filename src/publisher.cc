#include "broker/publisher.hh"

#include "broker/data.hh"
#include "broker/endpoint.hh"
#include "broker/internal/endpoint_access.hh"
#include "broker/internal/logger.hh"
#include "broker/internal/type_id.hh"
#include "broker/message.hh"
#include "broker/topic.hh"

#include <caf/attach_stream_source.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/send.hpp>

#include <numeric>

using broker::internal::facade;
using broker::internal::native;

namespace atom = broker::internal::atom;

namespace broker {

namespace {

// TODO: make these constants configurable

/// Defines how many seconds are averaged for the computation of the send rate.
constexpr size_t sample_size = 10;

/// Defines how many items are stored in the queue.
constexpr size_t queue_size = 30;

struct publisher_worker_state {
  std::vector<size_t> buf;
  size_t counter = 0;
  bool shutting_down = false;

  static const char* name;

  void tick() {
    if (buf.size() < sample_size) {
      buf.push_back(counter);
    } else {
      std::rotate(buf.begin(), buf.begin() + 1, buf.end());
      buf.back() = counter;
    }
    counter = 0;
  }

  size_t rate() {
    return !buf.empty()
           ? std::accumulate(buf.begin(), buf.end(), size_t{0}) / buf.size()
           : 0;
  }
};

const char* publisher_worker_state::name = "publisher_worker";

caf::behavior
publisher_worker(caf::stateful_actor<publisher_worker_state>* self,
                 endpoint* ep, detail::shared_publisher_queue_ptr<> qptr) {
  auto handler
    = caf::attach_stream_source(
        self, internal::native(ep->core()),
        [](caf::unit_t&) {
          // nop
        },
        [=](caf::unit_t&, caf::downstream<data_message>& out, size_t num) {
          auto& st = self->state;
          auto consumed = qptr->consume(
            num, [&](data_message&& x) { out.push(std::move(x)); });
          if (consumed > 0) {
            st.counter += consumed;
          }
        },
        [=](const caf::unit_t&) {
          return self->state.shutting_down && qptr->buffer_size() == 0;
        })
        .ptr();
  //self->delayed_send(self, std::chrono::seconds(1), atom::tick_v);
  return {
    [=](atom::resume) {
      if (handler->generate_messages())
        handler->push();
    },
    [=](atom::tick) {
      auto& st = self->state;
      st.tick();
      qptr->rate(st.rate());
      self->delayed_send(self, std::chrono::seconds(1), atom::tick_v);
    },
    [=](atom::shutdown) {
      self->state.shutting_down = true;
      self->unbecome();
      handler->generate_messages();
      // triggers the stream to terminate if the queue is already empty
      handler->push();
    }
  };
}

} // namespace <anonymous>

publisher::publisher(endpoint& ep, topic t)
  : drop_on_destruction_(false),
    queue_(detail::make_shared_publisher_queue(queue_size)),
    topic_(std::move(t)) {
  auto& sys = internal::endpoint_access{&ep}.sys();
  worker_ = facade(sys.spawn(publisher_worker, &ep, queue_));
}

publisher::~publisher() {
  reset();
}

size_t publisher::demand() const {
  return static_cast<size_t>(queue_->pending());
}

size_t publisher::buffered() const {
  return queue_->buffer_size();

}

size_t publisher::capacity() const {
  return queue_->capacity();
}

size_t publisher::free_capacity() const {
  auto x = capacity();
  auto y = buffered();
  return x > y ? x - y : 0;
}

size_t publisher::send_rate() const {
  return static_cast<size_t>(queue_->rate());
}

void publisher::drop_all_on_destruction() {
  drop_on_destruction_ = true;
}

void publisher::publish(data x) {
  BROKER_INFO("publishing" << std::make_pair(topic_, x));
  if (queue_->produce(topic_, std::move(x)))
    caf::anon_send(native(worker_), atom::resume_v);
}

void publisher::publish(std::vector<data> xs) {
  auto t = static_cast<ptrdiff_t>(queue_->capacity());
  auto i = xs.begin();
  auto e = xs.end();
  while (i != e) {
    auto j = i + std::min(std::distance(i, e), t);
#ifdef DEBUG
    BROKER_INFO("publishing batch of size" << (j - i));
    for (auto l = i; l < j; l++) {
      BROKER_INFO("publishing" << std::make_pair(topic_, *l));
    }
#endif
    if (queue_->produce(topic_, i, j))
      caf::anon_send(native(worker_), atom::resume_v);
    i = j;
  }
}

void publisher::reset() {
  if (!worker_)
    return;
  if (!drop_on_destruction_)
    caf::anon_send(native(worker_), atom::shutdown_v);
  else
    caf::anon_send_exit(native(worker_), caf::exit_reason::user_shutdown);
  worker_ = nullptr;
}

} // namespace broker
