namespace {

constexpr jint kBadValue = -2, kInvalidOperation = -3, kWouldBlock = -7;
constexpr uint32_t kMinimumBufferMs = 40;

using audio::Encoding;

struct AudioTrack {
    audio::PcmStream pcm;
    Encoding encoding;
    jint sample_rate, session, capacity, buffer_size;
    jint start_threshold, marker = 0, period = 0;
    jint loop_start = 0, loop_end = 0, loop_count = 0, loops_left = 0;
    uint32_t head_offset = 0, read_offset = 0;
    std::vector<uint8_t> pending;
    bool static_mode, playing = false;
    jint rate;
    jfloat speed = 1;

    void Fill() {
        if (!playing || pending.empty()) return;
        const auto bytes_per_frame = pcm.FrameBytes();
        auto frames = static_cast<uint32_t>(pending.size() / bytes_per_frame);
        while (read_offset < frames) {
            const auto end = loops_left ? static_cast<uint32_t>(loop_end) : frames;
            const auto written = pcm.Write(std::span(pending).subspan(
                size_t(read_offset) * bytes_per_frame, size_t(end - read_offset) * bytes_per_frame));
            read_offset += written;
            if (read_offset < end) return;
            if (loops_left) {
                read_offset = loop_start;
                if (loops_left > 0) --loops_left;
            }
        }
        if (!static_mode) {
            pending.clear();
            read_offset = 0;
        }
    }

    void ResetPosition(uint32_t position = 0) {
        pcm.Reset();
        read_offset = head_offset = position;
        const auto end = static_cast<uint32_t>(loop_end);
        loops_left = position < end && end <= pending.size() / pcm.FrameBytes() ? loop_count : 0;
    }

    uint64_t Head() { return head_offset + pcm.Position(); }

    uint32_t Write(std::span<const uint8_t> bytes) {
        const auto frame_bytes = pcm.FrameBytes();
        if (!static_mode && playing) {
            Fill();
            return pending.empty() ? pcm.Write(bytes) : 0;
        }
        const auto limit = size_t(static_mode ? capacity : buffer_size) * frame_bytes;
        const auto occupied = static_mode ? 0 : pending.size();
        const auto count = std::min(bytes.size(), limit - std::min(limit, occupied)) / frame_bytes * frame_bytes;
        if (static_mode && count) {
            if (playing) return 0;
            pending.assign(bytes.begin(), bytes.begin() + count);
            ResetPosition();
        } else {
            pending.insert(pending.end(), bytes.begin(), bytes.begin() + count);
        }
        return static_cast<uint32_t>(count / frame_bytes);
    }
};

} // namespace

struct AudioTracks {
    jfieldID handle;
    jlong next_id = 1;
    jint next_session = 1;
    std::unordered_map<jlong, AudioTrack> tracks;
};

