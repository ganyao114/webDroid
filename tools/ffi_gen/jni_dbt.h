#pragma once

#include "generated/jni_vtable_thunks.h"

namespace wa::ffi {

// Bind one callback provider per ARM64 DBT. The provider must outlive the DBT.
// The returned PCs can be installed in any JNIEnv that uses this provider.
[[nodiscard]] int32_t bind_jni_stubs(wa_dbt_t* dbt, JniCallbacks& callbacks, JniStubPcs& out);

} // namespace wa::ffi
