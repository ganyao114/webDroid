#pragma once

#include "wasmandroid/abi.h"

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define WA_SERVICEMANAGER_PID 1u

namespace wa::binder {

struct BinderHandle {
    wa_pid_t owner_pid = 0;
    uint32_t handle_id = 0;

    explicit operator bool() const { return owner_pid != 0; }
    bool operator==(const BinderHandle&) const = default;
};

class IBinder;
class BinderProcess;

// Values use the Android little-endian Parcel layout and four-byte alignment.
// Strong binders use two uint32_t words (owner_pid, handle_id); (0, 0) is null.
// Reads and writes advance the same cursor. A received Parcel starts at zero.
class Parcel {
  public:
    Parcel() = default;
    Parcel(const Parcel&) = default;
    Parcel& operator=(const Parcel&) = default;
    Parcel(Parcel&& other) noexcept;
    Parcel& operator=(Parcel&& other) noexcept;

    std::span<const uint8_t> data() const { return bytes_; }
    size_t dataPosition() const { return position_; }
    size_t dataAvail() const { return bytes_.size() - position_; }
    int32_t setData(std::span<const uint8_t> bytes);
    int32_t setDataPosition(size_t position) const;

    int32_t writeInt32(int32_t value);
    int32_t writeInt64(int64_t value);
    int32_t writeFloat(float value);
    int32_t writeCString(std::string_view value);
    int32_t writeString16(std::u16string_view value);
    int32_t writeString16(std::nullptr_t);
    int32_t writeByteArray(std::span<const uint8_t> value);
    int32_t writeByteArray(std::nullptr_t);
    int32_t writeInterfaceToken(std::u16string_view descriptor);
    int32_t writeStrongBinder(BinderHandle handle);
    int32_t writeStrongBinder(const std::shared_ptr<IBinder>& binder);

    int32_t readInt32(int32_t& value) const;
    int32_t readInt64(int64_t& value) const;
    int32_t readFloat(float& value) const;
    int32_t readCString(std::string& value) const;
    int32_t readString16(std::u16string& value, bool* is_null = nullptr) const;
    int32_t readByteArray(std::vector<uint8_t>& value, bool* is_null = nullptr) const;
    bool enforceInterface(std::u16string_view descriptor) const;
    int32_t readStrongBinder(BinderHandle& handle) const;
    int32_t readStrongBinder(std::shared_ptr<IBinder>& binder) const;

  private:
    int32_t writeInPlace(size_t size, uint8_t*& out);
    int32_t writeBytes(std::span<const uint8_t> prefix, const void* data, size_t size,
                       size_t terminator);
    int32_t writeArray(const void* data, size_t size, int32_t count, size_t terminator);
    const uint8_t* peek(size_t offset, size_t size) const;

    template <typename T> int32_t writeScalar(T value);
    template <typename T> int32_t readScalar(T& value) const;
    template <typename Array>
    int32_t readArray(Array& value, size_t terminator, bool* is_null) const;

    std::vector<uint8_t> bytes_;
    mutable size_t position_ = 0;
};

class IBinder {
  public:
    static constexpr uint32_t FIRST_CALL_TRANSACTION = 1;
    static constexpr uint32_t FLAG_ONEWAY = 1;

    virtual ~IBinder() = default;
    virtual BinderHandle handle() const = 0;
    virtual int32_t transact(uint32_t code, const Parcel& data, Parcel* reply,
                             uint32_t flags = 0) = 0;
};

class BBinder : public IBinder {
  public:
    BinderHandle handle() const final;
    int32_t transact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags = 0) final;

  protected:
    virtual int32_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags);

  private:
    friend class BinderProcess;
    std::atomic<uint64_t> address_{0};
};

// A submitted two-way call can be waited on later. Destruction abandons an
// outstanding reply. The BinderProcess must outlive its transactions and proxies.
class BinderTransaction {
  public:
    BinderTransaction() = default;
    ~BinderTransaction();
    BinderTransaction(const BinderTransaction&) = delete;
    BinderTransaction& operator=(const BinderTransaction&) = delete;
    BinderTransaction(BinderTransaction&& other) noexcept;
    BinderTransaction& operator=(BinderTransaction&& other) noexcept;

