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
  void VisitRoots(const std::function<void(Object*)>& mark) const;
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
  void DeleteLocal(jobject reference);
  void DeleteGlobal(jobject reference);
  void PushFrame(size_t capacity);
  jobject PopFrame(jobject keep);
  size_t FrameDepth() const { return frames_.size(); }
  size_t SetFrameFloor(size_t depth) { return std::exchange(frame_floor_, depth); }
  void RestoreFrameDepth(size_t depth) { frames_.resize(depth); }
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
  std::unordered_map<JniReference*, std::unique_ptr<JniReference>> globals_;
  std::unordered_map<void*, NativeThunk> adapters_;
  std::map<std::string, uint64_t, std::less<>> calls_;
  size_t frame_floor_ = 1;
  bool resources_registered_ = false;
};

}  // namespace wa::interp
