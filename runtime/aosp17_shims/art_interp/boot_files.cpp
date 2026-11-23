#include "boot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>

namespace wa::interp {
namespace {

std::u16string NormalizePath(std::u16string_view path) {
  std::u16string result;
  for (char16_t ch : path) {
    if (ch != u'/' || result.empty() || result.back() != u'/') result += ch;
  }
  if (result.size() > 1 && result.back() == u'/') result.pop_back();
  return result;
}

std::string GuestPath(std::u16string_view text) {
  if (text.empty() || text.contains(u'\0')) return {};
  auto path = (std::filesystem::path("/") / EncodeUtf8(text, false)).lexically_normal().string();
  if (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

std::string_view Parent(std::string_view path) {
  return path.substr(0, std::max<size_t>(1, path.rfind('/')));
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

bool MakeDirectory(Runtime& vm, std::string_view path, bool parents) {
  if (path.empty() || vm.guest_files.contains(path)) return false;
  std::vector<std::string_view> missing{path};
  for (auto parent = Parent(path);; parent = Parent(parent)) {
    const auto entry = vm.guest_files.find(parent);
    if (entry != vm.guest_files.end()) {
      if (entry->second) return false;
      break;
    }
    if (!parents) return false;
    missing.push_back(parent);
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it) vm.guest_files.emplace(*it, nullptr);
  return true;
}

std::filesystem::file_type FileType(Runtime& vm, Object* self) {
  using Type = std::filesystem::file_type;
  const auto path = GuestPath(self->text);
  if (path.empty()) return Type::not_found;
  const auto entry = vm.guest_files.find(path);
  if (entry != vm.guest_files.end()) return entry->second ? Type::regular : Type::directory;
  if (vm.options.sandboxed_files || path.starts_with("/data/") || path.starts_with("/storage/")) {
    return Type::not_found;
  }
  std::error_code error;
  const auto status = std::filesystem::status(EncodeUtf8(self->text, false), error);
  return error ? Type::not_found : status.type();
}

Object& Descriptor(Runtime& vm, Object* stream) {
  const auto& field = vm.linker.FindField(*stream->type, "fd", "Ljava/io/FileDescriptor;", false);
  return vm.NonNull(stream->fields[field.slot].ref());
}

Value Open(Runtime& vm, Object* name_object, bool readable, bool writable, bool append, bool truncate) {
  constexpr auto kMissing = "Ljava/io/FileNotFoundException;";
  const auto& name = vm.NonNull(name_object).text;
  const auto path = GuestPath(name);
  auto entry = vm.guest_files.find(path);
  if (path.empty() || (entry != vm.guest_files.end() && !entry->second) ||
      (writable && vm.read_only_paths.contains(path))) {
    vm.Fail(kMissing, EncodeUtf8(name, false));
  }
  if (entry == vm.guest_files.end()) {
    const auto parent = vm.guest_files.find(Parent(path));
    if (!writable || parent == vm.guest_files.end() || parent->second) {
      vm.Fail(kMissing, EncodeUtf8(name, false));
    }
  }
  if (vm.next_file_descriptor == INT32_MAX) vm.Fail(kMissing, "File descriptors exhausted");
  auto& type = vm.linker.Resolve("Ljava/io/FileDescriptor;");
  auto& ctor = vm.linker.FindMethod(type, "<init>", "(I)V", false);
  const Value number = Value::Int(vm.next_file_descriptor++);
  auto* descriptor = vm.NewObject(type, ctor, {&number, 1});
  if (entry == vm.guest_files.end()) {
    entry = vm.guest_files.emplace(path,
        std::make_shared<Runtime::GuestFile>(Runtime::GuestFile{{}, NowMillis()})).first;
  }
  vm.file_handles.emplace(descriptor, Runtime::FileHandle{entry->second, 0, readable, writable, append});
  if (truncate) {
    entry->second->bytes.clear();
    entry->second->last_modified = NowMillis();
  }
  return Value::Ref(descriptor);
}

Runtime::FileHandle& Handle(Runtime& vm, Object& descriptor, bool writable) {
  const auto entry = vm.file_handles.find(&descriptor);
  if (entry == vm.file_handles.end() || !(writable ? entry->second.writable : entry->second.readable)) {
    vm.Fail("Ljava/io/IOException;", "Bad file descriptor");
  }
  return entry->second;
}

std::span<Value> Buffer(Runtime& vm, Values args) {
  auto& bytes = vm.NonNull(args[0].ref()).elements;
  const int32_t offset = args[1].i(), length = args[2].i();
  if (offset < 0 || length < 0 || uint64_t(offset) + length > bytes.size()) {
    vm.Fail("Ljava/lang/IndexOutOfBoundsException;");
  }
  return std::span(bytes).subspan(offset, length);
}

void BindFileStreams(ClassLinker& cl, const Field& android_fd, const Field& host_fd) {
  constexpr auto kIn = "Ljava/io/FileInputStream;";
  constexpr auto kOut = "Ljava/io/FileOutputStream;";
  constexpr auto kRandom = "Ljava/io/RandomAccessFile;";
  const Builtin close_stream = [&android_fd, &host_fd](Runtime& vm, Object* self, Values) {
    auto& descriptor = Descriptor(vm, self);
    vm.file_handles.erase(&descriptor);
    descriptor.fields[android_fd.slot] = descriptor.fields[host_fd.slot] = Value::Int(-1);
    return Value{};
  };
  cl.AddBootNative(kIn, "open", "(Ljava/lang/String;)Ljava/io/FileDescriptor;",
      [](Runtime& vm, Object*, Values args) { return Open(vm, args[0].ref(), true, false, false, false); });
  cl.AddBootNative(kOut, "open", "(Ljava/lang/String;Z)Ljava/io/FileDescriptor;",
      [](Runtime& vm, Object*, Values args) {
        const bool append = args[1].i();
        return Open(vm, args[0].ref(), false, true, append, !append);
      });
  cl.AddBootNative(kRandom, "open", "(Ljava/lang/String;Z)Ljava/io/FileDescriptor;",
      [](Runtime& vm, Object*, Values args) {
        return Open(vm, args[0].ref(), true, args[1].i(), false, false);
      });
  cl.AddBootNative(kIn, "close0", "()V", close_stream);
  cl.AddBootNative(kOut, "close0", "()V", close_stream);
  cl.AddBootNative(kRandom, "close0", "()V", close_stream);
  const Builtin read = [&android_fd](Runtime& vm, Object* self, Values args) {
    auto buffer = Buffer(vm, args);
    if (buffer.empty()) return Value::Int(0);
    auto& descriptor = Descriptor(vm, self);
    if (descriptor.fields[android_fd.slot].i() == 0) return Value::Int(-1);
    auto& handle = Handle(vm, descriptor, false);
    const auto& bytes = handle.file->bytes;
    if (handle.position >= bytes.size()) return Value::Int(-1);
    const auto count = std::min<uint64_t>(buffer.size(), bytes.size() - handle.position);
    for (size_t i = 0; i < count; ++i) {
      buffer[i] = Value::Int(static_cast<int8_t>(bytes[handle.position + i]));
    }
    handle.position += count;
    return Value::Int(count);
  };
  cl.AddBootNative(kIn, "readBytes", "([BII)I", read);
  cl.AddBootNative(kRandom, "readBytes", "([BII)I", read);
  const Builtin length = [&android_fd](Runtime& vm, Object* self, Values) {
    auto& descriptor = Descriptor(vm, self);
    return Value::Long(descriptor.fields[android_fd.slot].i() == 0 ? 0 : Handle(vm, descriptor, false).file->bytes.size());
  };
  cl.AddBootNative(kIn, "length0", "()J", length);
  cl.AddBootNative(kRandom, "length", "()J", length);
  const Builtin position = [&android_fd](Runtime& vm, Object* self, Values) {
    auto& descriptor = Descriptor(vm, self);
    return Value::Long(descriptor.fields[android_fd.slot].i() == 0 ? 0 : Handle(vm, descriptor, false).position);
  };
  cl.AddBootNative(kIn, "position0", "()J", position);
  cl.AddBootNative(kRandom, "getFilePointer", "()J", position);
  cl.AddBootNative(kRandom, "seek0", "(J)V", [](Runtime& vm, Object* self, Values args) {
    if (args[0].j() < 0) vm.Fail("Ljava/io/IOException;", "Negative seek offset");
    Handle(vm, Descriptor(vm, self), false).position = args[0].j();
    return Value{};
  });
  cl.AddBootNative(kRandom, "setLength0", "(J)V", [](Runtime& vm, Object* self, Values args) {
    auto& handle = Handle(vm, Descriptor(vm, self), true);
    const auto size = static_cast<uint64_t>(args[0].j());
    if (size > handle.file->bytes.max_size()) vm.Fail("Ljava/io/IOException;", "Invalid file length");
    handle.file->bytes.resize(size);
    handle.file->last_modified = NowMillis();
    handle.position = std::min(size, handle.position);
    return Value{};
  });
  cl.AddBootNative(kIn, "available0", "()I", [&android_fd](Runtime& vm, Object* self, Values) {
    auto& descriptor = Descriptor(vm, self);
    if (descriptor.fields[android_fd.slot].i() == 0) return Value::Int(0);
    const auto& handle = Handle(vm, descriptor, false);
    const auto size = handle.file->bytes.size();
    const auto remaining = size - std::min<uint64_t>(size, handle.position);
    return Value::Int(std::min<uint64_t>(INT32_MAX, remaining));
  });
  cl.AddBootNative(kIn, "skip0", "(J)J", [&android_fd](Runtime& vm, Object* self, Values args) {
    auto& descriptor = Descriptor(vm, self);
    if (descriptor.fields[android_fd.slot].i() == 0) return Value::Long(0);
    auto& handle = Handle(vm, descriptor, false);
    const auto position = static_cast<int64_t>(handle.position), amount = args[0].j();
    if (amount < -position || amount > INT64_MAX - position) {
      vm.Fail("Ljava/io/IOException;", "Invalid file position");
    }
    handle.position = position + amount;
    return Value::Long(amount);
  });
  const Builtin write = [&android_fd](Runtime& vm, Object* self, Values args) {
    const auto buffer = Buffer(vm, args);
    if (buffer.empty()) return Value{};
    auto& descriptor = Descriptor(vm, self);
    const auto number = descriptor.fields[android_fd.slot].i();
    if (number == 1 || number == 2) {
      auto* stream = number == 2 ? stderr : stdout;
      for (const auto byte : buffer) {
        if (std::fputc(static_cast<uint8_t>(byte.i()), stream) == EOF) {
          vm.Fail("Ljava/io/IOException;", "Console write failed");
        }
      }
      if (std::fflush(stream) != 0) vm.Fail("Ljava/io/IOException;", "Console flush failed");
      return Value{};
    }
    auto& handle = Handle(vm, descriptor, true);
    auto& bytes = handle.file->bytes;
    const auto position = handle.append ? bytes.size() : handle.position;
    if (position > bytes.max_size() || buffer.size() > bytes.max_size() - position) {
      vm.Fail("Ljava/io/IOException;", "File is too large");
    }
    const auto end = position + buffer.size();
    if (end > bytes.size()) bytes.resize(end);
    for (size_t i = 0; i < buffer.size(); ++i) bytes[position + i] = static_cast<uint8_t>(buffer[i].i());
    handle.position = end;
    handle.file->last_modified = NowMillis();
    return Value{};
  };
  cl.AddBootNative(kOut, "writeBytes", "([BII)V", write);
  cl.AddBootNative(kRandom, "writeBytes", "([BII)V", write);
}

}  // namespace

void Vm::MountReadOnlyFile(std::string_view name, std::span<const uint8_t> bytes) {
  auto& vm = *runtime_;
  const auto path = GuestPath(DecodeUtf8(name));
  if (path.empty() || vm.guest_files.contains(path)) throw std::invalid_argument("Invalid guest file mount");
  const auto parent = Parent(path);
  if (!vm.guest_files.contains(parent)) MakeDirectory(vm, parent, true);
  const auto directory = vm.guest_files.find(parent);
  if (directory == vm.guest_files.end() || directory->second) {
    throw std::invalid_argument("Invalid guest file mount parent");
  }
  auto file = std::make_shared<Runtime::GuestFile>(Runtime::GuestFile{
      std::vector<uint8_t>(bytes.begin(), bytes.end()), NowMillis()});
  const auto permission = vm.read_only_paths.insert(path).first;
  try {
    vm.guest_files.emplace(path, std::move(file));
  } catch (...) {
    vm.read_only_paths.erase(permission);
    throw;
  }
}

void InstallFiles(Runtime& vm) {
  auto& cl = vm.linker;
  auto& file = cl.Define("Ljava/io/File;", kObject, 0, {"Ljava/io/Serializable;"});
  auto& separator = cl.AddField(file, "separator", kString, true);
  auto& path_separator = cl.AddField(file, "pathSeparator", kString, true);
  cl.AddField(file, "separatorChar", "C", true, Value::Int('/'));
  cl.AddField(file, "pathSeparatorChar", "C", true, Value::Int(':'));
  cl.AddMethod(file, "<clinit>", "()V", art::kAccStatic,
      [&separator, &path_separator](Runtime& vm, Object*, Values) {
        separator.value = StringValue(vm, "/");
        path_separator.value = StringValue(vm, ":");
        return Value{};
      });
  cl.AddMethod(file, "<init>", "(Ljava/lang/String;)V", 0,
      [](Runtime& vm, Object* self, Values a) {
        self->text = NormalizePath(vm.NonNull(a[0].ref()).text);
        return Value{};
      });
  const Builtin child = [](Runtime& vm, Object* self, Values a) {
    const auto& leaf = vm.NonNull(a[1].ref()).text;
    if (!a[0].ref() || a[0].ref()->text.empty()) self->text = NormalizePath(leaf);
    else self->text = NormalizePath(a[0].ref()->text + u'/' + leaf);
    return Value{};
  };
  cl.AddMethod(file, "<init>", "(Ljava/io/File;Ljava/lang/String;)V", 0, child);
  cl.AddMethod(file, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", 0, child);
  const Builtin path = [](Runtime& vm, Object* self, Values) { return StringValue(vm, self->text); };
  cl.AddMethod(file, "getPath", "()Ljava/lang/String;", 0, path);
  cl.AddMethod(file, "toString", "()Ljava/lang/String;", 0, path);
  cl.AddMethod(file, "getAbsolutePath", "()Ljava/lang/String;", 0, [](Runtime& vm, Object* self, Values) {
    if (self->text.starts_with(u'/')) return StringValue(vm, self->text);
    if (vm.options.sandboxed_files) return StringValue(vm, u"/" + self->text);
    std::error_code error;
    auto absolute = std::filesystem::current_path(error);
    if (error) vm.Fail("Ljava/io/IOException;", error.message());
    if (!self->text.empty()) absolute /= EncodeUtf8(self->text, false);
    return StringValue(vm, absolute.string());
  });
  cl.AddMethod(file, "getName", "()Ljava/lang/String;", 0, [](Runtime& vm, Object* self, Values) {
    const auto index = self->text.rfind(u'/');
    return StringValue(vm, index == self->text.npos ? self->text : self->text.substr(index + 1));
  });
  cl.AddMethod(file, "isAbsolute", "()Z", 0, [](Runtime&, Object* self, Values) {
    return Value::Int(self->text.starts_with(u'/'));
  });
  cl.AddMethod(file, "isInvalid", "()Z", 0, [](Runtime&, Object* self, Values) {
    return Value::Int(self->text.contains(u'\0'));
  });
  const auto parent_path = [](Object* self) -> std::u16string_view {
    const auto index = self->text.rfind(u'/');
    if (index == self->text.npos || self->text == u"/") return {};
    return std::u16string_view(self->text).substr(0, std::max<size_t>(1, index));
  };
  cl.AddMethod(file, "getParent", "()Ljava/lang/String;", 0,
      [parent_path](Runtime& vm, Object* self, Values) {
        const auto parent = parent_path(self);
        return parent.empty() ? Value{} : StringValue(vm, std::u16string(parent));
      });
  cl.AddMethod(file, "getParentFile", "()Ljava/io/File;", 0,
      [&file, parent_path](Runtime& vm, Object* self, Values) {
        const auto parent = parent_path(self);
        if (parent.empty()) return Value{};
        auto* result = vm.heap.Allocate(vm, file);
        result->text = parent;
        return Value::Ref(result);
      });
  using Type = std::filesystem::file_type;
  cl.AddMethod(file, "isDirectory", "()Z", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Int(FileType(vm, self) == Type::directory);
  });
  cl.AddMethod(file, "isFile", "()Z", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Int(FileType(vm, self) == Type::regular);
  });
  cl.AddMethod(file, "exists", "()Z", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Int(FileType(vm, self) != Type::not_found);
  });
  cl.AddMethod(file, "mkdir", "()Z", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Int(MakeDirectory(vm, GuestPath(self->text), false));
  });
  cl.AddMethod(file, "mkdirs", "()Z", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Int(MakeDirectory(vm, GuestPath(self->text), true));
  });
  cl.AddMethod(file, "length", "()J", 0, [](Runtime& vm, Object* self, Values) {
    const auto entry = vm.guest_files.find(GuestPath(self->text));
    return Value::Long(entry != vm.guest_files.end() && entry->second ? entry->second->bytes.size() : 0);
  });
  cl.AddMethod(file, "lastModified", "()J", 0, [](Runtime& vm, Object* self, Values) {
    const auto entry = vm.guest_files.find(GuestPath(self->text));
    return Value::Long(entry != vm.guest_files.end() && entry->second ? entry->second->last_modified : 0);
  });
  cl.AddMethod(file, "delete", "()Z", 0, [](Runtime& vm, Object* self, Values) {
    const auto path = GuestPath(self->text);
    const auto entry = vm.guest_files.find(path);
    if (entry == vm.guest_files.end() || path == "/" || vm.read_only_paths.contains(path)) return Value::Int(0);
    if (!entry->second) {
      const auto prefix = path + '/';
      const auto child = vm.guest_files.lower_bound(prefix);
      if (child != vm.guest_files.end() && child->first.starts_with(prefix)) return Value::Int(0);
    }
    vm.guest_files.erase(entry);
    return Value::Int(1);
  });
  cl.AddMethod(file, "renameTo", "(Ljava/io/File;)Z", 0, [](Runtime& vm, Object* self, Values args) {
    const auto source = vm.guest_files.find(GuestPath(self->text));
    const auto target = GuestPath(vm.NonNull(args[0].ref()).text);
    if (source == vm.guest_files.end() || !source->second || target.empty() ||
        vm.read_only_paths.contains(source->first) || vm.read_only_paths.contains(target)) return Value::Int(0);
    if (source->first == target) return Value::Int(1);
    const auto parent = vm.guest_files.find(Parent(target));
    const auto existing = vm.guest_files.find(target);
    if (parent == vm.guest_files.end() || parent->second ||
        (existing != vm.guest_files.end() && !existing->second)) return Value::Int(0);
    vm.guest_files.insert_or_assign(target, source->second);
    vm.guest_files.erase(source);
    return Value::Int(1);
  });
  cl.AddMethod(file, "listFiles", "()[Ljava/io/File;", 0, [](Runtime& vm, Object* self, Values) {
    std::vector<std::string> paths;
    const auto path = GuestPath(self->text);
    const auto entry = vm.guest_files.find(path);
    if (entry != vm.guest_files.end() && (vm.options.sandboxed_files || path != "/")) {
      if (entry->second) return Value{};
      const auto prefix = path == "/" ? path : path + '/';
      for (auto it = vm.guest_files.lower_bound(prefix);
           it != vm.guest_files.end() && it->first.starts_with(prefix); ++it) {
        const auto child = std::string_view(it->first).substr(prefix.size());
        if (!child.empty() && !child.contains('/')) paths.push_back(it->first);
      }
    } else {
      if (vm.options.sandboxed_files) return Value{};
      std::error_code error;
      const std::filesystem::directory_iterator end;
      for (auto it = std::filesystem::directory_iterator(EncodeUtf8(self->text, false), error);
           !error && it != end; it.increment(error)) {
        paths.push_back(it->path().string());
      }
      if (error) return Value{};
    }
    RootScope roots(vm);
    auto* array = roots.Hold(vm.NewArray(vm.linker.Resolve("[Ljava/io/File;"), paths.size()));
    Class& file = vm.linker.Resolve("Ljava/io/File;");
    for (size_t i = 0; i < paths.size(); ++i) {
      auto* child = vm.heap.Allocate(vm, file);
      child->text = DecodeUtf8(paths[i]);
      array->elements[i] = Value::Ref(child);
    }
    return Value::Ref(array);
  });

