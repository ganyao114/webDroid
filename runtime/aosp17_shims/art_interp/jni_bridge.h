#pragma once

#include <thread>

#include "runtime_internal.h"

namespace wa::interp {

struct JniReference {
  Object* object;
  jobjectRefType type;
};

class JniBridge {
 public:
  explicit JniBridge(Runtime& runtime);
  ~JniBridge();
  void RegisterResources();
  Value Invoke(Method& method, Object* receiver, std::span<const Value> args);
  Value InvokePlatform(Method& method, Object* receiver, std::span<const Value> args);
  void VisitRoots(const std::function<void(Object*)>& mark) const;
  void RetainNativePeer(jobject peer, std::shared_ptr<void> allocation);
  void SweepNativePeers();
  uint64_t Calls(std::string_view owner) const;

  JNIEnv* Env() { return &env_; }
  JavaVM* JavaVm() { return &vm_; }
  Runtime& Vm() { return runtime_; }
  bool IsVmThread() const { return owner_thread_ == std::this_thread::get_id(); }

  static JniBridge& From(JNIEnv* env) { return static_cast<Environment*>(env)->bridge; }
  static JniBridge& From(JavaVM* vm) { return static_cast<VirtualMachine*>(vm)->bridge; }
  static Object* Read(jobject object);
  jobject Local(Object* object);
  jobject Global(Object* object);
  jweak Weak(Object* object);
  void DeleteLocal(jobject reference);
  void DeleteGlobal(jobject reference);
  void DeleteWeak(jweak reference);
  void PushFrame(size_t capacity);
  jobject PopFrame(jobject keep);
  size_t FrameDepth() const { return frame_depth_; }
  size_t SetFrameFloor(size_t depth) { return std::exchange(frame_floor_, depth); }
  void RestoreFrameDepth(size_t depth);
  Class& AsClass(jclass cls);
  void RegisterAdapter(void* function, NativeThunk thunk);
  NativeThunk FindAdapter(void* function) const;

 private:
  struct Environment : JNIEnv { JniBridge& bridge; };
  struct VirtualMachine : JavaVM { JniBridge& bridge; };

  Runtime& runtime_;
  const std::thread::id owner_thread_ = std::this_thread::get_id();
  Environment env_;
  VirtualMachine vm_;
  std::vector<std::vector<std::unique_ptr<JniReference>>> frames_;
  size_t frame_depth_ = 0;
  std::unordered_map<JniReference*, std::unique_ptr<JniReference>> globals_, weak_globals_;
  std::unordered_multimap<Object*, std::shared_ptr<void>> native_peers_;
  std::unordered_map<void*, NativeThunk> adapters_;
  size_t frame_floor_ = 1;
  bool resources_registered_ = false;
};

}  // namespace wa::interp
