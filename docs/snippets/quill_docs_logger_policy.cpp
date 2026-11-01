#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/LoggerPolicy.h"
#include "quill/sinks/ConsoleSink.h"

// Inherit defaults so future policy members need no changes here.
struct FastLoggerPolicy : quill::LoggerPolicy
{
  static constexpr bool enable_immediate_flush = false;
};

struct SystemLoggerPolicy : quill::LoggerPolicy
{
  using clock_t = quill::SystemClock;
};

int main()
{
  quill::Backend::start();
  auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("policy_sink");

  auto* fast = quill::Frontend::create_or_get_logger<FastLoggerPolicy>("fast", sink);
  auto* system = quill::Frontend::create_or_get_logger<SystemLoggerPolicy>("system", sink);
  system->set_immediate_flush(1);

  LOG_INFO(fast, "TSC clock without automatic flush support");
  LOG_INFO(system, "System clock with automatic flushing");

  auto* same = quill::Frontend::get_logger<FastLoggerPolicy>("fast");
  same->flush_log(); // Explicit flushing works with either policy.
  quill::Frontend::remove_logger_blocking(fast);
  quill::Frontend::remove_logger_blocking(system);
  quill::Backend::stop();
}
