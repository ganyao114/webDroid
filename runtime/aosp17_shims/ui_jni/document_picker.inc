struct DocumentRequest {
    struct File {
        std::string path;
        std::string type;
    };
    uint32_t id;
    jint request_code;
    jobject activity = nullptr;
    std::vector<std::string> types;
    std::vector<File> files;
    bool multiple, ready = false;
};

namespace {

constexpr auto kDocuments = "/__wa_documents/";

void ClearDocumentRequest(State& state) {
    if (state.document) {
        state.env->DeleteGlobalRef(state.document->activity);
        state.document.reset();
    }
}

jboolean OpenDocument(JNIEnv* env, jobject activity, jint code, jstring type, jobjectArray types,
                      jboolean multiple) {
    return CallSafely(env, [&]() -> jboolean {
        auto* state = GetState(env);
        if (!state || !state->sf || state->document) return JNI_FALSE;
        auto request = std::make_unique<DocumentRequest>();
        request->id = ++state->document_id;
        request->request_code = code;
        request->multiple = multiple;
        const auto count = types ? std::min<jsize>(env->GetArrayLength(types), 32) : 0;
        for (jsize index = 0; index < count; ++index) {
            const auto item = static_cast<jstring>(env->GetObjectArrayElement(types, index));
            if (item) request->types.push_back(Utf8(env, item));
            env->DeleteLocalRef(item);
        }
        if (request->types.empty() && type) request->types.push_back(Utf8(env, type));
        if (env->ExceptionCheck()) return JNI_FALSE;
        request->activity = env->NewGlobalRef(activity);
        if (!request->activity) return JNI_FALSE;
        state->document = std::move(request);
        return JNI_TRUE;
    });
}

State* DocumentState(wa_sf_t* scene) {
    if (!scene) return nullptr;
    for (const auto& [env, state] : bindings)
        if (state->sf == scene && state->document && !state->document->ready) return state;
    return nullptr;
}

void DispatchDocument(State& state) {
    if (!state.document || !state.document->ready) return;
    auto* env = state.env;
    LocalFrame locals(env);
    const auto activity = env->NewLocalRef(state.document->activity);
    env->DeleteGlobalRef(state.document->activity);
    const auto request = std::move(state.document);
    if (!activity) return;
    const auto type = env->GetObjectClass(activity);
    const auto result = env->GetMethodID(type, "embeddedDocumentResult",
        "(I[Ljava/lang/String;Ljava/lang/String;)V");
    if (!result || env->ExceptionCheck()) throw interp::ManagedException{};
    jobjectArray paths = nullptr;
    jstring mime = nullptr;
    if (!request->files.empty()) {
        const auto element = env->FindClass("java/lang/String");
        if (!element) throw interp::ManagedException{};
        paths = env->NewObjectArray(request->files.size(), element, nullptr);
        if (!paths) throw interp::ManagedException{};
        for (size_t i = 0; i < request->files.size(); ++i) {
            const auto path = interp::JniBridge::From(env).Local(state.vm->NewString(request->files[i].path));
            env->SetObjectArrayElement(paths, i, path);
            env->DeleteLocalRef(path);
        }
        const auto& first = request->files.front().type;
        if (!first.empty() && std::ranges::all_of(request->files, [&](const auto& file) { return file.type == first; }))
            mime = env->NewStringUTF(first.c_str());
    }
    if (env->ExceptionCheck()) throw interp::ManagedException{};
    env->CallVoidMethod(activity, result, request->request_code, paths, mime);
    if (env->ExceptionCheck()) throw interp::ManagedException{};
}

} // namespace

extern "C" {
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int32_t wa_document_picker_request(wa_sf_t* scene, char* output, size_t capacity) {
    const auto* state = DocumentState(scene);
    if (!state) return 0;
    if (!output || capacity == 0) return WA_ERR_INVALID;
    try {
        const auto& request = *state->document;
        auto message = std::to_string(request.id) + (request.multiple ? "\n1" : "\n0");
        for (const auto& type : request.types) {
            if (type.size() <= 127 && type.find_first_of("\n\r") == std::string::npos) message += '\n' + type;
        }
        if (message.size() >= capacity) return WA_ERR_NOMEM;
        std::memcpy(output, message.data(), message.size());
        return static_cast<int32_t>(message.size());
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int32_t wa_document_picker_stage(wa_sf_t* scene, uint32_t id, const char* path, const char* type) {
    auto* state = DocumentState(scene);
    if (!state || !path || !type || id != state->document->id) return WA_ERR_INVALID;
    auto& request = *state->document;
    if (request.files.size() >= (request.multiple ? 64u : 1u)) return WA_ERR_INVALID;
    try {
        const auto prefix = kDocuments + std::to_string(id) + '/' + std::to_string(request.files.size()) + '/';
        const std::string_view full_path(path);
        if (!full_path.starts_with(prefix)) return WA_ERR_INVALID;
        const auto name = full_path.substr(prefix.size());
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos)
            return WA_ERR_INVALID;
        request.files.push_back({path, type});
        try {
            state->vm->MountReadOnlyHostFile(request.files.back().path);
        } catch (...) {
            request.files.pop_back();
            throw;
        }
        return WA_OK;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (const std::exception&) {
        return WA_ERR_NOENT;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int32_t wa_document_picker_complete(wa_sf_t* scene, uint32_t id, bool accepted) {
    auto* state = DocumentState(scene);
    if (!state || id != state->document->id) return WA_ERR_INVALID;
    if (!accepted) state->document->files.clear();
    state->document->ready = true;
    return WA_OK;
}
}
