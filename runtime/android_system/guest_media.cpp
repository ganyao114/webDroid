#include "guest_media.h"

#include "bionic_internal.h"
#include "bionic_math.h"

#include <sstream>

namespace wa::system {
namespace {

constexpr int32_t kInvalidMediaObject = -10003;

// The guest has no Android media service. NDK clients can choose their own software backend.
void NoMediaObject(wa_host_call_ctx_t&) {}

void InvalidMediaObject(wa_host_call_ctx_t& ctx) { ctx.ret0 = kInvalidMediaObject; }

void NoSample(wa_host_call_ctx_t& ctx) { ctx.ret0 = UINT64_MAX; }

void NoSampleTime(wa_host_call_ctx_t& ctx) {
    dbt::SetScalarResult(ctx, dbt::ArgKind::Long, UINT64_MAX);
}

void NoCodecBuffer(wa_host_call_ctx_t& ctx) {
    if (ctx.args[2]) dbt::GuestMemory(ctx).WriteWord(ctx.args[2], 0);
}

} // namespace

template<GuestMedia::FormatOp operation>
int32_t GuestMedia::FormatCall(wa_host_call_ctx_t* ctx, void* user) {
    try {
        static_cast<GuestMedia*>(user)->DispatchFormat<operation>(*ctx);
        return WA_OK;
    } catch (const dbt::GuestFault& fault) {
        return fault.status;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

template<GuestMedia::FormatOp operation> void GuestMedia::DispatchFormat(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const dbt::GuestMemory memory(ctx);
    if constexpr (operation == FormatOp::New) {
        const auto address = dbt::AllocateGuest(dbt, 1);
        if (!address) return;
        try {
            formats_.try_emplace(address);
        } catch (...) {
            dbt::FreeGuest(dbt, address);
            throw;
        }
        ctx.ret0 = address;
    } else {
        const auto found = formats_.find(ctx.args[0]);
        if (found == formats_.end()) {
            if constexpr (operation == FormatOp::Delete) ctx.ret0 = kInvalidMediaObject;
            return;
        }
        auto& format = found->second;
        if constexpr (operation == FormatOp::Delete) {
            dbt::FreeGuest(dbt, format.text);
            dbt::FreeGuest(dbt, found->first);
            formats_.erase(found);
        } else if constexpr (operation == FormatOp::String) {
            std::ostringstream text;
            bool first = true;
            for (const auto& [name, value] : format.values) {
                if (!std::exchange(first, false)) text << ", ";
                text << name << " = ";
                std::visit([&](const auto& field) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(field)>, std::vector<uint8_t>>)
                        text << "buffer(" << field.size() << ')';
                    else text << field;
                }, value);
            }
            const auto address = dbt::CopyString(dbt, text.str());
            dbt::FreeGuest(dbt, std::exchange(format.text, address));
            ctx.ret0 = address;
        } else {
            dbt::CallReader args(ctx);
            args.Next();
            const auto name = memory.String(args.Next());
            if constexpr (operation == FormatOp::GetInt || operation == FormatOp::GetString) {
                const auto value = format.values.find(name);
                if (value == format.values.end()) return;
                if constexpr (operation == FormatOp::GetInt) {
                    if (const auto* number = std::get_if<int32_t>(&value->second)) {
                        memory.Write(args.Next(), *number);
                        ctx.ret0 = 1;
                    }
                } else {
                    if (const auto* text = std::get_if<std::string>(&value->second)) {
                        memory.WriteWord(args.Next(), dbt::InternString(dbt, *text));
                        ctx.ret0 = 1;
                    }
                }
            } else if constexpr (operation == FormatOp::SetInt) {
                format.values.insert_or_assign(name, static_cast<int32_t>(args.Next()));
            } else if constexpr (operation == FormatOp::SetFloat) {
                format.values.insert_or_assign(name, dbt::ReadFloat<float>(args));
            } else if constexpr (operation == FormatOp::SetString) {
                format.values.insert_or_assign(name, memory.String(args.Next()));
            } else if constexpr (operation == FormatOp::SetBuffer) {
                const auto data = args.Next();
                const auto size = args.Next();
                std::vector<uint8_t> buffer;
                if (size > buffer.max_size()) throw std::bad_alloc();
                dbt::CheckGuest(dbt::CheckMappedAccess(&dbt, data, static_cast<size_t>(size), WA_PROT_READ));
                buffer.resize(static_cast<size_t>(size));
                memory.Read(data, buffer.data(), buffer.size());
                format.values.insert_or_assign(name, std::move(buffer));
            }
        }
    }
}

void GuestMedia::Register(wa_dbt_t* dbt) {
    constexpr std::pair<const char*, std::string_view> keys[]{
        {"AMEDIAFORMAT_KEY_AAC_PROFILE", "aac-profile"},
        {"AMEDIAFORMAT_KEY_BIT_RATE", "bitrate"},
        {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", "channel-count"},
        {"AMEDIAFORMAT_KEY_COLOR_FORMAT", "color-format"},
        {"AMEDIAFORMAT_KEY_FRAME_RATE", "frame-rate"},
        {"AMEDIAFORMAT_KEY_HEIGHT", "height"},
        {"AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", "i-frame-interval"},
        {"AMEDIAFORMAT_KEY_MAX_INPUT_SIZE", "max-input-size"},
        {"AMEDIAFORMAT_KEY_MIME", "mime"},
        {"AMEDIAFORMAT_KEY_SAMPLE_RATE", "sample-rate"},
        {"AMEDIAFORMAT_KEY_STRIDE", "stride"},
        {"AMEDIAFORMAT_KEY_WIDTH", "width"},
    };
    const dbt::GuestMemory memory(dbt->mmu, dbt->arch);
    auto slot = dbt::AllocateGuest(*dbt, std::size(keys) * memory.width());
    if (!slot) throw dbt::GuestFault{WA_ERR_NOMEM};
    for (const auto& [name, key] : keys) {
        memory.WriteWord(slot, dbt::InternString(*dbt, key));
        dbt::CheckGuest(wa_dbt_register_data(dbt, "libmediandk.so", name, slot));
        slot += memory.width();
    }

    constexpr dbt::Builtin formats[]{
        {"AMediaFormat_new", FormatCall<FormatOp::New>},
        {"AMediaFormat_delete", FormatCall<FormatOp::Delete>},
        {"AMediaFormat_getInt32", FormatCall<FormatOp::GetInt>},
        {"AMediaFormat_getString", FormatCall<FormatOp::GetString>},
        {"AMediaFormat_setInt32", FormatCall<FormatOp::SetInt>},
        {"AMediaFormat_setFloat", FormatCall<FormatOp::SetFloat>},
        {"AMediaFormat_setString", FormatCall<FormatOp::SetString>},
        {"AMediaFormat_setBuffer", FormatCall<FormatOp::SetBuffer>},
        {"AMediaFormat_toString", FormatCall<FormatOp::String>},
    };
    for (const auto& entry : formats) {
        dbt::CheckGuest(wa_dbt_register_symbol(dbt, "libmediandk.so", entry.name, entry.function, this));
    }
    constexpr dbt::Builtin media[]{
        {"AMediaCodec_createDecoderByType", dbt::Dispatch<NoMediaObject>},
        {"AMediaCodec_createEncoderByType", dbt::Dispatch<NoMediaObject>},
        {"AMediaCodec_configure", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_delete", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_dequeueInputBuffer", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_dequeueOutputBuffer", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_flush", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_getInputBuffer", dbt::Dispatch<NoCodecBuffer>},
        {"AMediaCodec_getOutputBuffer", dbt::Dispatch<NoCodecBuffer>},
        {"AMediaCodec_getOutputFormat", dbt::Dispatch<NoMediaObject>},
        {"AMediaCodec_queueInputBuffer", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_releaseOutputBuffer", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_start", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaCodec_stop", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaExtractor_new", dbt::Dispatch<NoMediaObject>},
        {"AMediaExtractor_delete", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaExtractor_setDataSourceFd", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaExtractor_getTrackCount", dbt::Dispatch<NoMediaObject>},
        {"AMediaExtractor_getTrackFormat", dbt::Dispatch<NoMediaObject>},
        {"AMediaExtractor_selectTrack", dbt::Dispatch<InvalidMediaObject>},
        {"AMediaExtractor_getSampleTime", dbt::Dispatch<NoSampleTime>},
        {"AMediaExtractor_readSampleData", dbt::Dispatch<NoSample>},
        {"AMediaExtractor_advance", dbt::Dispatch<NoMediaObject>},
        {"AMediaExtractor_seekTo", dbt::Dispatch<InvalidMediaObject>},
    };
    dbt::Register(*dbt, "libmediandk.so", media);
}

} // namespace wa::system
