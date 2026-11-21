#include "runtime_internal.h"

#include <exception>

// The GC probe links the real collector without loading JNI. Its heap never
// owns a JniBridge, so reaching the deleter would be a test configuration error.
void wa::art::JniDeleter::operator()(JniBridge*) const {
  std::terminate();
}