  auto& fd = cl.Define("Ljava/io/FileDescriptor;", kObject, art::kAccFinal);
  Field& android_fd = cl.AddField(fd, "descriptor", "I", false);
  Field& host_fd = cl.AddField(fd, "fd", "I", false);
  cl.AddField(fd, "handle", "J", false);
  const Builtin set_fd = [&android_fd, &host_fd](Runtime&, Object* self, Values a) {
    const Value descriptor = a.empty() ? Value::Int(-1) : a[0];
    self->fields[android_fd.slot] = self->fields[host_fd.slot] = descriptor;
    return Value{};
  };
  cl.AddMethod(fd, "<init>", "()V", 0, set_fd);
  cl.AddMethod(fd, "<init>", "(I)V", 0, set_fd);
  cl.AddMethod(fd, "setInt$", "(I)V", 0, set_fd);
  cl.AddMethod(fd, "getInt$", "()I", 0,
      [&android_fd](Runtime&, Object* self, Values) { return self->fields[android_fd.slot]; });
  cl.AddMethod(fd, "valid", "()Z", 0, [&android_fd](Runtime&, Object* self, Values) {
    return Value::Int(self->fields[android_fd.slot].i() >= 0);
  });
  cl.AddMethod(fd, "sync", "()V", 0, [&android_fd](Runtime& vm, Object* self, Values) {
    const auto number = self->fields[android_fd.slot].i();
    if (!vm.file_handles.contains(self) &&
        ((number != 1 && number != 2) || std::fflush(number == 1 ? stdout : stderr) != 0)) {
      vm.Fail("Ljava/io/SyncFailedException;");
    }
    return Value{};
  });
  const std::array standard{&cl.AddField(fd, "in", fd.descriptor, true),
      &cl.AddField(fd, "out", fd.descriptor, true), &cl.AddField(fd, "err", fd.descriptor, true)};
  cl.AddMethod(fd, "<clinit>", "()V", art::kAccStatic,
      [&fd, &android_fd, &host_fd, standard](Runtime& vm, Object*, Values) {
        for (size_t number = 0; number < standard.size(); ++number) {
          auto* descriptor = vm.heap.Allocate(vm, fd);
          descriptor->fields[android_fd.slot] = descriptor->fields[host_fd.slot] = Value::Int(number);
          standard[number]->value = Value::Ref(descriptor);
        }
        return Value{};
      });
  BindFileStreams(cl, android_fd, host_fd);

