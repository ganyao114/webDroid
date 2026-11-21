#pragma once

// Standalone settings for the two C accessors normally emitted by Soong aconfig.
inline constexpr bool android_content_res_xml_file_size_limit() {
    return true;
}

inline constexpr bool android_content_res_resource_readwrite_flags() {
    return true;
}
