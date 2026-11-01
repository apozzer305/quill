#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogFunctions.h"
#include "quill/LogMacros.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct ClockTestOptions : quill::FrontendOptions
{
  static constexpr size_t initial_queue_capacity = 1024;
};

struct SimulationClock
{
  uint64_t now() const
  {
    reads.fetch_add(1, std::memory_order_relaxed);
    return timestamp.load(std::memory_order_relaxed);
  }

  std::atomic<uint64_t> timestamp{0};
  mutable std::atomic<size_t> reads{0};
};

struct ValueClock
{
  explicit ValueClock(uint64_t value) : timestamp(value) {}

  uint64_t now() const { return timestamp; }
  uint64_t timestamp;
};

struct MoveOnlyClock
{
  uint64_t now() const { return *timestamp; }
  std::unique_ptr<uint64_t> timestamp;
};

template <typename TClock>
struct ClockPolicy : quill::LoggerPolicy
{
  using clock_t = TClock;
};

struct ClockRecord
{
  uint64_t timestamp;
  std::string logger;
  std::string message;
};

struct ClockSink : quill::Sink
{
  void write_log(quill::MacroMetadata const*, uint64_t timestamp, std::string_view, std::string_view,
                 std::string const&, std::string_view logger, quill::LogLevel, std::string_view,
                 std::string_view, std::vector<std::pair<std::string, std::string>> const*,
                 std::string_view message, std::string_view) override
  {
    logs.push_back({timestamp, std::string{logger}, std::string{message}});
  }

  void write_metric(quill::MetricMetadata const*, uint64_t timestamp, std::string_view,
                    std::string_view, std::string const&, std::string_view logger, double) override
  {
    metrics.push_back({timestamp, std::string{logger}, {}});
  }

  void flush_sink() noexcept override {}

  std::vector<ClockRecord> logs;
  std::vector<ClockRecord> metrics;
};
} // namespace

