#pragma once

#include "broker/convert.hh"
#include "broker/detail/comparable.hh"
#include "broker/detail/pp.hh"
#include "broker/fwd.hh"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace broker {

/// Broker's error codes.
// --ec-enum-start
enum class ec : uint8_t {
  /// Not-an-error.
  none,
  /// The unspecified default error code.
  unspecified,
  /// Version incompatibility.
  peer_incompatible,
  /// Referenced peer does not exist.
  peer_invalid,
  /// Remote peer not listening.
  peer_unavailable,
  /// Remote peer closed the connection during handshake.
  peer_disconnect_during_handshake,
  /// An peering request timed out.
  peer_timeout,
  /// Master with given name already exist.
  master_exists,
  /// Master with given name does not exist.
  no_such_master,
  /// The given data store key does not exist.
  no_such_key,
  /// The store operation timed out.
  request_timeout,
  /// The operation expected a different type than provided
  type_clash,
  /// The data value cannot be used to carry out the desired operation.
  invalid_data,
  /// The storage backend failed to execute the operation.
  backend_failure,
  /// The clone store has not yet synchronized with its master, or it has
  /// been disconnected for too long.
  stale_data,
  /// Opening a file failed.
  cannot_open_file,
  /// Writing to an open file failed.
  cannot_write_file,
  /// Received an unknown key for a topic.
  invalid_topic_key,
  /// Reached the end of an input file.
  end_of_file,
  /// Received an unknown type tag value.
  invalid_tag,
  /// Deserialized an invalid status.
  invalid_status,
};
// --ec-enum-end

/// Returns the 16-bit type ID that an @ref error stores if the 8-bit code
/// belongs to an @ref ec.
[[nodiscard]] uint16_t ec_category() noexcept;

/// Stores an error code along with a additional user-defined context.
class error : detail::comparable<error>, detail::comparable<error, ec> {
public:
  /// Opaque implementation type.
  struct impl;

  error();

  error(ec code);

  error(ec code, std::string description);

  error(ec code, endpoint_info info, std::string description);

  explicit error(const impl* other);

  error(const error& other);

  error(error&& other) noexcept;

  error& operator=(const error& other);

  error& operator=(error&& other) noexcept;

  ~error();

  /// Returns `valid()`.
  explicit operator bool() const noexcept {
    return valid();
  }

  /// Returns `!valid()`.
  bool operator!() const noexcept {
    return !valid();
  }

  /// Checks whether this instance stores an actual error or represents the
  /// `NULL` state.
  [[nodiscard]] bool valid() const noexcept;

  /// Returns the category-specific error code, whereas `0` means "no error".
  /// @pre `valid()`
  [[nodiscard]] uint8_t code() const noexcept;

  /// Returns the category for this error encoded as 16-bit "type ID".
  /// @pre `valid()`
  [[nodiscard]] uint16_t category() const noexcept;

  /// Returns the user-defined error message if present, `nullptr` otherwise.
  /// @pre `valid()`
  const std::string* message() const noexcept;

  /// Returns additional contextual network information if available.
  const endpoint_info* context() const noexcept;

  /// Returns a pointer to the native representation.
  [[nodiscard]] impl* native_ptr() noexcept;

  /// Returns a pointer to the native representation.
  [[nodiscard]] const impl* native_ptr() const noexcept;

  /// Compares `this` to `other`.
  /// @returns a negative value if `*this < other`, zero if `*this == other`,
  /// and a positive value if `*this > other`.
  [[nodiscard]] int compare(const error& other) const noexcept;

  [[nodiscard]] int compare(uint8_t code, uint16_t category) const noexcept;

  [[nodiscard]] int compare(ec code) const noexcept {
    return compare(static_cast<uint8_t>(code), ec_category());
  }

private:
  std::byte obj_[sizeof(impl*)];
};

/// @relates error
std::string to_string(const error& x);

/// Creates a new @ref error from given @ref ec code.
inline error make_error(ec code) {
  return error{code};
}

/// Creates a new @ref error from given @ref ec @p code and @p description.
inline error make_error(ec code, std::string description) {
  return error{code, std::move(description)};
}

/// Creates a new @ref error from given @ref ec @p code, @p info
/// and @p description.
error make_error(ec code, endpoint_info info, std::string description);

/// Evaluates to `true` if an ::error with code `E` can contain a `network_info`
/// in its context.
/// @relates ec
template <ec E>
constexpr bool ec_has_network_info_v
  = E == ec::peer_invalid || E == ec::peer_unavailable
    || E == ec::peer_disconnect_during_handshake;

/// @relates ec
template <ec Value>
using ec_constant = std::integral_constant<ec, Value>;

/// @relates ec
std::string to_string(ec code);

/// @relates ec
bool convert(const std::string& str, ec& code) noexcept;

/// @relates ec
bool convert(const data& str, ec& code) noexcept;

