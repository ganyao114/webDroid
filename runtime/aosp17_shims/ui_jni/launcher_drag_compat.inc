namespace {

void RegisterLauncherDragCollections(interp::Vm& vm) {
    constexpr auto descriptor = "Ljava/util/Stack;";
    if (vm.HasClass(descriptor)) return;
    auto& linker = interp::JniBridge::From(vm.JniEnv()).Vm().linker;
    const auto parent = vm.HasClass("Ljava/util/Vector;") ? "Ljava/util/Vector;" : "Ljava/util/ArrayList;";
    auto& stack = linker.Define(descriptor, parent, art::kAccPublic);
    if (!vm.HasClass("Ljava/util/EmptyStackException;"))
        linker.Define("Ljava/util/EmptyStackException;", "Ljava/lang/RuntimeException;", art::kAccPublic);
    using Args = std::span<const interp::Value>;
    const auto invoke = [](interp::Runtime& runtime, interp::Object* self, const char* name,
                           const char* signature, Args args = {}) {
        auto& method = runtime.linker.FindMethod(*self->type, name, signature, false);
        return runtime.Invoke(runtime.linker.VirtualMethod(*self->type, method), self, args);
    };
    linker.AddMethod(stack, "<init>", "()V", art::kAccPublic,
        [parent](interp::Runtime& runtime, interp::Object* self, Args) {
            auto& type = runtime.linker.Resolve(parent);
            return runtime.Invoke(runtime.linker.FindMethod(type, "<init>", "()V", false), self, {});
        });
    linker.AddMethod(stack, "push", "(Ljava/lang/Object;)Ljava/lang/Object;", art::kAccPublic,
        [invoke](interp::Runtime& runtime, interp::Object* self, Args args) {
            invoke(runtime, self, "add", "(Ljava/lang/Object;)Z", args);
            return args[0];
        });
    for (const bool remove : {false, true}) {
        linker.AddMethod(stack, remove ? "pop" : "peek", "()Ljava/lang/Object;", art::kAccPublic,
            [invoke, remove](interp::Runtime& runtime, interp::Object* self, Args) {
                const auto size = invoke(runtime, self, "size", "()I").i();
                if (size == 0) runtime.Fail("Ljava/util/EmptyStackException;");
                const std::array index{interp::Value::Int(size - 1)};
                return invoke(runtime, self, remove ? "remove" : "get", "(I)Ljava/lang/Object;", index);
            });
    }
    linker.AddMethod(stack, "empty", "()Z", art::kAccPublic,
        [invoke](interp::Runtime& runtime, interp::Object* self, Args) {
            return invoke(runtime, self, "isEmpty", "()Z");
        });
    linker.AddMethod(stack, "search", "(Ljava/lang/Object;)I", art::kAccPublic,
        [invoke](interp::Runtime& runtime, interp::Object* self, Args args) {
            const auto index = invoke(runtime, self, "lastIndexOf", "(Ljava/lang/Object;)I", args).i();
            return interp::Value::Int(index < 0 ? -1 : invoke(runtime, self, "size", "()I").i() - index);
        });
}

} // namespace
