package android.window;

public interface OnBackAnimationCallback extends OnBackInvokedCallback {
    default void onBackStarted(BackEvent event) {}
    default void onBackProgressed(BackEvent event) {}
    default void onBackCancelled() {}
}
