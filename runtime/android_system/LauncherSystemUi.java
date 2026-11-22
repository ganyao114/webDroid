package org.wasmandroid.launcher;

import android.app.Activity;
import android.app.UiRuntime;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.graphics.Rect;
import android.view.ContextThemeWrapper;
import android.view.EmbeddedWindow;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
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
    private boolean dispatchingForegroundTouch;
    private int foregroundKey;
    private long navigationDownTime;

    public static LauncherSystemUi create(Activity activity, String systemUiApk,
            String locale, int width, int height, int density) throws IOException {
        Resources resources = activity.getResources().forApkWithFramework(systemUiApk, locale, density);
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
        ((EmbeddedWindow) activity.getWindow()).setNavigationBarInset(height);
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
        if (dispatchingForegroundTouch) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) foregroundKey = lastKeyCode;
            return;
        }
        if (lastKeyCode == KEYCODE_APP_SWITCH) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) recentsPending = true;
        } else if (lastKeyCode == KeyEvent.KEYCODE_BACK || lastKeyCode == KeyEvent.KEYCODE_HOME) {
            UiRuntime.dispatchSystemKey(event);
        }
    }

    /** Dispatches a system navigation gesture while Launcher is paused behind an app. */
    public int dispatchNavigationTouch(long time, int action, float x, float y) {
        if (action == MotionEvent.ACTION_DOWN) navigationDownTime = time;
        MotionEvent event = MotionEvent.obtain(navigationDownTime, time, action, x, y, 0);
        foregroundKey = 0;
        dispatchingForegroundTouch = true;
        try {
            overlay.dispatchTouchEvent(event);
            return foregroundKey;
        } finally {
            dispatchingForegroundTouch = false;
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
        Rect bounds = viewBounds(button);
        if (bounds == null) return 0;
        return ((long) bounds.centerY() << 32) | (bounds.centerX() & 0xffffffffL);
    }

    public static Rect viewBounds(View view) {
        if (view == null || !view.isShown() || view.getWidth() <= 0 || view.getHeight() <= 0)
            return null;
        int[] location = new int[2];
        view.getLocationInWindow(location);
        return new Rect(location[0], location[1], location[0] + view.getWidth(), location[1] + view.getHeight());
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
