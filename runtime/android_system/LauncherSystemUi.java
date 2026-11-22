package org.wasmandroid.launcher;

import android.app.Activity;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.view.ContextThemeWrapper;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.ImageView;

import java.io.IOException;
import java.util.function.Consumer;

/** Gives the embedded SystemUI views their own APK resources and a window to send keys to. */
public final class LauncherSystemUi extends ContextThemeWrapper implements Consumer<KeyEvent> {
    private static final int KEYCODE_APP_SWITCH = 187;

    private final Activity activity;
    private final Resources resources;
    private FrameLayout overlay;
    private int keyEvents;
    private int lastKeyCode;
    private int lastKeyFlags;
    private boolean recentsPending;

    public static LauncherSystemUi create(Activity activity, String frameworkApk, String systemUiApk,
            String locale, int width, int height, int density) throws IOException {
        Resources resources = Resources.forApk(new String[] {frameworkApk, systemUiApk}, locale, density);
        try {
            resources.updateDisplay(width, height, density);
            Resources.Theme theme = resources.newTheme();
            theme.applyStyle(android.R.style.Theme_DeviceDefault, true);
            return new LauncherSystemUi(activity, resources, theme);
        } catch (RuntimeException | Error error) {
            resources.close();
            throw error;
        }
    }

    private LauncherSystemUi(Activity activity, Resources resources, Resources.Theme theme) {
        super(activity, theme);
        this.activity = activity;
        this.resources = resources;
    }

    @Override public Resources getResources() { return resources; }
    @Override public AssetManager getAssets() { return resources.getAssets(); }

    public FrameLayout attachOverlay() {
        overlay = new FrameLayout(this);
        ((FrameLayout) activity.getWindow().getDecorView()).addView(overlay,
                new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));
        return overlay;
    }

    public void layoutNavigation(View navigation, int height) {
        var navParams = navigation.getLayoutParams();
        if (navParams.height != height) {
            navParams.height = height;
            navigation.setLayoutParams(navParams);
        }
        // The framework's edge-to-edge window already insets Launcher3 for its gesture bar.
        // Reserve the extra height of the inflated navigation bar outside the app content.
        var window = activity.getWindow();
        var metrics = window.getWindowManager().getCurrentWindowMetrics();
        int inset = metrics.getWindowInsets().getInsets(WindowInsets.Type.navigationBars()).bottom;
        int scaledInset = Math.round(inset * resources.getDisplayMetrics().density / metrics.getDensity());
        var content = window.getContentParent();
        var contentParams = (FrameLayout.LayoutParams) content.getLayoutParams();
        int margin = Math.max(0, height - scaledInset);
        if (contentParams.bottomMargin != margin) {
            contentParams.bottomMargin = margin;
            content.setLayoutParams(contentParams);
        }
    }

    public void tintSignals(View status, int tint) {
        for (String name : new String[] {"wifi_signal", "mobile_signal"}) {
            int id = resources.getIdentifier(name, "id", "com.android.systemui");
            ImageView signal = status.findViewById(id);
            if (signal != null && signal.getDrawable() != null) signal.getDrawable().setTint(tint);
        }
    }

    @Override public void accept(KeyEvent event) {
        keyEvents++;
        lastKeyCode = event.getKeyCode();
        lastKeyFlags = event.getFlags();
        if (lastKeyCode == KEYCODE_APP_SWITCH) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) recentsPending = true;
        } else if (lastKeyCode == KeyEvent.KEYCODE_BACK ||
                (lastKeyCode == KeyEvent.KEYCODE_HOME && !event.isCanceled())) {
            activity.dispatchKeyEvent(event);
        }
    }

    public boolean consumeRecents() {
        boolean pending = recentsPending;
        recentsPending = false;
        return pending;
    }

    public int keyEvents() { return keyEvents; }
    public int lastKeyCode() { return lastKeyCode; }
    public int lastKeyFlags() { return lastKeyFlags; }

    public static long buttonCenter(View button) {
        if (button == null || !button.isShown() || button.getWidth() <= 0 || button.getHeight() <= 0)
            return 0;
        int[] location = new int[2];
        button.getLocationInWindow(location);
        int x = location[0] + button.getWidth() / 2;
        int y = location[1] + button.getHeight() / 2;
        return ((long) y << 32) | (x & 0xffffffffL);
    }

    public void close() {
        try {
            if (overlay != null) {
                ((FrameLayout) activity.getWindow().getDecorView()).removeView(overlay);
                overlay = null;
            }
        } finally {
            resources.close();
        }
    }
}