namespace {

AudioTrack* GetTrack(JNIEnv* env, jobject owner) {
    const auto* state = GetState(env);
    if (state && state->audio_tracks) {
        auto& store = *state->audio_tracks;
        const auto it = store.tracks.find(env->GetLongField(owner, store.handle));
        if (it != store.tracks.end()) return &it->second;
    }
    return nullptr;
}

jint SetupTrack(JNIEnv* env, jobject owner, jobject, jobject, jintArray sample_rate, jobject masks,
                jint format, jint bytes, jint mode, jintArray session, jobject, jlong native_track,
                jboolean offload, jint encapsulation, jobject, jstring, jstring) {
    return CallSafely(env, [&]() -> jint {
        if (native_track || offload || encapsulation || !sample_rate || !session || !masks ||
            bytes <= 0 || (mode != 0 && mode != 1)) return kBadValue;
        auto* state = GetState(env);
        if (!state || !state->audio_tracks || GetTrack(env, owner)) return kInvalidOperation;
        LocalFrame locals(env);
        const auto get_count = env->GetMethodID(env->GetObjectClass(masks), "getChannelCount", "()I");
        if (!get_count) return kBadValue;
        const auto channels = env->CallIntMethod(masks, get_count);
        jint rate = 0, id = 0;
        env->GetIntArrayRegion(sample_rate, 0, 1, &rate);
        env->GetIntArrayRegion(session, 0, 1, &id);
        if (env->ExceptionCheck()) return kBadValue;
        if (!rate) rate = WA_AUDIO_SAMPLE_RATE;
        const auto encoding = static_cast<Encoding>(format);
        if (!audio::SupportsPcm(rate, channels, encoding)) return kBadValue;
        const auto frame_bytes = channels * audio::SampleBytes(encoding);
        if (bytes % frame_bytes) return kBadValue;
        auto& store = *state->audio_tracks;
        const auto capacity = static_cast<jint>(bytes / frame_bytes);
        const auto size = mode == 0 ? capacity :
            std::max(capacity, static_cast<jint>(rate * kMinimumBufferMs / 1000));
        const auto track_id = store.next_id++;
        if (!id) id = store.next_session++;
        store.tracks.emplace(track_id, AudioTrack{
            .pcm = audio::PcmStream(audio::Backend::AudioTrack, rate, channels, encoding),
            .encoding = encoding, .sample_rate = rate, .session = id, .capacity = size,
            .buffer_size = size, .start_threshold = size, .static_mode = mode == 0, .rate = rate,
        });
        env->SetLongField(owner, store.handle, track_id);
        env->SetIntArrayRegion(sample_rate, 0, 1, &rate);
        env->SetIntArrayRegion(session, 0, 1, &id);
        return 0;
    });
}

void ReleaseTrack(JNIEnv* env, jobject owner) {
    const auto* state = GetState(env);
    if (!state || !state->audio_tracks) return;
    auto& store = *state->audio_tracks;
    store.tracks.erase(env->GetLongField(owner, store.handle));
    env->SetLongField(owner, store.handle, 0);
}

enum class TrackAction { Start, Stop, Pause, Flush };

template <TrackAction Action> void ControlTrack(JNIEnv* env, jobject owner) {
    auto* track = GetTrack(env, owner);
    if (!track) return;
    if constexpr (Action == TrackAction::Start) {
        track->playing = true;
        track->pcm.SetPlaying(true);
        track->Fill();
    } else if constexpr (Action == TrackAction::Pause) {
        track->playing = false;
        track->pcm.SetPlaying(false);
    } else {
        if constexpr (Action == TrackAction::Stop) {
            track->playing = false;
            track->pcm.SetPlaying(false);
        }
        if (!track->static_mode) track->pending.clear();
        track->ResetPosition();
    }
}

template <class Array, class Entry, auto GetRegion>
jint WriteArray(JNIEnv* env, jobject owner, Array data, jint offset, jint count, jint format, jboolean) {
    auto* track = GetTrack(env, owner);
    if (!track) return kInvalidOperation;
    if (!data || offset < 0 || count < 0 || count > env->GetArrayLength(data) - offset ||
        static_cast<Encoding>(format) != track->encoding) return kBadValue;
    return CallSafely(env, [&]() -> jint {
        const auto frame = track->pcm.FrameBytes();
        const auto entries_per_frame = frame / sizeof(Entry);
        if (!entries_per_frame) return kBadValue;
        count -= count % entries_per_frame;
        if (!count) return 0;
        if (track->static_mode) {
            count = std::min<uint32_t>(count, uint32_t(track->capacity) * entries_per_frame);
            std::vector<Entry> pcm(count);
            (env->functions->*GetRegion)(env, data, offset, count, pcm.data());
            if (env->ExceptionCheck()) return kBadValue;
            return track->Write({reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(Entry)}) *
                entries_per_frame;
        }
        std::array<Entry, 4096 / sizeof(Entry)> pcm;
        jint total = 0;
        while (total < count) {
            const auto n = std::min<uint32_t>(count - total, pcm.size() / entries_per_frame * entries_per_frame);
            (env->functions->*GetRegion)(env, data, offset + total, n, pcm.data());
            if (env->ExceptionCheck()) return kBadValue;
            const auto samples = track->Write({reinterpret_cast<const uint8_t*>(pcm.data()), n * sizeof(Entry)}) *
                entries_per_frame;
            total += samples;
            if (samples < n) break;
        }
        return total;
    });
}

jint WriteBuffer(JNIEnv* env, jobject owner, jobject data, jint offset, jint count, jint format, jboolean blocking) {
    if (!data || offset < 0 || count < 0) return kBadValue;
    LocalFrame locals(env);
    const auto type = env->GetObjectClass(data);
    const auto array_field = env->GetFieldID(type, "hb", "[B");
    const auto offset_field = env->GetFieldID(type, "offset", "I");
    if (!array_field || !offset_field) return kBadValue;
    const auto base = env->GetIntField(data, offset_field);
    if (base < 0 || offset > INT32_MAX - base) return kBadValue;
    const auto array = static_cast<jbyteArray>(env->GetObjectField(data, array_field));
    // Both the VM's direct buffers and read-only heap views retain a byte-array peer.
    return WriteArray<jbyteArray, jbyte, &JNINativeInterface::GetByteArrayRegion>(
        env, owner, array, base + offset, count, format, blocking);
}

jint MinimumBuffer(JNIEnv*, jclass, jint rate, jint channels, jint encoding) {
    const auto format = static_cast<Encoding>(encoding);
    if (!audio::SupportsPcm(rate, channels, format)) return kBadValue;
    return (rate * kMinimumBufferMs + 999) / 1000 * channels * audio::SampleBytes(format);
}

void SetVolume(JNIEnv* env, jobject owner, jfloat left, jfloat right) {
    if (auto* track = GetTrack(env, owner)) track->pcm.SetVolume(left, right);
}

template <auto Field> jint GetTrackInt(JNIEnv* env, jobject owner) {
    const auto* track = GetTrack(env, owner);
    return track ? track->*Field : kInvalidOperation;
}

template <auto Field, jint Minimum, bool Bounded = false>
jint SetTrackInt(JNIEnv* env, jobject owner, jint value) {
    auto* track = GetTrack(env, owner);
    if (!track) return kInvalidOperation;
    if (value < Minimum) return kBadValue;
    if constexpr (Bounded) value = std::min(track->capacity, std::max(value, 1));
    track->*Field = value;
    if constexpr (Bounded) return value;
    return 0;
}

jint SetRate(JNIEnv* env, jobject owner, jint rate) {
    auto* track = GetTrack(env, owner);
    if (!track) return kInvalidOperation;
    if (rate < 4000 || rate > 192000) return kBadValue;
    track->rate = rate;
    track->speed = 1;
    track->pcm.SetRate(rate);
    return 0;
}

void SetPlaybackParams(JNIEnv* env, jobject owner, jobject params) {
    auto* track = GetTrack(env, owner);
    if (!track || !params) return;
    LocalFrame locals(env);
    const auto type = env->GetObjectClass(params);
    const auto speed_field = env->GetFieldID(type, "mSpeed", "F");
    const auto pitch_field = env->GetFieldID(type, "mPitch", "F");
    if (!speed_field || !pitch_field) return;
    const auto speed = env->GetFloatField(params, speed_field);
    const auto pitch = env->GetFloatField(params, pitch_field);
    const auto rate = track->rate * speed;
    if (!std::isfinite(rate) || rate < 4000 || rate > 192000 || pitch != speed) {
        Throw(env, "java/lang/IllegalArgumentException", "The PCM sink changes pitch with playback speed");
        return;
    }
    track->speed = speed;
    track->pcm.SetRate(static_cast<uint32_t>(rate));
}

jobject GetPlaybackParams(JNIEnv* env, jobject owner) {
    auto* track = GetTrack(env, owner);
    if (!track) return nullptr;
    LocalFrame locals(env);
    const auto type = env->FindClass("android/media/PlaybackParams");
    if (!type) return nullptr;
    const auto ctor = env->GetMethodID(type, "<init>", "()V");
    if (!ctor) return nullptr;
    const auto params = env->NewObject(type, ctor);
    if (!params) return nullptr;
    constexpr auto signature = "(F)Landroid/media/PlaybackParams;";
    const auto speed = env->GetMethodID(type, "setSpeed", signature);
    const auto pitch = env->GetMethodID(type, "setPitch", signature);
    const auto defaults = env->GetMethodID(type, "allowDefaults", "()Landroid/media/PlaybackParams;");
    if (!speed || !pitch || !defaults) return nullptr;
    env->CallObjectMethod(params, speed, track->speed);
    env->CallObjectMethod(params, pitch, track->speed);
    env->CallObjectMethod(params, defaults);
    return locals.Keep(params);
}

jint PositionTrack(JNIEnv* env, jobject owner) {
    auto* track = GetTrack(env, owner);
    return track ? static_cast<jint>(track->Head()) : 0;
}

jlong WrittenTrack(JNIEnv* env, jobject owner) {
    const auto* track = GetTrack(env, owner);
    return track ? track->pcm.Written() + track->pending.size() / track->pcm.FrameBytes() - track->read_offset : 0;
}

jint Timestamp(JNIEnv* env, jobject owner, jlongArray output) {
    auto* track = GetTrack(env, owner);
    if (!track) return kInvalidOperation;
    if (!output || env->GetArrayLength(output) < 2) return kBadValue;
    const auto head = track->Head();
    if (!head) return kWouldBlock;
    const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(audio::Clock::now().time_since_epoch());
    const jlong values[]{static_cast<jlong>(head), time.count()};
    env->SetLongArrayRegion(output, 0, 2, values);
    return 0;
}

jint SeekTrack(JNIEnv* env, jobject owner, jint position) {
    auto* track = GetTrack(env, owner);
    if (!track || !track->static_mode || track->playing) return kInvalidOperation;
    if (position < 0 || uint32_t(position) > track->pending.size() / track->pcm.FrameBytes()) return kBadValue;
    track->ResetPosition(position);
    return 0;
}

jint LoopTrack(JNIEnv* env, jobject owner, jint start, jint end, jint count) {
    auto* track = GetTrack(env, owner);
    if (!track || !track->static_mode || track->playing) return kInvalidOperation;
    if (count < -1 || (count && (start < 0 || end <= start || end > track->capacity))) return kBadValue;
    track->loop_start = count ? start : 0;
    track->loop_end = count ? end : 0;
    track->loop_count = count;
    return SeekTrack(env, owner, track->head_offset);
}

jint Latency(JNIEnv* env, jobject owner) {
    auto* track = GetTrack(env, owner);
    if (!track) return 0;
    const auto played = track->pcm.Position();
    return static_cast<jint>((track->pcm.Written() - played) * 1000 / track->pcm.Rate());
}

jobject TrackMetrics(JNIEnv* env, jobject owner) {
    auto* track = GetTrack(env, owner);
    if (!track) return nullptr;
    LocalFrame locals(env);
    const auto type = env->FindClass("android/os/PersistableBundle");
    if (!type) return nullptr;
    const auto init = env->GetMethodID(type, "<init>", "()V");
    const auto put = env->GetMethodID(type, "putInt", "(Ljava/lang/String;I)V");
    if (!init || !put) return nullptr;
    const auto bundle = env->NewObject(type, init);
    if (!bundle) return nullptr;
    env->CallVoidMethod(bundle, put, env->NewStringUTF("android.media.audiotrack.samplerate"), track->sample_rate);
    env->CallVoidMethod(bundle, put, env->NewStringUTF("android.media.audiotrack.framecount"), track->capacity);
    return locals.Keep(bundle);
}

template <class... Args> jint UnavailableInt(JNIEnv*, jobject, Args...) { return kInvalidOperation; }
template <class... Args> void IgnoreTrackOption(JNIEnv*, jobject, Args...) {}

void RegisterAudioTrack(interp::Vm& vm, State& state) {
    if (!vm.HasClass("Landroid/media/AudioTrack;")) return;
    Register(vm, "android/media/AudioTrack",
        Method<TrackMetrics>(vm, "native_getMetrics", "()Landroid/os/PersistableBundle;"),
        Method<+[](JNIEnv*, jclass, jint, jint, jobject, jint, jint, jint) -> jboolean { return JNI_FALSE; }>(
            vm, "native_is_direct_output_supported", "(IILjava/lang/Object;III)Z"),
        Method<SetupTrack>(vm, "native_setup",
            "(Ljava/lang/Object;Ljava/lang/Object;[ILjava/lang/Object;III[ILandroid/os/Parcel;"
            "JZILjava/lang/Object;Ljava/lang/String;Ljava/lang/String;)I"),
        Method<ReleaseTrack>(vm, "native_finalize", "()V"),
        Method<ReleaseTrack>(vm, "native_release", "()V"),
        Method<ControlTrack<TrackAction::Start>>(vm, "native_start", "()V"),
        Method<ControlTrack<TrackAction::Stop>>(vm, "native_stop", "()V"),
        Method<ControlTrack<TrackAction::Pause>>(vm, "native_pause", "()V"),
        Method<ControlTrack<TrackAction::Flush>>(vm, "native_flush", "()V"),
        Method<WriteArray<jbyteArray, jbyte, &JNINativeInterface::GetByteArrayRegion>>(
            vm, "native_write_byte", "([BIIIZ)I"),
        Method<WriteArray<jshortArray, jshort, &JNINativeInterface::GetShortArrayRegion>>(
            vm, "native_write_short", "([SIIIZ)I"),
        Method<WriteArray<jfloatArray, jfloat, &JNINativeInterface::GetFloatArrayRegion>>(
            vm, "native_write_float", "([FIIIZ)I"),
        Method<WriteBuffer>(vm, "native_write_native_bytes", "(Ljava/nio/ByteBuffer;IIIZ)I"),
        Method<+[](JNIEnv* env, jobject owner) { return SeekTrack(env, owner, 0); }>(
            vm, "native_reload_static", "()I"),
        Method<GetTrackInt<&AudioTrack::buffer_size>>(vm, "native_get_buffer_size_frames", "()I"),
        Method<SetTrackInt<&AudioTrack::buffer_size, 0, true>>(vm, "native_set_buffer_size_frames", "(I)I"),
        Method<GetTrackInt<&AudioTrack::capacity>>(vm, "native_get_buffer_capacity_frames", "()I"),
        Method<SetVolume>(vm, "native_setVolume", "(FF)V"),
        Method<SetRate>(vm, "native_set_playback_rate", "(I)I"),
        Method<GetTrackInt<&AudioTrack::rate>>(vm, "native_get_playback_rate", "()I"),
        Method<SetPlaybackParams>(vm, "native_set_playback_params", "(Landroid/media/PlaybackParams;)V"),
        Method<GetPlaybackParams>(vm, "native_get_playback_params", "()Landroid/media/PlaybackParams;"),
        Method<SetTrackInt<&AudioTrack::marker, 0>>(vm, "native_set_marker_pos", "(I)I"),
        Method<GetTrackInt<&AudioTrack::marker>>(vm, "native_get_marker_pos", "()I"),
        Method<SetTrackInt<&AudioTrack::period, 0>>(vm, "native_set_pos_update_period", "(I)I"),
        Method<GetTrackInt<&AudioTrack::period>>(vm, "native_get_pos_update_period", "()I"),
        Method<WrittenTrack>(vm, "native_get_written_frames_count", "()J"),
        Method<SeekTrack>(vm, "native_set_position", "(I)I"),
        Method<PositionTrack>(vm, "native_get_position", "()I"),
        Method<Latency>(vm, "native_get_latency", "()I"),
        Method<+[](JNIEnv*, jobject) -> jint { return 0; }>(vm, "native_get_underrun_count", "()I"),
        Method<+[](JNIEnv*, jobject) -> jint { return 0; }>(vm, "native_get_flags", "()I"),
        Method<Timestamp>(vm, "native_get_timestamp", "([J)I"),
        Method<LoopTrack>(vm, "native_set_loop", "(III)I"),
        Method<+[](JNIEnv*, jclass, jint) -> jint { return WA_AUDIO_SAMPLE_RATE; }>(
            vm, "native_get_output_sample_rate", "(I)I"),
        Method<MinimumBuffer>(vm, "native_get_min_buff_size", "(III)I"),
        Method<+[](JNIEnv*, jobject, jint effect) -> jint { return effect ? kInvalidOperation : 0; }>(
            vm, "native_attachAuxEffect", "(I)I"),
        Method<UnavailableInt<jfloat>>(vm, "native_setAuxEffectSendLevel", "(F)I"),
        Method<+[](JNIEnv*, jobject, jint device) -> jboolean { return device == 0; }>(
            vm, "native_setOutputDevice", "(I)Z"),
        Method<+[](JNIEnv* env, jobject) { return env->NewIntArray(0); }>(vm, "native_getRoutedDeviceIds", "()[I"),
        Method<IgnoreTrackOption<>>(vm, "native_enableDeviceCallback", "()V"),
        Method<IgnoreTrackOption<>>(vm, "native_disableDeviceCallback", "()V"),
        Method<UnavailableInt<jobject, jobject>>(vm, "native_applyVolumeShaper",
            "(Landroid/media/VolumeShaper$Configuration;Landroid/media/VolumeShaper$Operation;)I"),
        Method<+[](JNIEnv*, jobject, jint) -> jobject { return nullptr; }>(
            vm, "native_getVolumeShaperState", "(I)Landroid/media/VolumeShaper$State;"),
        Method<UnavailableInt<jint, jint>>(vm, "native_setPresentation", "(II)I"),
        Method<GetTrackInt<&AudioTrack::session>>(vm, "native_getPortId", "()I"),
        Method<IgnoreTrackOption<jint, jint>>(vm, "native_set_delay_padding", "(II)V"),
        Method<UnavailableInt<jfloat>>(vm, "native_set_audio_description_mix_level_db", "(F)I"),
        Method<UnavailableInt<jfloatArray>>(vm, "native_get_audio_description_mix_level_db", "([F)I"),
        Method<UnavailableInt<jint>>(vm, "native_set_dual_mono_mode", "(I)I"),
        Method<UnavailableInt<jintArray>>(vm, "native_get_dual_mono_mode", "([I)I"),
        Method<IgnoreTrackOption<jstring>>(vm, "native_setLogSessionId", "(Ljava/lang/String;)V"),
        Method<SetTrackInt<&AudioTrack::start_threshold, 1, true>>(vm, "native_setStartThresholdInFrames", "(I)I"),
        Method<GetTrackInt<&AudioTrack::start_threshold>>(vm, "native_getStartThresholdInFrames", "()I"),
        Method<+[](JNIEnv*, jclass, jint, jint, jobject, jobject) -> jint { return 0; }>(
            vm, "native_getFlushWrittenFramesFromPositionSupport",
            "(IILandroid/media/AudioFormat$ChannelMasks;Landroid/media/AudioAttributes;)I"),
        Method<+[](JNIEnv*, jobject, jint, jlong) -> jlong { return kInvalidOperation; }>(
            vm, "native_flushFromFrame", "(IJ)J"),
        Method<IgnoreTrackOption<jint>>(vm, "native_setPlayerIId", "(I)V"));
    auto* env = vm.JniEnv();
    const auto type = env->FindClass("android/media/AudioTrack");
    const auto handle = type ? env->GetFieldID(type, "mNativeTrackInJavaObj", "J") : nullptr;
    if (!handle) throw std::runtime_error("AudioTrack native peer handle is absent");
    state.audio_tracks = std::make_shared<AudioTracks>(AudioTracks{.handle = handle});
}

void PumpAudio(State& state) {
    bool playing = false;
    if (state.audio_tracks) {
        for (auto& [id, track] : state.audio_tracks->tracks) {
            track.Fill();
            playing |= track.playing;
        }
    }
    audio::PumpWorkers(playing);
}

} // namespace