TEST_CASE("compile_time_clock_sources")
{
  using Frontend = quill::FrontendImpl<ClockTestOptions>;
  auto sink = Frontend::create_sink<ClockSink>("clock_sources_sink");
  auto* captured = static_cast<ClockSink*>(sink.get());
  SimulationClock simulation;
  SimulationClock other_simulation;
  other_simulation.timestamp.store(42);

  auto* tsc = Frontend::create_logger("clock_sources_tsc", sink);
  auto* system = Frontend::create_logger<ClockPolicy<quill::SystemClock>>(
    "clock_sources_system", {sink}, quill::PatternFormatterOptions{}, quill::SystemClock{});
  auto* custom = Frontend::create_logger<ClockPolicy<SimulationClock*>>(
    "clock_sources_custom", sink, quill::PatternFormatterOptions{}, &simulation);
  auto* other = Frontend::create_logger<ClockPolicy<SimulationClock*>>(
    "clock_sources_other", sink, quill::PatternFormatterOptions{}, &other_simulation);
  auto* value = Frontend::create_logger<ClockPolicy<ValueClock>>(
    "clock_sources_value", sink, quill::PatternFormatterOptions{}, ValueClock{123});
  auto* move_only = Frontend::create_logger<ClockPolicy<MoveOnlyClock>>(
    "clock_sources_move_only", sink, quill::PatternFormatterOptions{},
    MoveOnlyClock{std::make_unique<uint64_t>(456)});
  auto* clone = Frontend::create_or_get_logger("clock_sources_clone", custom);

  CHECK_EQ(tsc->get_clock_source_type(), quill::ClockSourceType::Tsc);
  CHECK_EQ(system->get_clock_source_type(), quill::ClockSourceType::System);
  CHECK_EQ(custom->get_clock_source_type(), quill::ClockSourceType::User);
  CHECK_EQ(clone->get_clock(), &simulation);
  CHECK_EQ(Frontend::get_logger<ClockPolicy<SimulationClock*>>("clock_sources_custom"), custom);
  CHECK_EQ(Frontend::get_logger<ClockPolicy<quill::SystemClock>>("clock_sources_system"), system);
  CHECK_EQ(Frontend::get_logger<ClockPolicy<ValueClock>>("clock_sources_missing"), nullptr);
  CHECK_EQ(Frontend::get_all_loggers<ClockPolicy<SimulationClock*>>().size(), 3);
  CHECK_EQ(Frontend::get_all_loggers().size(), 1);
  CHECK_EQ(Frontend::get_valid_logger<ClockPolicy<quill::SystemClock>>(), system);
  CHECK_EQ(Frontend::create_or_get_logger<ClockPolicy<SimulationClock*>>("clock_sources_clone"), clone);
  CHECK_EQ(Frontend::create_or_get_logger<ClockPolicy<MoveOnlyClock>>("clock_sources_move_only"), move_only);
  CHECK_EQ(Frontend::create_or_get_logger("clock_sources_move_only", move_only), move_only);
  CHECK_EQ(Frontend::create_or_get_logger<ClockPolicy<SimulationClock*>>(
             "clock_sources_custom", sink, quill::PatternFormatterOptions{}, &other_simulation),
           custom);
  CHECK_EQ(custom->get_clock(), &simulation);

#if !defined(QUILL_NO_EXCEPTIONS)
  CHECK_THROWS_AS(Frontend::create_or_get_logger("clock_sources_move_only_clone", move_only), quill::QuillError);
  CHECK_THROWS_AS((void)Frontend::get_logger("clock_sources_custom"), quill::QuillError);
  CHECK_THROWS_AS((void)Frontend::get_logger<ClockPolicy<ValueClock>>("clock_sources_custom"), quill::QuillError);
  CHECK_THROWS_AS(Frontend::create_or_get_logger("clock_sources_custom", sink), quill::QuillError);
  CHECK_THROWS_AS(Frontend::create_logger<ClockPolicy<SimulationClock*>>(
                    "clock_sources_null", sink, quill::PatternFormatterOptions{},
                    static_cast<SimulationClock*>(nullptr)),
                  quill::QuillError);
  CHECK_EQ(Frontend::get_logger<ClockPolicy<SimulationClock*>>("clock_sources_null"), nullptr);
#endif

  // The first event initializes the queue. Zero must be read exactly once and kept intact.
  LOG_INFO(custom, "zero");
  CHECK_EQ(simulation.reads.load(), 1);

  // Grow the queue before starting the backend, exercising the reservation recovery path.
  std::string const payload(700, 'x');
  for (size_t i = 0; i < 20; ++i)
  {
    LOG_INFO(custom, "{} {}", i, payload);
  }
  CHECK_EQ(simulation.reads.load(), 21);
  CHECK_GT(Frontend::get_thread_local_queue_capacity(), ClockTestOptions::initial_queue_capacity);

  quill::info(custom, "runtime metadata {}", 7);
  auto const* metric = Frontend::create_metric("clock_sources_metric", "clock_sources_metric");
  custom->publish_metric(metric, 1.0);
  custom->publish_dynamic_metric("clock_sources_dynamic", {}, 2.0);
  CHECK_EQ(simulation.reads.load(), 24);

  simulation.timestamp.store(999);
  LOG_INFO(clone, "clone");
  LOG_INFO(other, "other");
  LOG_INFO(value, "value");
  LOG_INFO(move_only, "move only");

  uint64_t const earliest_system_time = quill::SystemClock::now();
  LOG_INFO(system, "system");
  LOG_INFO(tsc, "tsc");

  // Exercise the real signal handler with a custom clock and erased logger pointer.
  custom->set_log_level(quill::LogLevel::Info);
  quill::SignalHandlerOptions signal_options;
  signal_options.catchable_signals = {SIGABRT};
  signal_options.logger_name = "clock_sources_custom";
  quill::Backend::start<ClockTestOptions>(quill::BackendOptions{}, signal_options);
  quill::detail::SignalHandlerContext::instance().should_reraise_signal.store(false);
#if defined(_WIN32)
  quill::init_signal_handler<ClockTestOptions>({SIGABRT});
#endif
  CHECK_EQ(std::raise(SIGABRT), 0);
  CHECK_EQ(simulation.reads.load(), 27);
  Frontend::remove_logger_blocking(clone);
  Frontend::remove_logger_blocking(custom);
  Frontend::remove_logger_blocking(other);
  Frontend::remove_logger_blocking(value);
  Frontend::remove_logger_blocking(move_only);
  Frontend::remove_logger_blocking(system);
  Frontend::remove_logger_blocking(tsc);
  quill::Backend::stop();
  CHECK_EQ(Frontend::get_number_of_loggers(), 0);

  REQUIRE_EQ(captured->logs.size(), 29);
  CHECK_EQ(captured->logs.front().timestamp, 0);
  CHECK_EQ(captured->logs.front().message, "zero");
  for (size_t i = 1; i <= 20; ++i)
  {
    CHECK_EQ(captured->logs[i].timestamp, 0);
    CHECK_EQ(captured->logs[i].message, std::to_string(i - 1) + " " + payload);
  }
  CHECK_EQ(captured->logs[21].timestamp, 0);
  CHECK_EQ(captured->logs[21].message, "runtime metadata 7");
  CHECK_EQ(captured->logs[22].timestamp, 999);
  CHECK_EQ(captured->logs[23].timestamp, 42);
  CHECK_EQ(captured->logs[24].timestamp, 123);
  CHECK_EQ(captured->logs[25].timestamp, 456);
  CHECK_GE(captured->logs[26].timestamp, earliest_system_time);
  CHECK_GT(captured->logs[27].timestamp, 0);
  CHECK_EQ(captured->logs[28].timestamp, 999);
  CHECK_EQ(captured->logs[28].message,
           "Received signal: SIGABRT (signum: " + std::to_string(SIGABRT) + ")");
  REQUIRE_EQ(captured->metrics.size(), 2);
  CHECK_EQ(captured->metrics[0].timestamp, 0);
  CHECK_EQ(captured->metrics[1].timestamp, 0);
}
