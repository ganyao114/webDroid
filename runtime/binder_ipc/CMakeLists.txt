# Deprecated. The unified runtime uses aosp17_libbinder.
add_library(wa_binder_ipc INTERFACE)
target_link_libraries(wa_binder_ipc INTERFACE wa_libkern)
