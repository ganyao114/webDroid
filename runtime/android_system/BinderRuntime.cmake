include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Aosp17ServiceManager.cmake")

set(binder_upstream "${AOSP17_SOURCE_DIR}/frameworks/native/libs/binder")
set(binder_overlay "${CMAKE_CURRENT_BINARY_DIR}/aosp17-system-binder")

# The C++ client shim uses the same generated IServiceManager as the server.
# Enable the ported Binder device on the host and omit the adb delegate.
_wa_binder_port_source("${binder_upstream}/IServiceManager.cpp"
    "${binder_overlay}/IServiceManager.cpp"
    "#if !(defined(__ANDROID__) || defined(__FUCHSIA))" "#if 0 // The vproc uses /dev/binder.")
_wa_binder_port_source("${binder_upstream}/BackendUnifiedServiceManager.cpp"
    "${binder_overlay}/BackendUnifiedServiceManager.cpp"
    [=[#if !defined(BINDER_WITH_KERNEL_IPC) || !defined(__BIONIC__)
    return false;
#else
#ifdef __ANDROID_VNDK__]=]
    [=[#if !defined(BINDER_WITH_KERNEL_IPC)
    return false;
#else
#if !defined(__BIONIC__) || defined(__ANDROID_VNDK__)]=])
target_sources(aosp17_servicemanager PRIVATE
    "${binder_overlay}/IServiceManager.cpp" "${binder_overlay}/BackendUnifiedServiceManager.cpp"
    "${binder_upstream}/IPermissionController.cpp")
target_include_directories(aosp17_servicemanager PRIVATE "${binder_upstream}"
    "${binder_upstream}/include_private" "${_wa_binder_shims}/include")
target_compile_definitions(aosp17_servicemanager PRIVATE BINDER_WITH_KERNEL_IPC)
if(APPLE)
    target_compile_options(aosp17_servicemanager PRIVATE -include "${_wa_binder_shims}/darwin.h")
endif()
aosp17_servicemanager_generate_aidl(aosp17_servicemanager PRIVATE
    "${binder_upstream}/aidl" android/os IAccessor)

# Extend the existing M0 driver after the context-manager overlay. The AOSP
# Parcel codec, object fixups and transaction routing are shared with M4/M5.
function(_wa_system_extend_binder_driver)
    set(base "${CMAKE_CURRENT_BINARY_DIR}/aosp17-servicemanager/wa_binder_driver.cpp")
    set(port "${binder_overlay}/wa_binder_driver.cpp")
    file(READ "${base}" driver_contents)
    _aosp17_sm_replace_driver_code([=[#include "wa_binder_syscalls.h"]=] [=[
#include "wa_binder_syscalls.h"
#include "binder_bridge.h"

#include <binder/IPCThreadState.h>
#include <utility>
]=])
    _aosp17_sm_replace_driver_code([=[thread_local bool ringAttached;]=] [=[
thread_local bool ringAttached;
thread_local int pollTimeout = -1;]=])
    _aosp17_sm_replace_driver_code([=[    std::deque<Packet> requests_;]=] [=[
    std::deque<Packet> requests_;
    BinderPacketReader packetReader_;]=])
    _aosp17_sm_replace_driver_code([=[WA_BINDER_MAX_PAYLOAD - sizeof(header)]=]
        [=[kMaxBinderWireBytes - sizeof(header)]=])
    _aosp17_sm_replace_driver_code([=[    using namespace std::chrono_literals;
    lock.unlock();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    int32_t result;
    do {
        result = wa_binder_send(dst, id, code, flags, bytes.data(), bytes.size());
        if (result != WA_ERR_AGAIN)
            break;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    lock.lock();]=] [=[
    lock.unlock();
    const auto result = sendBinderPacket(dst, id, code, flags, bytes);
    lock.lock();]=])
    _aosp17_sm_replace_driver_code([=[int Driver::read(Lock& lock, Thread& thread, binder_write_read& io) {
    for (;;) {]=] [=[
int Driver::read(Lock& lock, Thread& thread, binder_write_read& io) {
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    const int timeout = std::exchange(pollTimeout, -1);
    const auto deadline = Clock::now() + milliseconds(timeout);
    bool readRing = false;
    for (;;) {]=])
    _aosp17_sm_replace_driver_code([=[        if (receiving_) {
            ready_.wait(lock);
            continue;
        }]=] [=[
        if (receiving_) {
            if (timeout < 0) {
                ready_.wait(lock);
            } else if (ready_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return 0;
            }
            continue;
        }
        const auto remaining = std::chrono::ceil<milliseconds>(deadline - Clock::now()).count();
        if (timeout >= 0 && remaining <= 0 && readRing)
            return 0;
        const int wait = timeout < 0 ? 50 : std::clamp<int64_t>(remaining, 0, 50);
        readRing = true;]=])
    _aosp17_sm_replace_driver_code([=[        packet.bytes.resize(WA_BINDER_MAX_PAYLOAD);
        const auto result =
            wa_binder_recv(&packet.message, packet.bytes.data(), packet.bytes.size(), 50);]=] [=[
        const auto result = packetReader_.receive(packet.message, packet.bytes, wait);]=])
    _aosp17_sm_replace_driver_code([=[void stopLooper() {
    driver().stopLooper();
}]=] [=[
void stopLooper() {
    driver().stopLooper();
}

int32_t pollCommands(int timeout_ms) {
    pollTimeout = timeout_ms;
    const auto status = android::IPCThreadState::self()->handlePolledCommands();
    pollTimeout = -1;
    return status;
}]=])
    get_target_property(sources aosp17_libbinder SOURCES)
    list(REMOVE_ITEM sources "${base}")
    list(APPEND sources "${port}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/binder_bridge.cpp")
    set_property(TARGET aosp17_libbinder PROPERTY SOURCES "${sources}")
    target_include_directories(aosp17_libbinder PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    file(CONFIGURE OUTPUT "${port}" CONTENT "${driver_contents}" @ONLY)
endfunction()
_wa_system_extend_binder_driver()
