#pragma once

#include "runtime_internal.h"

namespace wa::interp {

inline constexpr std::string_view kObject = "Ljava/lang/Object;";
inline constexpr std::string_view kString = "Ljava/lang/String;";
inline constexpr uint32_t kInterface = art::kAccInterface | art::kAccAbstract;

using Values = std::span<const Value>;

void InstallCollections(Runtime& vm);
void InstallPlatform(Runtime& vm);
Value CallVirtual(Runtime& vm, Object* obj, std::string_view name, std::string_view signature,
                  Values args = {});
bool JavaEquals(Runtime& vm, Object* left, Object* right);
std::u16string JavaText(Runtime& vm, Object* obj);
Value StringValue(Runtime& vm, std::u16string text);
Value StringValue(Runtime& vm, std::string_view text);
int32_t IdentityHash(Object* object);

}  // namespace wa::interp