  Class& parcel_fd = cl.Define("Landroid/os/ParcelFileDescriptor;", kObject, 0, {"Ljava/io/Closeable;"});
  Field& wrapped = cl.AddField(parcel_fd, "descriptor", "Ljava/io/FileDescriptor;", false);
  cl.AddMethod(parcel_fd, "<init>", "(Ljava/io/FileDescriptor;)V", 0,
      [&wrapped](Runtime& vm, Object* self, Values a) {
        self->fields[wrapped.slot] = Value::Ref(&vm.NonNull(a[0].ref()));
        return Value{};
      });
  const Builtin detach = [&wrapped, &android_fd, &host_fd](Runtime& vm, Object* self, Values) {
    Object& fd = vm.NonNull(self->fields[wrapped.slot].ref());
    if (vm.file_handles.contains(&fd)) {
      vm.Fail("Ljava/lang/UnsupportedOperationException;", "Guest files have no operating-system descriptor");
    }
    Value current = fd.fields[android_fd.slot];
    fd.fields[host_fd.slot] = fd.fields[android_fd.slot] = Value::Int(-1);
    return current;
  };
  cl.AddMethod(parcel_fd, "detachFd", "()I", 0, detach);
  cl.AddMethod(parcel_fd, "close", "()V", 0, [detach, &wrapped, &android_fd, &host_fd](Runtime& vm, Object* self, Values a) {
    auto& descriptor = vm.NonNull(self->fields[wrapped.slot].ref());
    if (vm.file_handles.erase(&descriptor)) {
      descriptor.fields[android_fd.slot] = descriptor.fields[host_fd.slot] = Value::Int(-1);
      return Value{};
    }
    const jint fd = detach(vm, self, a).i();
    if (fd >= 0 && close(fd) != 0) vm.Fail("Ljava/io/IOException;", "Could not close file descriptor");
    return Value{};
  });
}

}  // namespace wa::interp
