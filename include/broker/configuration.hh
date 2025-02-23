#pragma once

#include "broker/defaults.hh"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace broker::internal {

struct configuration_access;

} // namespace broker::internal

namespace broker {

struct skip_init_t {};

constexpr skip_init_t skip_init = skip_init_t{};

struct broker_options {
  /// If true, peer connections won't use SSL.
  bool disable_ssl = false;

  /// If true, endpoints will forward incoming messages to peers.
  bool forward = true;

  /// TTL to insert into forwarded messages. Messages will be droppped once
  /// they have traversed more than this many hops. Note that the 1st
  /// receiver inserts the TTL (not the sender!). The 1st receiver does
  /// already count against the TTL.
  unsigned int ttl = defaults::ttl;

  /// Whether to use real/wall clock time for data store time-keeping
  /// tasks or whether the application will simulate time on its own.
  bool use_real_time = true;

  /// Whether to ignore the `broker.conf` file.
  bool ignore_broker_conf = false;

  broker_options() = default;

  broker_options(const broker_options&) = default;

  broker_options& operator=(const broker_options&) = default;
};

/// Configures an ::endpoint.
///
/// The configuration draws user-provided options from three sources (in order):
/// 1. The file `broker.conf`. Contents of this file override hard-coded
///    defaults. Broker only scans the current working directory when looking
///    for this file.
/// 2. Environment variables. Broker currently recognizes the following
///    environment variables:
///    - `BROKER_CONSOLE_VERBOSITY`: enables console output by overriding
///      `logger.console-verbosity`. Valid values are `trace`, `debug`, `info`,
///      `warning`, and `error`.
///    - `BROKER_FILE_VERBOSITY`: enables log file output by overriding
///      `logger.file-verbosity`.
///    - `BROKER_RECORDING_DIRECTORY` enables recording of meta data for the
///      `broker-cluster-benchmark` tool.
///    - `BROKER_OUTPUT_GENERATOR_FILE_CAP` restricts the number of recorded
///      messages in recording mode.
/// 3. Command line arguments (if provided).
///
/// As a rule of thumb, set `BROKER_CONSOLE_VERBOSITY` to `info` for getting
/// output on high-level events such as peerings. If you need to tap
/// into published messages, set `BROKER_CONSOLE_VERBOSITY` to `debug`. Enabling
/// debug output will slow down Broker and generates a lot of console output.
///
/// Writing to a file instead of printing to the command line can help grepping
/// through large logs or correlating logs from multiple Broker peers.
class configuration {
public:
  // --- friends ---------------------------------------------------------------

  friend struct internal::configuration_access;

  // --- member types ----------------------------------------------------------

  struct impl;

  // --- construction and destruction ------------------------------------------

  /// Constructs the configuration without calling `init` implicitly. Requires
  /// the user to call `init` manually.
  explicit configuration(skip_init_t);

  configuration();

  configuration(configuration&&);

  /// Constructs a configuration with non-default Broker options.
  explicit configuration(broker_options opts);

  /// Constructs a configuration from command line arguments.
  configuration(int argc, char** argv);

  ~configuration();

  // -- properties -------------------------------------------------------------

  /// Returns default Broker options and flags.
  const broker_options& options() const;

  std::string help_text() const;

  const std::vector<std::string>& remainder() const;

  bool cli_helptext_printed() const;

  std::string openssl_certificate() const;

  void openssl_certificate(std::string);

  std::string openssl_key() const;

  void openssl_key(std::string);

  std::string openssl_passphrase() const;

  void openssl_passphrase(std::string);

  std::string openssl_capath() const;

  void openssl_capath(std::string);

  std::string openssl_cafile() const;

  void openssl_cafile(std::string);

  // -- mutators ---------------------------------------------------------------

  void add_option(int64_t* dst, std::string_view name,
                  std::string_view description);

  void add_option(uint64_t* dst, std::string_view name,
                  std::string_view description);

  void add_option(double* dst, std::string_view name,
                  std::string_view description);

  void add_option(bool* dst, std::string_view name,
                  std::string_view description);

  void add_option(std::string* dst, std::string_view name,
                  std::string_view description);

  void add_option(std::vector<std::string>* dst, std::string_view name,
                  std::string_view description);

  template <class T>
  std::enable_if_t<std::is_integral_v<T>> set(std::string key, T val) {
    if constexpr (std::is_same_v<T, bool>)
      set_bool(std::move(key), val);
    if constexpr (std::is_signed_v<T>)
      set_i64(std::move(key), val);
    else
      set_u64(std::move(key), val);
  }

  void set(std::string key, timespan val);

  void set(std::string key, std::string val);

  void set(std::string key, std::vector<std::string> val);

  std::optional<int64_t> read_i64(std::string_view key, int64_t min_val,
                                  int64_t max_val) const;

  std::optional<uint64_t> read_u64(std::string_view key,
                                   uint64_t max_val) const;

  std::optional<timespan> read_ts(std::string_view key) const;

  std::optional<std::string> read_str(std::string_view key) const;

  std::optional<std::vector<std::string>>
  read_str_vec(std::string_view key) const;

  /// Initializes any global state required by Broker such as the global meta
  /// object table for Broker and CAF (core, I/O and OpenSSL modules). This
  /// function is safe to call multiple times (repeated calls have no effect).
  /// @note this function has no effect when compiling against CAF < 0.18
  /// @note all constructors call this function implicitly, but users can call
  ///       it explicitly when using a custom config class or when calling CAF
  ///       code prior to creating the configuration object.
  static void init_global_state();

  /// Returns a pointer to the native representation.
  [[nodiscard]] impl* native_ptr() noexcept;

  /// Returns a pointer to the native representation.
  [[nodiscard]] const impl* native_ptr() const noexcept;

  void init(int argc, char** argv);

private:
  void set_i64(std::string key, int64_t val);

  void set_u64(std::string key, uint64_t val);

  void set_bool(std::string key, bool val);

  std::unique_ptr<impl> impl_;
};

template <class T>
auto get_as(const configuration& cfg, std::string_view key) {
  if constexpr (std::is_integral_v<T>){
    std::optional<T> res;
    using lim = std::numeric_limits<T>;
    if constexpr (std::is_signed_v<T>) {
      if (auto val = cfg.read_i64(key, lim::min(), lim::max()))
        res = static_cast<T>(*val);
    } else {
      if (auto val = cfg.read_u64(key, lim::max()))
        res = static_cast<T>(*val);
    }
    return res;
  } else if constexpr (std::is_same_v<T, timespan>) {
    return cfg.read_ts(key);
  } else if constexpr (std::is_same_v<T, std::string>) {
    return cfg.read_str(key);
  } else {
    static_assert(std::is_same_v<T, std::vector<std::string>>);
    return cfg.read_str_vec(key);
  }
}

} // namespace broker
