#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/DeferredFormatCodec.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

struct RuntimeClockValue
{
  uint64_t value;
};

template <>
struct fmtquill::formatter<RuntimeClockValue>
{
  constexpr auto parse(format_parse_context& context) { return context.begin(); }

  auto format(RuntimeClockValue const& value, format_context& context) const
  {
    return fmtquill::format_to(context.out(), "{}", value.value);
  }
};

template <>
struct quill::Codec<RuntimeClockValue> : quill::DeferredFormatCodec<RuntimeClockValue>
{
  template <typename Arg>
  static void encode(std::byte*& buffer, quill::detail::SizeCacheVector const& size_cache,
                     uint32_t& size_cache_index, Arg&& value)
  {
    static_assert(std::is_rvalue_reference_v<Arg&&>,
                  "Clock dispatch must preserve rvalue arguments");
    DeferredFormatCodec<RuntimeClockValue>::encode(buffer, size_cache, size_cache_index,
                                                   static_cast<Arg&&>(value));
  }
};

namespace
{
struct RuntimeClockOptions : quill::FrontendOptions
{
  static constexpr quill::QueueType queue_type = quill::QueueType::QUILL_TEST_QUEUE_TYPE;
  static constexpr size_t initial_queue_capacity = 1024;
  static constexpr size_t unbounded_queue_max_capacity = 4096;
};

struct CountingClock : quill::UserClockSource
{
  uint64_t now() const override
  {
    reads.fetch_add(1, std::memory_order_relaxed);
    return timestamp.load(std::memory_order_relaxed);
  }

  mutable std::atomic<size_t> reads{0};
  std::atomic<uint64_t> timestamp{0};
};

struct LogRecord
{
  std::string logger;
  uint64_t timestamp;
  std::string message;
};

struct MetricRecord
{
  std::string logger;
  uint64_t timestamp;
  double value;
};

struct RuntimeClockSink : quill::Sink
{
  void write_log(quill::MacroMetadata const*, uint64_t timestamp, std::string_view, std::string_view,
                 std::string const&, std::string_view logger, quill::LogLevel, std::string_view,
                 std::string_view, std::vector<std::pair<std::string, std::string>> const*,
                 std::string_view message, std::string_view) override
  {
    logs.push_back({std::string{logger}, timestamp, std::string{message}});
    log_count.fetch_add(1, std::memory_order_release);
  }

  void write_metric(quill::MetricMetadata const*, uint64_t timestamp, std::string_view,
                    std::string_view, std::string const&, std::string_view logger, double value) override
  {
    metrics.push_back({std::string{logger}, timestamp, value});
  }

  void flush_sink() override {}

  std::vector<LogRecord> logs;
  std::vector<MetricRecord> metrics;
  std::atomic<size_t> log_count{0};
};
} // namespace

