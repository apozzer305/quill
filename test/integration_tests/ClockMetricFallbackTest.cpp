#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct MetricClockOptions : quill::FrontendOptions
{
  static constexpr size_t initial_queue_capacity = 64;
};

struct ZeroMetricClock
{
  uint64_t now() const
  {
    reads.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }

  mutable std::atomic<size_t> reads{0};
};

struct MetricClockSink : quill::Sink
{
  void write_log(quill::MacroMetadata const*, uint64_t, std::string_view, std::string_view,
                 std::string const&, std::string_view, quill::LogLevel, std::string_view,
                 std::string_view, std::vector<std::pair<std::string, std::string>> const*,
                 std::string_view, std::string_view) override
  {
  }

  void write_metric(quill::MetricMetadata const*, uint64_t timestamp, std::string_view,
                    std::string_view, std::string const&, std::string_view, double value) override
  {
    timestamps.push_back(timestamp);
    values.push_back(value);
  }

  void flush_sink() noexcept override {}

  std::vector<uint64_t> timestamps;
  std::vector<double> values;
};
} // namespace

TEST_CASE("clock_metric_fallback_preserves_zero_timestamp")
{
  using Frontend = quill::FrontendImpl<MetricClockOptions>;
  ZeroMetricClock clock;
  auto sink = Frontend::create_sink<MetricClockSink>("clock_metric_fallback_sink");
  auto* captured = static_cast<MetricClockSink*>(sink.get());
  auto* logger = Frontend::create_logger("clock_metric_fallback_logger", sink,
                                         quill::PatternFormatterOptions{}, &clock);
  auto const* metric =
    Frontend::create_metric("clock_metric_fallback_metric", "clock_metric_fallback_metric");

  // The first metric initializes the thread context; later samples force queue growth.
  constexpr size_t number_of_samples = 100;
  for (size_t i = 0; i < number_of_samples; ++i)
  {
    REQUIRE(logger->publish_metric(metric, static_cast<double>(i)));
  }
  CHECK_EQ(clock.reads.load(), number_of_samples);
  CHECK_GT(Frontend::get_thread_local_queue_capacity(), MetricClockOptions::initial_queue_capacity);

  quill::Backend::start();
  logger->flush_log();
  CHECK_EQ(clock.reads.load(), number_of_samples + 1);
  Frontend::remove_logger_blocking(logger);
  quill::Backend::stop();

  REQUIRE_EQ(captured->timestamps.size(), number_of_samples);
  REQUIRE_EQ(captured->values.size(), number_of_samples);
  for (size_t i = 0; i < number_of_samples; ++i)
  {
    CHECK_EQ(captured->timestamps[i], 0);
    CHECK_EQ(captured->values[i], doctest::Approx{static_cast<double>(i)});
  }
}
