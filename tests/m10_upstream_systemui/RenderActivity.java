package org.wasmandroid.tests.systemui;

import android.app.Activity;
import android.app.UiRuntime;
import android.content.Context;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.view.ContextThemeWrapper;
import android.view.KeyEvent;
import android.view.View;
import android.widget.FrameLayout;

import java.io.IOException;
import java.util.ArrayList;
import java.util.function.Consumer;

public final class RenderActivity extends Activity {
    private static String apk;
    private static String platform;
    private static Drawable background;
    private Resources systemUi;
    private Context systemUiContext;
    private FrameLayout host;
    private final ArrayList<KeyEvent> keys = new ArrayList<>();
    private final int[] location = new int[2];

    public static void configure(String systemUiApk, String platformResources, Drawable scrim) {
        apk = systemUiApk;
        platform = platformResources;
        background = scrim;
    }

    @Override
    protected void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        getWindow().setDecorFitsSystemWindows(false);
        try {
            systemUi = Resources.forApk(new String[] {platform, apk}, "en-US", 160);
        } catch (IOException error) {
            throw new IllegalStateException(error);
        }
        Resources.Theme theme = systemUi.newTheme();
        theme.applyStyle(android.R.style.Theme_DeviceDefault, true);
        systemUiContext = new ContextThemeWrapper(this, theme) {
            @Override public Resources getResources() { return systemUi; }
        };
        host = new FrameLayout(systemUiContext);
        host.setBackground(background);
        setContentView(host);
    }

    private static RenderActivity activity() {
        return (RenderActivity) UiRuntime.getActivity();
    }

    public static FrameLayout host() {
        return activity().host;
    }

    public static Context systemUiContext() {
        return activity().systemUiContext;
    }

    public static Consumer<KeyEvent> keyEventListener() {
        return activity()::dispatchKeyEvent;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        keys.add(event);
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        keys.add(event);
        return true;
    }

    public static int keyCount() {
        return activity().keys.size();
    }

    public static KeyEvent keyAt(int index) {
        return activity().keys.get(index);
    }

    public static void locate(View button) {
        int[] xy = activity().location;
        button.getLocationInWindow(xy);
        xy[0] += button.getWidth() / 2;
        xy[1] += button.getHeight() / 2;
    }

    public static int tapX() {
        return activity().location[0];
    }

    public static int tapY() {
        return activity().location[1];
    }

    @Override
    protected void onDestroy() {
        try {
            if (systemUi != null) systemUi.close();
            background = null;
        } finally {
            super.onDestroy();
        }
    }
}
