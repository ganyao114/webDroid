# SDK 36 still emits a default implementation fallback for UNKNOWN_TRANSACTION.
# AOSP 17 removed getDefaultImpl(). Keep the generated status handling and wire code.
if(CMAKE_SCRIPT_MODE_FILE)
    file(READ "${INPUT}" contents)
    string(REGEX REPLACE
        "  if \\(_aidl_ret_status == ::android::UNKNOWN_TRANSACTION && [A-Za-z0-9_]+::getDefaultImpl\\(\\)\\) \\[\\[unlikely\\]\\] \\{\n     return [A-Za-z0-9_]+::getDefaultImpl\\(\\)->[^\n]+\n  \\}\n"
        "" contents "${contents}")
    if(contents MATCHES "getDefaultImpl")
        message(FATAL_ERROR "Unrecognized AIDL 36 fallback in ${INPUT}")
    endif()
    file(WRITE "${OUTPUT}" "${contents}")
    return()
endif()

include_guard(GLOBAL)

# This also supplies real libbase properties and libcutils multiuser functions.
include("${CMAKE_CURRENT_LIST_DIR}/Aosp17Core.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Aosp17Binder.cmake")

find_program(WA_AIDL_EXECUTABLE NAMES aidl
    HINTS "$ENV{ANDROID_HOME}/build-tools/36.0.0"
          "$ENV{ANDROID_SDK_ROOT}/build-tools/36.0.0"
          "$ENV{HOME}/Library/Android/sdk/build-tools/36.0.0"
    NO_CMAKE_FIND_ROOT_PATH REQUIRED)

