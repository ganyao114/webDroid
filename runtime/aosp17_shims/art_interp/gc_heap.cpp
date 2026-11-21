#include "jni_bridge.h"

#include <cassert>
#include <stdexcept>

namespace wa::interp {

RootScope::RootScope(Runtime& rt, size_t slots) : runtime(rt), previous(rt.roots), values(slots) {
  runtime.roots = this;
}

RootScope::~RootScope() {
  assert(runtime.roots == this);
  runtime.roots = previous;
}

Object* GcHeap::Allocate(Runtime& runtime, Class& cls) {
  if (runtime.options.gc_interval != 0 && since_collection_ >= runtime.options.gc_interval) {
    Collect(runtime);
  }
  auto object = std::make_unique<Object>();
  object->type = &cls;
  object->fields.resize(cls.instance_slots);
  Object* address = object.get();
  objects_.emplace(address, std::move(object));
  ++since_collection_;
  stats_.live = objects_.size();
  return address;
}

GcStats GcHeap::Collect(Runtime& runtime) {
  std::vector<Object*> worklist;
  worklist.reserve(objects_.size());
  for (const auto& [address, object] : objects_) object->marked = false;
  const auto mark = [&](Object* object) {
    if (!object) return;
    if (!objects_.contains(object)) throw std::logic_error("Dangling managed reference during GC");
    if (object->marked) return;
    object->marked = true;
    worklist.push_back(object);
  };

  for (const RootScope* scope = runtime.roots; scope; scope = scope->previous) {
    for (Value value : scope->values) mark(value.ref());
  }
  runtime.linker.VisitRoots(mark);
  runtime.jni->VisitRoots(mark);
  mark(runtime.exception);
  while (!worklist.empty()) {
    const Object* object = worklist.back();
    worklist.pop_back();
    for (Value field : object->fields) mark(field.ref());
    for (Value element : object->elements) mark(element.ref());
    mark(object->payload.ref());
  }

  std::erase_if(runtime.interned, [](const auto& entry) { return !entry.second->marked; });
  stats_.reclaimed += std::erase_if(objects_, [](const auto& entry) { return !entry.second->marked; });
  stats_.live = objects_.size();
  ++stats_.collections;
  since_collection_ = 0;
  return stats_;
}

}  // namespace wa::interp
