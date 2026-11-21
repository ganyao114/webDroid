#include "wasmandroid/binder.h"

#include "kernel_internal.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace wa::binder {
namespace {

template <typename String> bool validName(String name) {
    return !name.empty() && name.size() <= 127 && std::all_of(name.begin(), name.end(), [](auto c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '/';
    });
}

int32_t writeName(Parcel& data, std::string_view name) {
    return validName(name) ? data.writeString16(std::u16string(name.begin(), name.end()))
                           : WA_ERR_INVALID;
}

int32_t managerTransact(BpBinder& binder, uint32_t code, const Parcel& request, Parcel& reply) {
    const int32_t status = binder.transact(code, request, &reply);
    if (status != WA_OK) {
        return status;
    }
    int32_t exception;
    return reply.readInt32(exception) == WA_OK && exception == 0 ? WA_OK : WA_ERR_INVALID;
}

} // namespace

struct ServiceManager::Registry {
    struct Service {
        BinderHandle binder;
        int32_t dump_priority;
    };

    FutexMutex mutex;
    std::map<std::u16string, Service> services;
};

ServiceManager::ServiceManager() : registry_(std::make_unique<Registry>()) {}
ServiceManager::~ServiceManager() = default;

int32_t ServiceManager::publish(BinderProcess& process) {
    if (process.pid() != WA_SERVICEMANAGER_PID) {
        return WA_ERR_INVALID;
    }
    try {
        return process.publish(std::shared_ptr<ServiceManager>(new ServiceManager), true);
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

int32_t ServiceManager::onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t) {
    if (!reply || !data.enforceInterface(kServiceManagerDescriptor)) {
        return WA_ERR_INVALID;
    }
    if (const int32_t status = reply->writeInt32(0); status != WA_OK) {
        return status;
    }

    if (code == LIST_SERVICES_TRANSACTION) {
        int32_t dump_priority;
        if (data.readInt32(dump_priority) != WA_OK || data.dataAvail()) {
            return WA_ERR_INVALID;
        }
        const size_t count_position = reply->dataPosition();
        if (const int32_t status = reply->writeInt32(0); status != WA_OK) {
            return status;
        }
        int32_t count = 0;
        std::lock_guard lock(registry_->mutex);
        for (const auto& [name, service] : registry_->services) {
            if (service.dump_priority & dump_priority) {
                if (const int32_t status = reply->writeString16(name); status != WA_OK) {
                    return status;
                }
                ++count;
            }
        }
        const size_t end = reply->dataPosition();
        reply->setDataPosition(count_position);
        const int32_t status = reply->writeInt32(count);
        reply->setDataPosition(end);
        return status;
    }

    if (code != ADD_SERVICE_TRANSACTION && code != GET_SERVICE_TRANSACTION &&
        code != CHECK_SERVICE_TRANSACTION) {
        return WA_ERR_NOSYS;
    }
    std::u16string name;
    if (data.readString16(name) != WA_OK || !validName(std::u16string_view(name))) {
        return WA_ERR_INVALID;
    }
    if (code == ADD_SERVICE_TRANSACTION) {
        BinderHandle handle;
        int32_t allow_isolated, dump_priority;
        if (data.readStrongBinder(handle) != WA_OK || !handle ||
            data.readInt32(allow_isolated) != WA_OK ||
            (allow_isolated != 0 && allow_isolated != 1) ||
            data.readInt32(dump_priority) != WA_OK || data.dataAvail()) {
            return WA_ERR_INVALID;
        }
        std::lock_guard lock(registry_->mutex);
        registry_->services.insert_or_assign(std::move(name),
                                             Registry::Service{handle, dump_priority});
        return WA_OK;
    }
    if (data.dataAvail()) {
        return WA_ERR_INVALID;
    }
    std::lock_guard lock(registry_->mutex);
    const auto it = registry_->services.find(name);
    return reply->writeStrongBinder(it == registry_->services.end() ? BinderHandle{}
                                                                    : it->second.binder);
}

BpServiceManager::BpServiceManager(BinderProcess& process)
    : process_(process), remote_(process, {WA_SERVICEMANAGER_PID, WA_SERVICEMANAGER_HANDLE}) {}

int32_t BpServiceManager::addService(std::string_view name, BinderHandle handle,
                                     bool allow_isolated, int32_t dump_priority) {
    Parcel request, reply;
    int32_t status = request.writeInterfaceToken(kServiceManagerDescriptor);
    if (status == WA_OK)
        status = writeName(request, name);
    if (status == WA_OK)
        status = request.writeStrongBinder(handle);
    if (status == WA_OK)
        status = request.writeInt32(allow_isolated);
    if (status == WA_OK)
        status = request.writeInt32(dump_priority);
    return status == WA_OK ? managerTransact(remote_, ADD_SERVICE_TRANSACTION, request, reply)
                           : status;
}

std::shared_ptr<BpBinder> BpServiceManager::lookup(uint32_t code, std::string_view name,
                                                   int32_t* result) {
    Parcel request, reply;
    BinderHandle handle;
    int32_t status = request.writeInterfaceToken(kServiceManagerDescriptor);
    if (status == WA_OK)
        status = writeName(request, name);
    if (status == WA_OK)
        status = managerTransact(remote_, code, request, reply);
    if (status == WA_OK)
        status = reply.readStrongBinder(handle);
    if (status == WA_OK && reply.dataAvail())
        status = WA_ERR_INVALID;
    if (result)
        *result = status;
    return status == WA_OK && handle ? std::make_shared<BpBinder>(process_, handle) : nullptr;
}

std::shared_ptr<BpBinder> BpServiceManager::getService(std::string_view name, int32_t* status) {
    return lookup(GET_SERVICE_TRANSACTION, name, status);
}

std::shared_ptr<BpBinder> BpServiceManager::checkService(std::string_view name, int32_t* status) {
    return lookup(CHECK_SERVICE_TRANSACTION, name, status);
}

std::vector<std::string> BpServiceManager::listServices(int32_t dump_priority, int32_t* result) {
    Parcel request, reply;
    int32_t status = request.writeInterfaceToken(kServiceManagerDescriptor);
    if (status == WA_OK)
        status = request.writeInt32(dump_priority);
    if (status == WA_OK)
        status = managerTransact(remote_, LIST_SERVICES_TRANSACTION, request, reply);
    std::vector<std::string> services;
    int32_t count = 0;
    if (status == WA_OK && (reply.readInt32(count) != WA_OK || count < 0 ||
                            static_cast<size_t>(count) > reply.dataAvail() / 8)) {
        status = WA_ERR_INVALID;
    }
    while (status == WA_OK && count-- > 0) {
        std::u16string name;
        status = reply.readString16(name);
        if (status == WA_OK && validName(std::u16string_view(name))) {
            services.emplace_back(name.begin(), name.end());
        } else if (status == WA_OK) {
            status = WA_ERR_INVALID;
        }
    }
    if (status == WA_OK && reply.dataAvail())
        status = WA_ERR_INVALID;
    if (result)
        *result = status;
    if (status != WA_OK)
        services.clear();
    return services;
}

std::shared_ptr<BpServiceManager> defaultServiceManager() {
    auto* process = BinderProcess::current();
    return process ? std::make_shared<BpServiceManager>(*process) : nullptr;
}

} // namespace wa::binder
