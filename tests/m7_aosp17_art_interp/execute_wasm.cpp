#include "art_interp.h"

#include <stdexcept>

#if defined(WA_TEST_HGRAPH) && defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(WA_TEST_HGRAPH) && defined(__APPLE__)
#include <JavaScriptCore/JavaScriptCore.h>

#include <cstring>
#endif

namespace wa::interp::test {

#if defined(WA_TEST_HGRAPH) && defined(__EMSCRIPTEN__)

EM_JS(int, Install, (const uint8_t* bytes, size_t size, const char* symbol), {
  const state = Module['tier1'] || (Module['tier1'] = { next: 1, functions: new Map() });
  const compiled = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + size));
  const fn = new WebAssembly.Instance(compiled).exports[UTF8ToString(symbol)];
  const id = state.next++;
  state.functions.set(id, fn);
  return id;
});

EM_JS(void, Invoke, (int id, const char* shorty, const uint64_t* words, uint64_t* result), {
  const types = UTF8ToString(shorty);
  const args = [];
  for (let i = 1; i < types.length; ++i) {
    const offset = words + (i - 1) * 8;
    switch (types[i]) {
      case 'J': args.push(HEAP64[offset / 8]); break;
      case 'D': args.push(HEAPF64[offset / 8]); break;
      case 'F': args.push(HEAPF32[offset / 4]); break;
      default: args.push(HEAP32[offset / 4]); break;
    }
  }
  const value = Module['tier1'].functions.get(id)(...args);
  switch (types[0]) {
    case 'V': break;
    case 'J': HEAP64[result / 8] = value; break;
    case 'D': HEAPF64[result / 8] = value; break;
    case 'F': HEAPF32[result / 4] = value; break;
    default: HEAP32[result / 4] = value; break;
  }
});

EM_JS(void, Remove, (int id), { Module['tier1'].functions.delete(id); });

class Installed {
 public:
  explicit Installed(int index) : id(index) {}
  ~Installed() { Remove(id); }
  const int id;
};

#elif defined(WA_TEST_HGRAPH) && defined(__APPLE__)
namespace {

class JsString {
 public:
  explicit JsString(const char* text) : value_(JSStringCreateWithUTF8CString(text)) {}
  ~JsString() { JSStringRelease(value_); }
  operator JSStringRef() const { return value_; }

 private:
  JSStringRef value_;
};

class Installed {
 public:
  Installed(std::span<const uint8_t> module, std::string_view name) {
    context_ = JSGlobalContextCreate(nullptr);
    try {
      JSValueRef exception = nullptr;
      auto bytes = JSObjectMakeTypedArray(context_, kJSTypedArrayTypeUint8Array, module.size(), &exception);
      Check(exception);
      auto* ptr = JSObjectGetTypedArrayBytesPtr(context_, bytes, &exception);
      Check(exception);
      std::memcpy(ptr, module.data(), module.size());
      const auto global = JSContextGetGlobalObject(context_);
      JSObjectSetProperty(context_, global, JsString("moduleBytes"), bytes, kJSPropertyAttributeNone, &exception);
      Check(exception);
      JSObjectSetProperty(context_, global, JsString("entryName"),
          JSValueMakeString(context_, JsString(std::string(name).c_str())), kJSPropertyAttributeNone, &exception);
      Check(exception);
      auto function = JSEvaluateScript(context_, JsString(
          "new WebAssembly.Instance(new WebAssembly.Module(moduleBytes)).exports[entryName]"),
          nullptr, nullptr, 1, &exception);
      Check(exception);
      export_ = JSValueToObject(context_, function, &exception);
      Check(exception);
      if (!JSObjectIsFunction(context_, export_)) throw std::runtime_error("Missing HGraph function");
      JSValueProtect(context_, export_);
    } catch (...) {
      JSGlobalContextRelease(context_);
      throw;
    }
  }

  ~Installed() {
    JSValueUnprotect(context_, export_);
    JSGlobalContextRelease(context_);
  }

  Value Call(std::string_view types, std::span<const Value> values) {
    JSValueRef exception = nullptr;
    struct Arguments {
      JSContextRef context;
      std::vector<JSValueRef> refs;
      ~Arguments() { for (auto value : refs) JSValueUnprotect(context, value); }
    } args{context_, {}};
    args.refs.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      Value value = values[i];
      JSValueRef boxed;
      switch (types[i + 1]) {
        case 'J': boxed = JSBigIntCreateWithInt64(context_, value.j(), &exception); break;
        case 'D': boxed = JSValueMakeNumber(context_, value.d()); break;
        case 'F': boxed = JSValueMakeNumber(context_, value.f()); break;
        default: boxed = JSValueMakeNumber(context_, value.i()); break;
      }
      Check(exception);
      JSValueProtect(context_, boxed);
      args.refs.push_back(boxed);
    }
    auto result = JSObjectCallAsFunction(context_, export_, nullptr, args.refs.size(), args.refs.data(), &exception);
    Check(exception);
    if (types[0] == 'V') return {};
    if (types[0] == 'J') {
      const auto integer = JSValueToInt64(context_, result, &exception);
      Check(exception);
      return Value::Long(integer);
    }
    const auto number = JSValueToNumber(context_, result, &exception);
    Check(exception);
    switch (types[0]) {
      case 'D': return Value::Double(number);
      case 'F': return Value::Float(number);
      default: return Value::Int(number);
    }
  }

 private:
  void Check(JSValueRef exception) const {
    if (!exception) return;
    auto str = JSValueToStringCopy(context_, exception, nullptr);
    std::string message(JSStringGetMaximumUTF8CStringSize(str), '\0');
    message.resize(JSStringGetUTF8CString(str, message.data(), message.size()) - 1);
    JSStringRelease(str);
    throw std::runtime_error(message);
  }

  JSGlobalContextRef context_;
  JSObjectRef export_;
};

}  // namespace
#endif

InstallWasm WasmExecutor() {
#if defined(WA_TEST_HGRAPH) && defined(__EMSCRIPTEN__)
  return [](std::span<const uint8_t> module, std::string_view name, std::string_view shorty) {
    const auto fn = std::make_shared<Installed>(Install(module.data(), module.size(), std::string(name).c_str()));
    return [fn, types = std::string(shorty)](std::span<const Value> values) {
      std::vector<uint64_t> words;
      words.reserve(values.size());
      for (Value value : values) words.push_back(value.bits());
      uint64_t result = 0;
      Invoke(fn->id, types.c_str(), words.data(), &result);
      return Value::Bits(result);
    };
  };
#elif defined(WA_TEST_HGRAPH) && defined(__APPLE__)
  return [](std::span<const uint8_t> module, std::string_view name, std::string_view shorty) {
    auto function = std::make_shared<Installed>(module, name);
    return [function, types = std::string(shorty)](std::span<const Value> values) {
      return function->Call(types, values);
    };
  };
#else
  return {};
#endif
}

}  // namespace wa::interp::test
