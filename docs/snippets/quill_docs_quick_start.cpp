#include "quill/LogMacros.h"
#include "quill/SimpleSetup.h"

int main()
{
  // log to the console
  quill::Logger* logger = quill::simple_logger();
  LOG_INFO(logger, "Hello from {}!", "Quill");

  // log to a file
  quill::Logger* logger2 = quill::simple_logger("test.log");
  LOG_WARNING(logger2, "This message goes to a file");

  // If you also want Quill's built-in signal handler, use:
  // quill::Logger* logger3 = quill::simple_logger_with_signal_handler();
}
