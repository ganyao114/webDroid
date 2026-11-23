namespace {

constexpr auto kRegistry = "Llibcore/util/NativeAllocationRegistry;";
constexpr auto kReleaseAction = "Lorg/wasmandroid/ui/NativeAllocationRelease;";

interp::Object* NewAllocationObject(interp::Runtime& vm, const char* name) {
    auto& type = vm.linker.Resolve(name);
    return vm.NewObject(type, vm.linker.FindMethod(type, "<init>", "()V", false), {});
}

void ReleaseAllocation(State& state, jlong function, jlong handle) {
    if (function == kFinalizeFontFamily && state.font_families) state.font_families->Erase(handle);
    if (function == kFinalizeGlyphRun && state.glyph_runs) state.glyph_runs->Erase(handle);
}

void RegisterHostAllocationRegistry(interp::Vm& vm) {
    if (vm.HasClass(kRegistry)) return;
    auto& linker = interp::JniBridge::From(vm.JniEnv()).Vm().linker;
    auto& registry = linker.Define(kRegistry, "Ljava/lang/Object;", art::kAccFinal);
    auto& release = linker.Define(kReleaseAction, "Ljava/lang/Object;", art::kAccFinal,
                                  {"Ljava/lang/Runnable;"});
    auto& function = linker.AddField(registry, "function", "J", false);
    auto& release_function = linker.AddField(release, "function", "J", false);
    auto& release_handle = linker.AddField(release, "handle", "J", false);
    const auto ctor = [](interp::Runtime&, interp::Object*, std::span<const interp::Value>) {
        return interp::Value{};
    };
    linker.AddMethod(registry, "<init>", "()V", 0, ctor);
    linker.AddMethod(release, "<init>", "()V", 0, ctor);
    const auto factory = [&function](interp::Runtime& rt, interp::Object*, std::span<const interp::Value> args) {
        auto* object = NewAllocationObject(rt, kRegistry);
        object->fields[function.slot] = args[1];
        return interp::Value::Ref(object);
    };
    for (const auto* name : {"createMalloced", "createNonmalloced"}) {
        linker.AddMethod(registry, name, "(Ljava/lang/ClassLoader;J)Llibcore/util/NativeAllocationRegistry;",
                         art::kAccStatic, factory);
        linker.AddMethod(registry, name, "(Ljava/lang/ClassLoader;JJ)Llibcore/util/NativeAllocationRegistry;",
                         art::kAccStatic, factory);
    }
    linker.AddMethod(registry, "registerNativeAllocation", "(Ljava/lang/Object;J)Ljava/lang/Runnable;", 0,
        [&function, &release_function, &release_handle](interp::Runtime& rt, interp::Object* object,
                                                       std::span<const interp::Value> args) {
            auto& jni = *rt.jni;
            auto* env = jni.Env();
            auto* state = GetState(env);
            if (!state) return interp::Value{};
            const jlong func = object->fields[function.slot].j(), handle = args[1].j();
            const auto owner = jni.Local(args[0].ref());
            if (func == kFinalizeFontFamily && state->font_families) state->font_families->Retain(env, owner, handle);
            if (func == kFinalizeGlyphRun && state->glyph_runs) state->glyph_runs->Retain(env, owner, handle);
            jni.DeleteLocal(owner);
            auto* action = NewAllocationObject(rt, kReleaseAction);
            action->fields[release_function.slot] = interp::Value::Long(func);
            action->fields[release_handle.slot] = interp::Value::Long(handle);
            return interp::Value::Ref(action);
        });
    linker.AddMethod(release, "run", "()V", art::kAccPublic,
        [&release_function, &release_handle](interp::Runtime& rt, interp::Object* object,
                                             std::span<const interp::Value>) {
            const auto handle = std::exchange(object->fields[release_handle.slot], {}).j();
            if (handle)
                if (auto* state = GetState(rt.jni->Env()))
                    ReleaseAllocation(*state, object->fields[release_function.slot].j(), handle);
            return interp::Value{};
        });
}

} // namespace
