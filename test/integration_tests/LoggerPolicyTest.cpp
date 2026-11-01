#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogFunctions.h"
#include "quill/LogMacros.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct FastPolicy : quill::LoggerPolicy
{
  static constexpr bool enable_immediate_flush = false;
};

struct SystemPolicy : quill::LoggerPolicy
{
  using clock_t = quill::SystemClock;
};

struct PolicySink : quill::Sink
{
  void write_log(quill::MacroMetadata const*, uint64_t, std::string_view, std::string_view,
                 std::string const&, std::string_view, quill::LogLevel, std::string_view,
                 std::string_view, std::vector<std::pair<std::string, std::string>> const*,
                 std::string_view, std::string_view) override
  {
    writes.fetch_add(1, std::memory_order_relaxed);
  }

  void flush_sink() noexcept override {}
  void flush_sink(quill::SinkFlushReason reason) override
  {
    if (reason == quill::SinkFlushReason::Explicit)
    {
      explicit_flushes.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::atomic<size_t> writes{0};
  std::atomic<size_t> explicit_flushes{0};
};
} // namespace

TEST_CASE("logger_policy_immediate_flush")
{
  auto sink = quill::Frontend::create_sink<PolicySink>("logger_policy_sink");
  auto* captured = static_cast<PolicySink*>(sink.get());
  auto* enabled = quill::Frontend::create_logger<SystemPolicy>("logger_policy_enabled", sink);
  auto* disabled = quill::Frontend::create_logger<FastPolicy>("logger_policy_disabled", {sink});
  enabled->set_immediate_flush(1);
  disabled->set_immediate_flush(1);
  CHECK_EQ(enabled->get_clock_source_type(), quill::ClockSourceType::System);
  CHECK_EQ(disabled->get_clock_source_type(), quill::ClockSourceType::Tsc);
  CHECK_EQ(quill::Frontend::get_logger<FastPolicy>("logger_policy_disabled"), disabled);
  CHECK_EQ(quill::Frontend::get_all_loggers<FastPolicy>().size(), 1);
#if !defined(QUILL_NO_EXCEPTIONS)
  CHECK_THROWS_AS((void)quill::Frontend::get_logger("logger_policy_disabled"), quill::QuillError);
#endif
  quill::Backend::start();
  LOG_INFO(enabled, "enabled macro");
  CHECK_EQ(captured->explicit_flushes.load(), 1);
  quill::info(enabled, "enabled function");
  CHECK_EQ(captured->explicit_flushes.load(), 2);
  LOG_INFO_LIMIT(std::chrono::nanoseconds{0}, enabled, "enabled limited macro");
  CHECK_EQ(captured->explicit_flushes.load(), 3);

  LOG_INFO(disabled, "disabled macro");
  quill::info(disabled, "disabled function");
  LOG_INFO_LIMIT(std::chrono::nanoseconds{0}, disabled, "disabled limited macro");
  disabled->flush_log();
  CHECK_EQ(captured->explicit_flushes.load(), 4);
  CHECK_EQ(captured->writes.load(), 6);

  auto* clone = quill::Frontend::create_or_get_logger("logger_policy_clone", disabled);
  CHECK_EQ(quill::Frontend::get_logger<FastPolicy>("logger_policy_clone"), clone);
  quill::Frontend::remove_logger_blocking(clone);
  quill::Frontend::remove_logger_blocking(disabled);
  quill::Frontend::remove_logger_blocking(enabled);
  quill::Backend::stop();
}
