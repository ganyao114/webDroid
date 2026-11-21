#include "Access.h"

#include <binder/IPCThreadState.h>

namespace android {

Access::Access() = default;
Access::~Access() = default;

Access::CallingContext Access::getCallingContext() {
    auto* ipc = IPCThreadState::self();
    const char* sid = ipc->getCallingSid();
    return {ipc->getCallingPid(), ipc->getCallingUid(), sid ? sid : ""};
}

std::string Access::CallingContext::toDebugString() const {
    return "Caller(pid=" + std::to_string(debugPid) + ",uid=" + std::to_string(uid) +
           ",sid=" + sid + ")";
}

bool Access::canFind(const CallingContext&, const std::string&) {
    return true;
}

bool Access::canAdd(const CallingContext&, const std::string&) {
    return true;
}

bool Access::canList(const CallingContext&) {
    return true;
}

}  // namespace android