/// @relates ec
bool convertible_to_ec(const data& src) noexcept;

/// @relates ec
template <class Inspector>
bool inspect(Inspector& f, ec& x) {
  auto get = [&] { return static_cast<uint8_t>(x); };
  auto set = [&](uint8_t val) {
    if (val <= static_cast<uint8_t>(ec::invalid_status)) {
      x = static_cast<ec>(val);
      return true;
    } else {
      return false;
    }
  };
  return f.apply(get, set);
}

template <>
struct can_convert_predicate<ec> {
  static bool check(const data& src) noexcept {
    return convertible_to_ec(src);
  }
};

/// Checks whethter `src` is convertible to a `caf::error` with
/// `category() == caf::atom("broker")`.
bool convertible_to_error(const data& src) noexcept;

/// @copydoc convertible_to_error
bool convertible_to_error(const vector& xs) noexcept;

template <>
struct can_convert_predicate<error> {
  static bool check(const data& src) noexcept {
    return convertible_to_error(src);
  }

  static bool check(const vector& src) noexcept {
    return convertible_to_error(src);
  }
};

/// Maps `src` to `["error", code, context]` if
/// `src.category() == caf::atom("broker")`. The `context` field, depending on
/// the error code, is either, `nil`,`[<string>]`, or
/// `[<endpoint_info>, <string>]`.
bool convert(const error& src, data& dst);

/// Converts data in the format `["error", code, context]` back to an error.
bool convert(const data& src, error& dst);

/// Creates a view into a ::data object that is convertible to ::error.
class error_view {
public:
  error_view(const error_view&) noexcept = default;

  error_view& operator=(const error_view&) noexcept = default;

  bool valid() const noexcept {
    return xs_ != nullptr;
  }

  explicit operator bool() const noexcept {
    return valid();
  }

  /// @pre `valid()`
  ec code() const noexcept;

  /// @copydoc error::code
  const std::string* message() const noexcept;

  /// Retrieves additional contextual information, if available.
  std::optional<endpoint_info> context() const;

  /// Creates a view for given data.
  /// @returns A ::valid view on success, an invalid view otherwise.
  static error_view make(const data& src);

private:
  explicit error_view(const vector* ptr) noexcept : xs_(ptr) {
    // nop
  }

  const vector* xs_;
};

/// @relates error_view
inline error_view make_error_view(const data& src) {
  return error_view::make(src);
}

} // namespace broker

#define BROKER_TRY_IMPL(statement)                                             \
  if (auto err = statement)                                                    \
  return err

#define BROKER_TRY_1(x1) BROKER_TRY_IMPL(x1)

#define BROKER_TRY_2(x1, x2)                                                   \
  BROKER_TRY_1(x1);                                                            \
  BROKER_TRY_IMPL(x2)

#define BROKER_TRY_3(x1, x2, x3)                                               \
  BROKER_TRY_2(x1, x2);                                                        \
  BROKER_TRY_IMPL(x3)

#define BROKER_TRY_4(x1, x2, x3, x4)                                           \
  BROKER_TRY_3(x1, x2, x3);                                                    \
  BROKER_TRY_IMPL(x4)

#define BROKER_TRY_5(x1, x2, x3, x4, x5)                                       \
  BROKER_TRY_4(x1, x2, x3, x4);                                                \
  BROKER_TRY_IMPL(x5)

#define BROKER_TRY_6(x1, x2, x3, x4, x5, x6)                                   \
  BROKER_TRY_5(x1, x2, x3, x4, x5);                                            \
  BROKER_TRY_IMPL(x6)

#define BROKER_TRY_7(x1, x2, x3, x4, x5, x6, x7)                               \
  BROKER_TRY_6(x1, x2, x3, x4, x5, x6);                                        \
  BROKER_TRY_IMPL(x7)

#define BROKER_TRY_8(x1, x2, x3, x4, x5, x6, x7, x8)                           \
  BROKER_TRY_7(x1, x2, x3, x4, x5, x6, x7);                                    \
  BROKER_TRY_IMPL(x8)

#define BROKER_TRY_9(x1, x2, x3, x4, x5, x6, x7, x8, x9)                       \
  BROKER_TRY_8(x1, x2, x3, x4, x5, x6, x7, x8);                                \
  BROKER_TRY_IMPL(x9)

#ifdef _MSC_VER

#define BROKER_TRY(...)                                                        \
  BROKER_PP_CAT(BROKER_PP_OVERLOAD(BROKER_TRY_, __VA_ARGS__)(__VA_ARGS__),     \
                BROKER_PP_EMPTY())

#else // _MSVC_VER

#define BROKER_TRY(...)                                                        \
  BROKER_PP_OVERLOAD(BROKER_TRY_, __VA_ARGS__)(__VA_ARGS__)

#endif // _MSVC_VER