    int32_t wait(Parcel* reply, int32_t timeout_ms = 5000);

  private:
    friend class BinderProcess;
    void reset();

    BinderProcess* process_ = nullptr;
    uint32_t id_ = 0;
};

class BpBinder final : public IBinder {
  public:
    BpBinder(BinderProcess& process, BinderHandle handle);
    BinderHandle handle() const override { return handle_; }
    int32_t transact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags = 0) override;
    int32_t beginTransact(uint32_t code, const Parcel& data, BinderTransaction& transaction,
                          uint32_t flags = 0);

  private:
    BinderProcess& process_;
    BinderHandle handle_;
};

class BinderProcess {
  public:
    // With a null base, Wasm uses its worker's separate Binder memory bridge.
    BinderProcess(wa_pid_t pid, void* shared_ring_base, size_t ring_bytes);
    ~BinderProcess();
    BinderProcess(const BinderProcess&) = delete;
    BinderProcess& operator=(const BinderProcess&) = delete;

    // Attach every execution thread before it uses this vproc's Binder APIs.
    int32_t attachThread();
    static BinderProcess* current();
    wa_pid_t pid() const { return pid_; }
    int32_t registerBinder(const std::shared_ptr<BBinder>& binder);
    std::shared_ptr<IBinder> getBinder(BinderHandle handle);

    // Dispatch either one incoming call or one correlated reply. It is safe
    // for several attached threads to pump at once, including during callbacks.
    int32_t pollOnce(int32_t timeout_ms);
    int32_t serve(const std::atomic_bool& stop);

  private:
    friend class BpBinder;
    friend class BinderTransaction;
    friend class ServiceManager;

    int32_t publish(const std::shared_ptr<BBinder>& binder, bool context_manager);
    int32_t send(BinderHandle target, uint32_t code, const Parcel& data, uint32_t flags,
                 BinderTransaction* transaction);
    int32_t wait(uint32_t id, Parcel* reply, int32_t timeout_ms);
    void abandon(uint32_t id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    wa_pid_t pid_;
    void* ring_base_;
    size_t ring_bytes_;
};

inline constexpr uint32_t WA_SERVICEMANAGER_HANDLE = 0;
inline constexpr std::u16string_view kServiceManagerDescriptor = u"android.os.IServiceManager";
inline constexpr uint32_t GET_SERVICE_TRANSACTION = IBinder::FIRST_CALL_TRANSACTION;
inline constexpr uint32_t CHECK_SERVICE_TRANSACTION = GET_SERVICE_TRANSACTION + 1;
inline constexpr uint32_t ADD_SERVICE_TRANSACTION = GET_SERVICE_TRANSACTION + 2;
inline constexpr uint32_t LIST_SERVICES_TRANSACTION = GET_SERVICE_TRANSACTION + 3;

class ServiceManager final : public BBinder {
  public:
    static int32_t publish(BinderProcess& process);
    ~ServiceManager() override;

  private:
    ServiceManager();
    int32_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override;

    struct Registry;
    std::unique_ptr<Registry> registry_;
};

class BpServiceManager final {
  public:
    explicit BpServiceManager(BinderProcess& process);

    int32_t addService(std::string_view name, BinderHandle handle, bool allow_isolated = false,
                       int32_t dump_priority = 8);
    std::shared_ptr<BpBinder> getService(std::string_view name, int32_t* status = nullptr);
    std::shared_ptr<BpBinder> checkService(std::string_view name, int32_t* status = nullptr);
    std::vector<std::string> listServices(int32_t dump_priority = 15, int32_t* status = nullptr);

  private:
    std::shared_ptr<BpBinder> lookup(uint32_t code, std::string_view name, int32_t* status);

    BinderProcess& process_;
    BpBinder remote_;
};

std::shared_ptr<BpServiceManager> defaultServiceManager();

} // namespace wa::binder
