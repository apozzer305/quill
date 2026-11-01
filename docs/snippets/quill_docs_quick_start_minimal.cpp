#include "quill/LogMacros.h"
#include "quill/SimpleSetup.h"

int main()
{
  quill::Logger* logger = quill::simple_logger();
  LOG_INFO(logger, "Hello from {}!", "Quill");
}