function(aosp17_servicemanager_generate_aidl target visibility aidl_root package)
    set(gen "${CMAKE_CURRENT_BINARY_DIR}/aidl/${target}")
    foreach(type IN LISTS ARGN)
        list(APPEND inputs "${aidl_root}/${package}/${type}.aidl")
        set(raw "${gen}/raw/${package}/${type}.cpp")
        set(source "${gen}/src/${package}/${type}.cpp")
        list(APPEND raw_sources "${raw}")
        list(APPEND sources "${source}")
        # SDK 36 emits Bn/Bp headers for parcelables and unions, too.
        string(REGEX REPLACE "^I([A-Z])" "\\1" stub "${type}")
        list(APPEND headers "${gen}/include/${package}/${type}.h"
            "${gen}/include/${package}/Bn${stub}.h" "${gen}/include/${package}/Bp${stub}.h")
        list(APPEND adapt_commands COMMAND "${CMAKE_COMMAND}" "-DINPUT=${raw}"
            "-DOUTPUT=${source}" -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    endforeach()
    add_custom_command(
        OUTPUT ${sources} ${headers}
        BYPRODUCTS ${raw_sources}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen}/src/${package}"
        COMMAND "${WA_AIDL_EXECUTABLE}" --lang=cpp --omit_invocation
            "-I${aidl_root}" "-o${gen}/raw" "-h${gen}/include" ${inputs}
        ${adapt_commands}
        DEPENDS ${inputs} "${WA_AIDL_EXECUTABLE}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        VERBATIM)
    target_sources(${target} PRIVATE ${sources} ${headers})
    target_include_directories(${target} ${visibility} "${gen}/include")
endfunction()

function(_aosp17_sm_replace_driver_code before after)
    string(FIND "${driver_contents}" "${before}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "M0 Binder bridge changed at: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" driver_contents "${driver_contents}")
    set(driver_contents "${driver_contents}" PARENT_SCOPE)
endfunction()

function(_aosp17_sm_extend_binder_driver)
    # Add the missing handle-zero and death-watch commands to the existing M0
    # bridge in the build tree. The Parcel codec and ring transport stay shared.
    get_filename_component(shims "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/aosp17_shims/binder"
        ABSOLUTE)
    set(original "${shims}/wa_binder_driver.cpp")
    set(local "${CMAKE_CURRENT_BINARY_DIR}/aosp17-servicemanager/wa_binder_driver.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" driver_contents)
    _aosp17_sm_replace_driver_code([=[namespace wa::aosp_binder {]=] [=[
namespace android {
extern sp<BBinder> the_context_object;
}

namespace wa::aosp_binder {]=])
    _aosp17_sm_replace_driver_code([=[constexpr char kDevice[] = "/dev/binder";]=] [=[
constexpr char kDevice[] = "/dev/binder";
constexpr Endpoint kContextManager{1, 0};]=])
    _aosp17_sm_replace_driver_code([=[endpoint.node > 0;]=]
        [=[(endpoint.node > 0 || endpoint == kContextManager);]=])
    _aosp17_sm_replace_driver_code([=[    std::deque<uint32_t> commands;]=] [=[
    std::deque<uint32_t> commands;
    std::deque<binder_uintptr_t> clearedDeaths;]=])
    _aosp17_sm_replace_driver_code([=[    int32_t attach(void* base, size_t bytes, wa_pid_t pid);]=] [=[
    int32_t attach(void* base, size_t bytes, wa_pid_t pid);
    int becomeContextManager();]=])
    _aosp17_sm_replace_driver_code([=[    std::vector<Endpoint> handles_{{}};]=] [=[
    std::vector<Endpoint> handles_{kContextManager};
    std::map<uint32_t, binder_uintptr_t> deathWatches_;]=])
    _aosp17_sm_replace_driver_code([=[Thread& Driver::currentThread() {]=] [=[
int Driver::becomeContextManager() {
    std::lock_guard lock(mutex_);
    if (pid_ != kContextManager.pid)
        return fail(EPERM);
    if (nodes_.front())
        return fail(EBUSY);
    if (!android::the_context_object)
        return fail(EINVAL);
    nodes_.front() = android::the_context_object;
    return 0;
}

Thread& Driver::currentThread() {]=])
    _aosp17_sm_replace_driver_code([=[handles_.begin() + 1, handles_.end()]=]
        [=[handles_.begin(), handles_.end()]=])
    _aosp17_sm_replace_driver_code([=[if (!flat.handle || flat.handle >= handles_.size())]=]
        [=[if (flat.handle >= handles_.size())]=])
    _aosp17_sm_replace_driver_code([=[if (!handle || handle >= handles_.size())]=]
        [=[if (handle >= handles_.size())]=])
    _aosp17_sm_replace_driver_code([=[    case BC_INCREFS:]=] [=[
    case BC_REQUEST_DEATH_NOTIFICATION:
    case BC_CLEAR_DEATH_NOTIFICATION: {
        const auto watch = load<binder_handle_cookie>(data);
        const uint32_t handle = watch.handle;
        const binder_uintptr_t cookie = watch.cookie;
        if (handle >= handles_.size() || !cookie)
            return fail(EINVAL);
        // Published nodes are pinned and M0 has no peer-exit notification.
        // Keep the watch so clearing it can release libbinder's weak reference.
        if (command == BC_REQUEST_DEATH_NOTIFICATION) {
            return deathWatches_.emplace(handle, cookie).second ? 0 : fail(EBUSY);
        }
        const auto found = deathWatches_.find(handle);
        if (found == deathWatches_.end() || found->second != cookie)
            return fail(EINVAL);
        deathWatches_.erase(found);
        thread.clearedDeaths.push_back(cookie);
        return 0;
    }
    case BC_INCREFS:]=])
    _aosp17_sm_replace_driver_code([=[
        transaction.target.ptr = reinterpret_cast<uintptr_t>(binder->getWeakRefs());
        transaction.cookie = reinterpret_cast<uintptr_t>(binder);]=] [=[
        // A zero target is dispatched through IPCThreadState's context object.
        if (header.targetNode != kContextManager.node) {
            transaction.target.ptr = reinterpret_cast<uintptr_t>(binder->getWeakRefs());
            transaction.cookie = reinterpret_cast<uintptr_t>(binder);
        }]=])
    _aosp17_sm_replace_driver_code([=[        if (thread.stopping && thread.calls.empty()) {]=] [=[
        if (!thread.clearedDeaths.empty()) {
            auto* output = addressOf<uint8_t>(io.read_buffer);
            store(output, uint32_t{BR_CLEAR_DEATH_NOTIFICATION_DONE});
            store(output + sizeof(uint32_t), thread.clearedDeaths.front());
            thread.clearedDeaths.pop_front();
            io.read_consumed = sizeof(uint32_t) + sizeof(binder_uintptr_t);
            return 0;
        }
        if (thread.stopping && thread.calls.empty()) {]=])
    # Upstream ProcessState falls back to this ioctl when the driver has no
    # extended SELinux security-context support.
    _aosp17_sm_replace_driver_code([=[    case BINDER_SET_MAX_THREADS:]=] [=[
    case BINDER_SET_CONTEXT_MGR:
        return argument ? driver().becomeContextManager() : fail(EFAULT);
    case BINDER_SET_MAX_THREADS:]=])

    get_target_property(sources aosp17_libbinder SOURCES)
    if(NOT original IN_LIST sources)
        message(FATAL_ERROR "Expected ${original} in aosp17_libbinder")
    endif()
    file(CONFIGURE OUTPUT "${local}" CONTENT "${driver_contents}" @ONLY)
    list(REMOVE_ITEM sources "${original}")
    list(APPEND sources "${local}")
    set_property(TARGET aosp17_libbinder PROPERTY SOURCES "${sources}")
endfunction()

_aosp17_sm_extend_binder_driver()

set(_aosp17_sm_native "${AOSP17_SOURCE_DIR}/frameworks/native")
set(_aosp17_sm_source "${_aosp17_sm_native}/cmds/servicemanager")
add_library(aosp17_servicemanager STATIC "${_aosp17_sm_source}/ServiceManager.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/aosp17_shims/servicemanager/Access_wasm.cpp")
target_include_directories(aosp17_servicemanager PUBLIC "${_aosp17_sm_source}")
target_include_directories(aosp17_servicemanager SYSTEM PUBLIC
    "${_aosp17_sm_native}/libs/binder/ndk/include_cpp")
# Use AOSP's build variant for a Binder domain without VINTF or Perfetto.
target_compile_definitions(aosp17_servicemanager PUBLIC VENDORSERVICEMANAGER=1
    PRIVATE ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION)
target_link_libraries(aosp17_servicemanager PUBLIC aosp17_libbinder aosp17_libbase aosp17_libutils)
if(EMSCRIPTEN)
    target_compile_options(aosp17_servicemanager PRIVATE -O3)
endif()
aosp17_servicemanager_generate_aidl(aosp17_servicemanager PUBLIC
    "${_aosp17_sm_native}/libs/binder/aidl" android/os
    ConnectionInfo IClientCallback IServiceCallback IServiceManager Service ServiceDebugInfo
    ServiceWithMetadata)