TEST_CASE("runtime_clock_queues_" QUILL_TEST_QUEUE_NAME)
{
  using Frontend = quill::FrontendImpl<RuntimeClockOptions>;
  using Logger = quill::LoggerImpl<RuntimeClockOptions>;

  CountingClock clock;
  auto sink = Frontend::create_sink<RuntimeClockSink>("runtime_clock_sink_" QUILL_TEST_QUEUE_NAME);
  RuntimeClockSink* capture = static_cast<RuntimeClockSink*>(sink.get());
  Logger* user =
    Frontend::create_logger("runtime_clock_user_" QUILL_TEST_QUEUE_NAME, sink,
                            quill::PatternFormatterOptions{}, quill::ClockSourceType::User, &clock);
  Logger* system = Frontend::create_logger("runtime_clock_system_" QUILL_TEST_QUEUE_NAME, sink,
                                           quill::PatternFormatterOptions{}, quill::ClockSourceType::System);
  Logger* tsc = Frontend::create_logger("runtime_clock_tsc_" QUILL_TEST_QUEUE_NAME, sink);
  quill::MetricMetadata const* metric =
    Frontend::create_metric("runtime_clock_metric_" QUILL_TEST_QUEUE_NAME, "runtime_clock_metric");
  static constexpr quill::MacroMetadata metadata{
    "", "", "{} {} {}", nullptr, quill::LogLevel::Info, quill::MacroMetadata::Event::Log};

  std::vector<LogRecord> expected_logs;
  std::vector<MetricRecord> expected_metrics;
  uint64_t sequence{0};
  uint64_t const system_begin = quill::detail::get_system_time_ns();

  auto log = [&](Logger* logger, std::string const& payload)
  {
    size_t const reads = clock.reads.load();
    bool const accepted =
      logger->log_statement<false>(&metadata, sequence, payload.c_str(), RuntimeClockValue{sequence});
    if (logger == user)
    {
      CHECK_EQ(clock.reads.load(), reads + 1);
    }
    if (accepted)
    {
      std::string const number = std::to_string(sequence);
      expected_logs.push_back(
        {logger->get_logger_name(), clock.timestamp.load(), number + " " + payload + " " + number});
    }
    ++sequence;
    return accepted;
  };

  auto publish = [&](Logger* logger)
  {
    size_t const reads = clock.reads.load();
    double const value = static_cast<double>(sequence++);
    bool const accepted = logger->publish_metric(metric, value);
    if (logger == user)
    {
      CHECK_EQ(clock.reads.load(), reads + 1);
    }
    if (accepted)
    {
      expected_metrics.push_back({logger->get_logger_name(), clock.timestamp.load(), value});
    }
    return accepted;
  };

  // A zero user timestamp is valid, including on the first call from a thread.
  CHECK(log(user, "initial"));
  std::thread first_metric{[&] { CHECK(publish(user)); }};
  first_metric.join();
  CHECK(log(user, "cached"));
  CHECK(publish(user));

  if constexpr (RuntimeClockOptions::queue_type == quill::QueueType::BoundedDropping ||
                RuntimeClockOptions::queue_type == quill::QueueType::UnboundedDropping)
  {
    // With no backend running, exhaust the queue and verify failed reservations read the clock once.
    bool dropped{false};
    for (size_t i = 0; i < 128 && !dropped; ++i)
    {
      dropped = !log(user, std::string(200, 'x'));
    }
    CHECK(dropped);
    dropped = false;
    for (size_t i = 0; i < 128 && !dropped; ++i)
    {
      dropped = !publish(user);
    }
    CHECK(dropped);
  }
  else if constexpr (Logger::using_unbounded_queue)
  {
    // Grow beyond the initial node before allowing the backend to drain it.
    for (size_t i = 0; i < 8; ++i)
    {
      CHECK(log(user, std::string(200, 'x')));
    }
  }

  quill::BackendOptions backend_options;
  backend_options.error_notifier = {};
  quill::Backend::start(backend_options);
  user->flush_log();
  clock.timestamp.store(123456789);

  // All clock types use the same logger type and thread queue; repeatedly refresh cached space.
  for (Logger* logger : {tsc, system, user})
  {
    for (size_t i = 0; i < 100; ++i)
    {
      log(logger, std::string(200, 'y'));
      publish(logger);
    }
    logger->flush_log();
  }

  user->set_immediate_flush(1);
  LOG_INFO(user, "immediate clock flush");
  expected_logs.push_back(
    {user->get_logger_name(), clock.timestamp.load(), "immediate clock flush"});
  CHECK_EQ(capture->log_count.load(std::memory_order_acquire), expected_logs.size());

  quill::Backend::stop();
  uint64_t const system_end = quill::detail::get_system_time_ns();
  REQUIRE_EQ(capture->logs.size(), expected_logs.size());
  REQUIRE_EQ(capture->metrics.size(), expected_metrics.size());

  auto check_timestamp = [&](std::string const& logger, uint64_t timestamp, uint64_t expected)
  {
    if (logger == user->get_logger_name())
    {
      CHECK_EQ(timestamp, expected);
    }
    else if (logger == system->get_logger_name())
    {
      CHECK_GE(timestamp, system_begin);
      CHECK_LE(timestamp, system_end);
    }
    else
    {
      CHECK_GT(timestamp, 0);
    }
  };

  for (LogRecord const& expected : expected_logs)
  {
    auto const found =
      std::find_if(capture->logs.begin(), capture->logs.end(),
                   [&](LogRecord const& record)
                   { return record.logger == expected.logger && record.message == expected.message; });
    REQUIRE(found != capture->logs.end());
    check_timestamp(found->logger, found->timestamp, expected.timestamp);
  }
  for (MetricRecord const& expected : expected_metrics)
  {
    auto const found = std::find_if(
      capture->metrics.begin(), capture->metrics.end(),
      [&](MetricRecord const& record)
      { return record.logger == expected.logger && record.value == doctest::Approx{expected.value}; });
    REQUIRE(found != capture->metrics.end());
    check_timestamp(found->logger, found->timestamp, expected.timestamp);
  }
}
