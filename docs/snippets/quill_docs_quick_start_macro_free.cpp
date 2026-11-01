#include "quill/LogFunctions.h"
#include "quill/SimpleSetup.h"

int main()
{
  quill::Logger* logger = quill::simple_logger();
  quill::info(logger, "Hello from {}!", "Quill");
}
