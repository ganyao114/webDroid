package android.view;

import android.graphics.Insets;

public final class WindowInsets {
    public static final class Type {
        private Type() {}
        public static int statusBars() { return 1; }
        public static int navigationBars() { return 2; }
        public static int systemBars() { return statusBars() | navigationBars(); }
        public static int ime() { return 8; }
        public static int displayCutout() { return 128; }
    }

    static final WindowInsets NONE = new WindowInsets(0, 0);
    private final Insets status, navigation;

    WindowInsets(int statusPx, int navigationPx) {
        status = Insets.of(0, statusPx, 0, 0);
        navigation = Insets.of(0, 0, 0, navigationPx);
    }

    public Insets getInsets(int mask) {
        Insets result = (mask & Type.statusBars()) == 0 ? Insets.NONE : status;
        return (mask & Type.navigationBars()) == 0 ? result : Insets.max(result, navigation);
    }
}
