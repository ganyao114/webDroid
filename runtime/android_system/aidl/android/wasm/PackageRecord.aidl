package android.wasm;

parcelable PackageRecord {
    const int ABI_ARMEABI_V7A = 1;
    const int ABI_ARM64_V8A = 2;
    const int ABI_X86 = 4;
    const int ABI_X86_64 = 8;
    const int ABI_RISCV64 = 16;
    const int ABI_ARMEABI = 32;

    @utf8InCpp String packageName;
    @utf8InCpp String label;
    long versionCode;
    @utf8InCpp String mainActivity;
    @utf8InCpp String apkPath;
    boolean isHomeLauncher;
    int abiMask;
}
