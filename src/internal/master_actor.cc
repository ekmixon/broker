#include "broker/internal/logger.hh" // Needs to come before CAF includes.

#include <caf/actor.hpp>
#include <caf/attach_stream_sink.hpp>
#include <caf/behavior.hpp>
#include <caf/error.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/make_message.hpp>
#include <caf/stateful_actor.hpp>
#include <caf/sum_type.hpp>
#include <caf/system_messages.hpp>
#include <caf/unit.hpp>

#include "broker/convert.hh"
#include "broker/data.hh"
#include "broker/detail/abstract_backend.hh"
#include "broker/detail/die.hh"
#include "broker/internal/master_actor.hh"
#include "broker/internal/native.hh"
#include "broker/internal/type_id.hh"
#include "broker/store.hh"
#include "broker/time.hh"
#include "broker/topic.hh"

namespace broker::internal {

namespace {

std::optional<timestamp> to_opt_timestamp(timestamp ts,
                                          std::optional<timespan> span) {
  return span ? ts + *span : std::optional<timestamp>{};
}

template <class T>
auto to_caf_res(expected<T>&& x) {
  if (x)
    return caf::result<T>{std::move(*x)};
  else
    return caf::result<T>{std::move(native(x.error()))};
}

} // namespace

void master_state::init(caf::event_based_actor* ptr, std::string&& nm,
                        backend_pointer&& bp, caf::actor&& parent,
                        endpoint::clock* ep_clock) {
  super::init(ptr, ep_clock, std::move(nm), std::move(parent));
  clones_topic = id / topic::clone_suffix();
  backend = std::move(bp);
  if (auto es = backend->expiries()) {
    for (auto& e : *es) {
      auto& key = e.first;
      auto& expire_time = e.second;
      auto n = clock->now();
      auto dur = expire_time - n;
      auto msg = caf::make_message(atom::expire_v, std::move(key));
      clock->send_later(facade(caf::actor{self}), dur, &msg);
    }
  } else {
    detail::die("failed to get master expiries while initializing");
  }
}

void master_state::broadcast(internal_command&& x) {
  self->send(core, atom::publish_v,
             make_command_message(clones_topic, std::move(x)));
}

void master_state::remind(timespan expiry, const data& key) {
  auto msg = caf::make_message(atom::expire_v, key);
  clock->send_later(facade(caf::actor{self}), expiry, &msg);
}

void master_state::expire(data& key) {
  BROKER_INFO("EXPIRE" << key);
  if (auto result = backend->expire(key, clock->now()); !result) {
    BROKER_ERROR("EXPIRE" << key << "(FAILED)" << to_string(result.error()));
  } else if (!*result) {
    BROKER_INFO("EXPIRE" << key << "(IGNORE/STALE)");
  } else {
    expire_command cmd{std::move(key),
                       publisher_id{facade(self->node()), self->id()}};
    emit_expire_event(cmd);
    broadcast_cmd_to_clones(std::move(cmd));
  }
}

void master_state::command(internal_command& cmd) {
  command(cmd.content);
}

void master_state::command(internal_command::variant_type& cmd) {
  std::visit(*this, cmd);
}

void master_state::operator()(none) {
  BROKER_INFO("received empty command");
}

void master_state::operator()(put_command& x) {
  BROKER_INFO("PUT" << x.key << "->" << x.value << "with expiry" << (x.expiry ? to_string(*x.expiry) : "none"));
  auto et = to_opt_timestamp(clock->now(), x.expiry);
  auto old_value = backend->get(x.key);
  auto result = backend->put(x.key, x.value, et);
  if (!result) {
    BROKER_WARNING("failed to put" << x.key << "->" << x.value);
    return; // TODO: propagate failure? to all clones? as status msg?
  }
  if (x.expiry)
    remind(*x.expiry, x.key);
  if (old_value)
    emit_update_event(x, *old_value);
  else
    emit_insert_event(x);
  broadcast_cmd_to_clones(std::move(x));
}

void master_state::operator()(put_unique_command& x) {
  BROKER_INFO("PUT_UNIQUE" << x.key << "->" << x.value << "with expiry" << (x.expiry ? to_string(*x.expiry) : "none"));
  if (exists(x.key)) {
    // Note that we don't bother broadcasting this operation to clones since
    // no change took place.
    self->send(native(x.who), caf::make_message(data{false}, x.req_id));
    return;
  }
  auto et = to_opt_timestamp(clock->now(), x.expiry);
  if (auto res = backend->put(x.key, x.value, et); !res) {
    BROKER_WARNING("failed to put_unique" << x.key << "->" << x.value);
    self->send(native(x.who), caf::make_message(data{false}, x.req_id));
    return;
  }
  self->send(native(x.who), caf::make_message(data{true}, x.req_id));
  if (x.expiry)
    remind(*x.expiry, x.key);
  emit_insert_event(x);
  // Broadcast a regular "put" command. Clones don't have to do their own
  // existence check.
  put_command cmd{std::move(x.key), std::move(x.value), x.expiry,
                  std::move(x.publisher)};
  broadcast_cmd_to_clones(std::move(cmd));
}

void master_state::operator()(erase_command& x) {
  BROKER_INFO("ERASE" << x.key);
  if (auto res = backend->erase(x.key); !res) {
    BROKER_WARNING("failed to erase" << x.key << "->" << res.error());
    return; // TODO: propagate failure? to all clones? as status msg?
  }
  emit_erase_event(x.key, x.publisher);
  broadcast_cmd_to_clones(std::move(x));
}

void master_state::operator()(expire_command&) {
  BROKER_ERROR("received an expire_command in master actor");
}

void master_state::operator()(add_command& x) {
  BROKER_INFO("ADD" << x);
  auto old_value = backend->get(x.key);
  auto et = to_opt_timestamp(clock->now(), x.expiry);
  if (auto res = backend->add(x.key, x.value, x.init_type, et); !res) {
    BROKER_WARNING("failed to add" << x.value << "to" << x.key << "->"
                                   << res.error());
    return; // TODO: propagate failure? to all clones? as status msg?
  }
  if (auto val = backend->get(x.key); !val) {
    BROKER_ERROR("failed to get"
                 << x.value << "after add() returned success:" << val.error());
    return; // TODO: propagate failure? to all clones? as status msg?
  } else {
    if (x.expiry)
      remind(*x.expiry, x.key);
    // Broadcast a regular "put" command. Clones don't have to repeat the same
    // processing again.
    put_command cmd{std::move(x.key), std::move(*val), std::nullopt,
                    std::move(x.publisher)};
    if (old_value)
      emit_update_event(cmd, *old_value);
    else
      emit_insert_event(cmd);
    broadcast_cmd_to_clones(std::move(cmd));
  }
}

void master_state::operator()(subtract_command& x) {
  BROKER_INFO("SUBTRACT" << x);
  auto et = to_opt_timestamp(clock->now(), x.expiry);
  auto old_value = backend->get(x.key);
  if (!old_value) {
    // Unlike `add`, `subtract` fails if the key didn't exist previously.
    BROKER_WARNING("cannot substract from non-existing value for key" << x.key);
    return; // TODO: propagate failure? to all clones? as status msg?
  }
  if (auto res = backend->subtract(x.key, x.value, et); !res) {
    BROKER_WARNING("failed to substract" << x.value << "from" << x.key);
    return; // TODO: propagate failure? to all clones? as status msg?
  }
  if (auto val = backend->get(x.key); !val) {
    BROKER_ERROR("failed to get"
                 << x.value
                 << "after subtract() returned success:" << val.error());
    return; // TODO: propagate failure? to all clones? as status msg?
  } else {
    if (x.expiry)
      remind(*x.expiry, x.key);
    // Broadcast a regular "put" command. Clones don't have to repeat the same
    // processing again.
    put_command cmd{std::move(x.key), std::move(*val), std::nullopt,
                    std::move(x.publisher)};
    emit_update_event(cmd, *old_value);
    broadcast_cmd_to_clones(std::move(cmd));
  }
}

void master_state::operator()(snapshot_command& x) {
  BROKER_INFO("SNAPSHOT from" << to_string(x.remote_core));
  if (x.remote_core == nullptr || x.remote_clone == nullptr) {
    BROKER_INFO("snapshot command with invalid address received");
    return;
  }
  auto ss = backend->snapshot();
  if (!ss)
    detail::die("failed to snapshot master");
  auto hdl = native(x.remote_core);
  self->monitor(hdl);
  clones.emplace(hdl.address(), native(x.remote_clone));

  // The snapshot gets sent over a different channel than updates,
  // so we send a "sync" point over the update channel that target clone
  // can use in order to apply any updates that arrived before it
  // received the now-outdated snapshot.
  broadcast_cmd_to_clones(snapshot_sync_command{x.remote_clone});

  // TODO: possible improvements to do here
  // (1) Use a separate *streaming* channel to send the snapshot.
  //     A benefit of that would potentially be less latent queries
  //     that go directly against the master store.
  // (2) Always keep an updated snapshot in memory on the master to
  //     avoid numerous expensive retrievals from persistent backends
  //     in quick succession (e.g. at startup).
  // (3) As an alternative to (2), give backends an API to stream
  //     key-value pairs without ever needing the full snapshot in
  //     memory.  Note that this would require halting the application
  //     of updates on the master while there are any snapshot streams
  //     still underway.
  self->send(native(x.remote_clone), set_command{std::move(*ss)});
}

void master_state::operator()(snapshot_sync_command&) {
  BROKER_ERROR("received a snapshot_sync_command in master actor");
}

void master_state::operator()(set_command& x) {
  BROKER_ERROR("received a set_command in master actor");
}

void master_state::operator()(clear_command& x) {
  BROKER_INFO("CLEAR" << x);
  if (auto keys_res = backend->keys(); !keys_res) {
    BROKER_ERROR("unable to obtain keys:" << keys_res.error());
    return;
  } else {
    if (auto keys = get_if<vector>(*keys_res)) {
      for (auto& key : *keys)
        emit_erase_event(key, x.publisher);
    } else if (auto keys = get_if<set>(*keys_res)) {
      for (auto& key : *keys)
        emit_erase_event(key, x.publisher);
    } else if (!is<none>(*keys_res)) {
      BROKER_ERROR("backend->keys() returned an unexpected result type");
    }
  }
  if (auto res = backend->clear(); !res)
    detail::die("failed to clear master");
  broadcast_cmd_to_clones(std::move(x));
}

bool master_state::exists(const data& key) {
  if (auto res = backend->exists(key))
    return *res;
  return false;
}

caf::behavior master_actor(caf::stateful_actor<master_state>* self,
                           caf::actor core, std::string id,
                           master_state::backend_pointer backend,
                           endpoint::clock* clock) {
  self->monitor(core);
  self->state.init(self, std::move(id), std::move(backend),
                   std::move(core), clock);
  self->set_down_handler(
    [=](const caf::down_msg& msg) {
      if (msg.source == core) {
        BROKER_INFO("core is down, kill master as well");
        self->quit(msg.reason);
      } else {
        BROKER_INFO("lost a clone");
        self->state.clones.erase(msg.source);
      }
    }
  );
  return {
    // --- local communication -------------------------------------------------
    [=](atom::local, internal_command& x) {
      // treat locally and remotely received commands in the same way
      self->state.command(x);
    },
    [=](atom::sync_point, caf::actor& who) {
      self->send(who, atom::sync_point_v);
    },
    [=](atom::expire, data& key) { self->state.expire(key); },
    [=](atom::get, atom::keys) -> caf::result<data> {
      auto x = self->state.backend->keys();
      BROKER_INFO("KEYS ->" << x);
      return to_caf_res(std::move(x));
    },
    [=](atom::get, atom::keys, request_id id) {
      auto x = self->state.backend->keys();
      BROKER_INFO("KEYS" << "with id:" << id << "->" << x);
      if (x)
        return caf::make_message(std::move(*x), id);
      else
        return caf::make_message(native(std::move(x.error())), id);
    },
    [=](atom::exists, const data& key) -> caf::result<data> {
      auto x = self->state.backend->exists(key);
      BROKER_INFO("EXISTS" << key << "->" << x);
      return {data{std::move(*x)}};
    },
    [=](atom::exists, const data& key, request_id id) {
      auto x = self->state.backend->exists(key);
      BROKER_INFO("EXISTS" << key << "with id:" << id << "->" << x);
      return caf::make_message(data{std::move(*x)}, id);
    },
    [=](atom::get, const data& key) -> caf::result<data> {
      auto x = self->state.backend->get(key);
      BROKER_INFO("GET" << key << "->" << x);
      return to_caf_res(std::move(x));
    },
    [=](atom::get, const data& key, const data& aspect) -> caf::result<data> {
      auto x = self->state.backend->get(key, aspect);
      BROKER_INFO("GET" << key << aspect << "->" << x);
      return to_caf_res(std::move(x));
    },
    [=](atom::get, const data& key, request_id id) {
      auto x = self->state.backend->get(key);
      BROKER_INFO("GET" << key << "with id:" << id << "->" << x);
      if (x)
        return caf::make_message(std::move(*x), id);
      else
        return caf::make_message(native(x.error()), id);
    },
    [=](atom::get, const data& key, const data& value, request_id id) {
      auto x = self->state.backend->get(key, value);
      BROKER_INFO("GET" << key << "->" << value << "with id:" << id << "->" << x);
      if (x)
        return caf::make_message(std::move(*x), id);
      else
        return caf::make_message(native(x.error()), id);
    },
    [=](atom::get, atom::name) { return self->state.id; },
    // --- stream handshake with core ------------------------------------------
    [=](caf::stream<command_message>& in) {
      BROKER_DEBUG("received stream handshake from core");
      attach_stream_sink(
        self,
        // input stream
        in,
        // initialize state
        [](caf::unit_t&) {
          // nop
        },
        // processing step
        [=](caf::unit_t&, command_message y) {
          // TODO: our operator() overloads require mutable references, but
          //       only a fraction actually benefit from it.
          auto cmd = move_command(y);
          self->state.command(cmd);
        },
        // cleanup
        [](caf::unit_t&, const caf::error&) {
          // nop
        });
    }};
}

} // namespace broker::internal
