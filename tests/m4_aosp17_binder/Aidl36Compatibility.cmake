# SDK 36 emits a default-implementation fallback for UNKNOWN_TRANSACTION.
# AOSP 17 removed getDefaultImpl(). Keep the generated proxy's status handling
# and omit the obsolete fallback from the build copy.
file(READ "${INPUT}" contents)
string(REGEX REPLACE
    "  if \\(_aidl_ret_status == ::android::UNKNOWN_TRANSACTION && ICalcService::getDefaultImpl\\(\\)\\) \\[\\[unlikely\\]\\] \\{\n     return ICalcService::getDefaultImpl\\(\\)->[^\n]+\n  \\}\n"
    "" contents "${contents}")
if(contents MATCHES "getDefaultImpl")
    message(FATAL_ERROR "Unrecognized default-implementation fallback in ${INPUT}")
endif()
file(WRITE "${OUTPUT}" "${contents}")
