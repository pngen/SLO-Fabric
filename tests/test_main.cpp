#include "test_framework.hpp"

int main(int argc, char** argv) {
  const char* filter = argc >= 2 ? argv[1] : nullptr;
  return ::slofabric_test::run_all(filter);
}
