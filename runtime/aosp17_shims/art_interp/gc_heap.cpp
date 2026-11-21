#include "jni_bridge.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace wa::interp {

RootScope::RootScope(Runtime& rt, std::span<const Value> borrowed, Object* receiver)
    : runtime(rt), previous(rt.roots), borrowed(borrowed), receiver(receiver) {
  runtime.roots = this;
}

RootScope::~RootScope() {
  assert(runtime.roots == this);
  runtime.roots = previous;
}

Object* GcHeap::Allocate(Runtime& runtime, Class& cls) {
  if (!runtime.gc_paused && runtime.options.gc_interval != 0 &&
      since_collection_ >= std::max(runtime.options.gc_interval, next_collection_)) {
    Collect(runtime);
  }
  if (!free_) {
    slabs_.push_back(std::make_unique<Object[]>(kSlabSize));
    Object* slab = slabs_.back().get();
    for (size_t i = kSlabSize; i-- != 0;) {
      slab[i].next_free = free_;
      free_ = &slab[i];
    }
  }
  Object* object = free_;
  object->fields.resize(cls.instance_slots);
  free_ = object->next_free;
  object->next_free = nullptr;
  object->type = &cls;
  ++since_collection_;
  ++stats_.live;
  return object;
}

GcStats GcHeap::Collect(Runtime& runtime) {
  worklist_.reserve(stats_.live);
  const auto mark = [&](Object* object) {
    if (!object) return;
    if (!object->type) throw std::logic_error("Dangling managed reference during GC");
    if (object->marked) return;
    object->marked = true;
    worklist_.push_back(object);
  };

  for (const RootScope* scope = runtime.roots; scope; scope = scope->previous) {
    mark(scope->receiver);
    for (Value value : scope->borrowed) mark(value.ref());
    for (Value value : scope->values) mark(value.ref());
  }
  for (Object* string : runtime.dex_literals) mark(string);
  runtime.linker.VisitRoots(mark);
  runtime.jni->VisitRoots(mark);
  mark(runtime.exception);
  for (size_t i = 0; i < worklist_.size(); ++i) {
    const Object* object = worklist_[i];
    for (Value field : object->fields) mark(field.ref());
    for (Value element : object->elements) mark(element.ref());
    mark(object->payload.ref());
    if (object->table) {
      for (Object* entry : object->table->order) mark(entry);
    }
  }

  for (Object* object : worklist_) {
    if (object->weak_referent && !object->weak_referent->marked) object->weak_referent = nullptr;
  }
  std::erase_if(runtime.interned, [](const auto& entry) { return !entry.second->marked; });
  for (const auto& slab : slabs_) {
    for (size_t i = 0; i < kSlabSize; ++i) {
      Object& object = slab[i];
      if (!object.type) continue;
      if (object.marked) {
        object.marked = false;
        continue;
      }
      object = Object{};
      object.next_free = free_;
      free_ = &object;
      --stats_.live;
      ++stats_.reclaimed;
    }
  }
  worklist_.clear();
  ++stats_.collections;
  since_collection_ = 0;
  // Growth of the tenured live set should not force a full scan for every nursery-sized batch.
  next_collection_ = stats_.live / 2;
  return stats_;
}

}  // namespace wa::interp
