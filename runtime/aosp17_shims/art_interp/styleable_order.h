#pragma once

#include <algorithm>
#include <numeric>
#include <vector>

namespace android {

// AOSP's attribute finders advance within each package. The compatibility
// widgets pass arrays in accessor order, so translate only unsorted requests.
static void ApplyStyleInAccessorOrder(JNIEnv* env, jclass, jlong ptr, jlong theme_ptr,
                                      jint def_style_attr, jint def_style_resid,
                                      jlong xml_parser_ptr, jintArray java_attrs,
                                      jintArray java_values, jintArray java_indices) {
  ScopedIntCriticalArrayRO attrs(env, java_attrs);
  if (!attrs.get()) return;
  const auto* ids = reinterpret_cast<const uint32_t*>(attrs.get());
  const size_t count = attrs.size();
  ScopedIntCriticalArrayRW out_values(env, java_values);
  ScopedIntCriticalArrayRW out_indices(env, java_indices);
  if (!out_values.get() || !out_indices.get()) return;
  auto* out = reinterpret_cast<uint32_t*>(out_values.get());
  auto* xml = reinterpret_cast<ResXMLParser*>(xml_parser_ptr);
  auto asset_manager = LockAndStartAssetManager(ptr);
  auto* theme = reinterpret_cast<Theme*>(theme_ptr);
  CHECK(theme->GetAssetManager() == &(*asset_manager));
  if (std::is_sorted(ids, ids + count)) {
    ApplyStyle(theme, xml, def_style_attr, def_style_resid, ids, count, out,
               reinterpret_cast<uint32_t*>(out_indices.get()));
    return;
  }

  std::vector<size_t> order(count);
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, {}, [ids](size_t i) { return ids[i]; });
  std::vector<uint32_t> sorted(count), values(count * STYLE_NUM_ENTRIES), indices(count + 1);
  for (size_t i = 0; i < count; ++i) sorted[i] = ids[order[i]];

  if (!ApplyStyle(theme, xml, def_style_attr, def_style_resid, sorted.data(), count,
                  values.data(), indices.data()).has_value()) {
    return;
  }

  for (size_t i = 0; i < count; ++i)
    std::copy_n(values.data() + i * STYLE_NUM_ENTRIES, STYLE_NUM_ENTRIES,
                out + order[i] * STYLE_NUM_ENTRIES);
  for (size_t i = 1; i <= indices[0]; ++i) indices[i] = order[indices[i]];
  std::sort(indices.begin() + 1, indices.begin() + indices[0] + 1);
  std::copy_n(indices.begin(), indices[0] + 1, out_indices.get());
}

}  // namespace android
