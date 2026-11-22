package android.view;

import android.graphics.Insets;

public final class WindowInsets {
    public static final class Type {
        @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
        public @interface InsetsType {}
        private Type() {}
        public static int statusBars() { return 1; }
        public static int navigationBars() { return 2; }
        public static int systemBars() { return statusBars() | navigationBars(); }
        public static int ime() { return 8; }
        public static int systemGestures() { return 16; }
        public static int mandatorySystemGestures() { return 32; }
        public static int tappableElement() { return 64; }
        public static int displayCutout() { return 128; }
    }

    static final WindowInsets NONE = new WindowInsets(0, 0);
    private final Insets status, navigation, gestures;

    WindowInsets(int statusPx, int navigationPx) {
        this(statusPx, navigationPx, navigationPx);
    }
    WindowInsets(int statusPx, int navigationPx, int gesturePx) {
        status = Insets.of(0, statusPx, 0, 0);
        navigation = Insets.of(0, 0, 0, navigationPx);
        gestures = Insets.of(0, 0, 0, gesturePx);
    }

    public Insets getInsets(int mask) {
        Insets result = (mask & (Type.statusBars() | Type.tappableElement())) == 0 ? Insets.NONE : status;
        if ((mask & Type.navigationBars()) != 0) result = Insets.max(result, navigation);
        return (mask & (Type.systemGestures() | Type.mandatorySystemGestures())) == 0
                ? result : Insets.max(result, gestures);
    }
    public Insets getInsetsIgnoringVisibility(int mask) { return getInsets(mask); }
    public boolean isVisible(int mask) {
        int visible = (status.top == 0 ? 0 : Type.statusBars() | Type.tappableElement())
                | (navigation.bottom == 0 ? 0 : Type.navigationBars())
                | (gestures.bottom == 0 ? 0 : Type.systemGestures() | Type.mandatorySystemGestures());
        return (mask & ~visible) == 0;
    }
    static Insets insetInsets(Insets insets, int left, int top, int right, int bottom) {
        return Insets.of(Math.max(0, insets.left - left), Math.max(0, insets.top - top),
                Math.max(0, insets.right - right), Math.max(0, insets.bottom - bottom));
    }
    public DisplayCutout getDisplayCutout() { return null; }
    public Insets getSystemGestureInsets() { return getInsets(Type.systemGestures()); }
    public Insets getMandatorySystemGestureInsets() { return getInsets(Type.mandatorySystemGestures()); }
    public Insets getTappableElementInsets() { return getInsets(Type.tappableElement()); }
    public int getSystemWindowInsetLeft() { return getInsets(Type.systemBars()).left; }
    public int getSystemWindowInsetTop() { return getInsets(Type.systemBars()).top; }
    public int getSystemWindowInsetRight() { return getInsets(Type.systemBars()).right; }
    public int getSystemWindowInsetBottom() { return getInsets(Type.systemBars()).bottom; }
}
