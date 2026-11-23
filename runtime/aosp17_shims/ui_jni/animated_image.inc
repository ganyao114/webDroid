namespace {

jlong CreateAnimatedImage(JNIEnv*, jclass, jlong, jobject, jint, jint, jlong, jboolean, jobject) {
    // Animated ImageDecoder allocation is unavailable in the software runtime.
    // The Java constructor treats a zero handle as a decode failure.
    return 0;
}

template <class Result, Result Empty, class... Args>
Result EmptyAnimatedImage(JNIEnv* env, jclass, jlong handle, Args...) {
    if (handle) Throw(env, "java/lang/IllegalStateException", "Invalid animated image handle");
    return Empty;
}
template <class... Args>
void SetAnimatedImage(JNIEnv* env, jclass, jlong handle, Args...) {
    if (handle) Throw(env, "java/lang/IllegalStateException", "Invalid animated image handle");
}

void RegisterAnimatedImages(interp::Vm& vm) {
    if (!vm.HasClass("Landroid/graphics/drawable/AnimatedImageDrawable;")) return;
    Register(vm, "android/graphics/drawable/AnimatedImageDrawable",
             Method<CreateAnimatedImage>(vm, "nCreate", "(JLandroid/graphics/ImageDecoder;IIJZLandroid/graphics/Rect;)J"),
             Method<ManagedFinalizer>(vm, "nGetNativeFinalizer", "()J"),
             Method<EmptyAnimatedImage<jlong, -1, jlong>>(vm, "nDraw", "(JJ)J"),
             Method<EmptyAnimatedImage<jlong, 0>>(vm, "nNativeByteSize", "(J)J"),
             Method<EmptyAnimatedImage<jint, 255>>(vm, "nGetAlpha", "(J)I"),
             Method<EmptyAnimatedImage<jint, 0>>(vm, "nGetRepeatCount", "(J)I"),
             Method<EmptyAnimatedImage<jboolean, false>>(vm, "nIsRunning", "(J)Z"),
             Method<EmptyAnimatedImage<jboolean, false>>(vm, "nStart", "(J)Z"),
             Method<EmptyAnimatedImage<jboolean, false>>(vm, "nStop", "(J)Z"),
             Method<EmptyAnimatedImage<jboolean, false>>(vm, "nGetFilterBitmap", "(J)Z"),
             Method<EmptyAnimatedImage<jboolean, false, jboolean>>(vm, "nSetFilterBitmap", "(JZ)Z"),
             Method<SetAnimatedImage<jint>>(vm, "nSetAlpha", "(JI)V"),
             Method<SetAnimatedImage<jint>>(vm, "nSetRepeatCount", "(JI)V"),
             Method<SetAnimatedImage<jboolean>>(vm, "nSetMirrored", "(JZ)V"),
             Method<SetAnimatedImage<jlong>>(vm, "nSetColorFilter", "(JJ)V"),
             Method<SetAnimatedImage<jobject>>(vm, "nSetBounds", "(JLandroid/graphics/Rect;)V"),
             Method<SetAnimatedImage<jobject>>(vm, "nSetOnAnimationEndListener", "(JLjava/lang/ref/WeakReference;)V"),
             Method<SetAnimatedImage<jobject>>(vm, "nSetOnFrameRateHintListener", "(JLjava/lang/ref/WeakReference;)V"));
}

} // namespace
