package org.wasmandroid.tests.systemui;

import android.app.Activity;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.IOException;

public final class RenderActivity extends Activity {
    private static String apk;
    private static String platform;
    private static Drawable background;
    private Resources systemUi;
    private View navigation;

    public static void configure(String systemUiApk, String platformResources, Drawable scrim) {
        apk = systemUiApk;
        platform = platformResources;
        background = scrim;
    }

    private int id(String type, String name) {
        int id = systemUi.getIdentifier(name, type, "com.android.systemui");
        if (id == 0) throw new IllegalStateException("Missing SystemUI resource " + type + "/" + name);
        return id;
    }

    @Override
    protected void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        try {
            systemUi = Resources.forApk(new String[] {platform, apk}, "en-US", 160);
        } catch (IOException error) {
            throw new IllegalStateException(error);
        }
        Resources.Theme theme = systemUi.newTheme();
        theme.applyStyle(android.R.style.Theme_DeviceDefault, true);
        theme.applyStyle(id("style", "DualToneLightTheme"), true);
        ContextThemeWrapper context = new ContextThemeWrapper(this, theme) {
            @Override public Resources getResources() { return systemUi; }
        };
        LayoutInflater inflater = LayoutInflater.from(context);
        FrameLayout root = new FrameLayout(context);
        root.setBackground(background);

        TextView battery = (TextView) inflater.inflate(id("layout", "battery_percentage_view"), root, false);
        battery.setText(systemUi.getString(id("string", "accessibility_battery_level"), 73));
        root.addView(battery, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP));

        LinearLayout status = new LinearLayout(context);
        inflater.inflate(id("layout", "status_bar_wifi_group_inner"), status, true);
        ImageView signal = status.findViewById(id("id", "wifi_signal"));
        signal.setImageDrawable(systemUi.getDrawable(id("drawable", "ic_signal_wifi_off"), theme));
        status.findViewById(id("id", "inout_container")).setVisibility(View.GONE);
        root.addView(status, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, 32, Gravity.TOP | Gravity.END));

        // This real status bar include instantiates the Kotlin user-switcher and its launch delegate.
        inflater.inflate(id("layout", "status_bar_user_chip_container"), root, false);

        navigation = inflater.inflate(id("layout", "navigation_layout"), root, false);
        root.addView(navigation, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 48, Gravity.BOTTOM));
        LinearLayout icons = navigation.findViewById(id("id", "center_group"));
        int width = systemUi.getDimensionPixelSize(id("dimen", "navigation_key_width"));
        // NavigationBarView normally assigns these drawables to its button views.
        // Exercise the resource renderer and keep the fixture's composition out of the APK.
        for (String name : new String[] {"ic_sysbar_back", "ic_sysbar_home", "ic_sysbar_recent"}) {
            ImageView icon = new ImageView(context);
            Drawable drawable = systemUi.getDrawable(id("drawable", name), theme);
            icon.setImageDrawable(drawable);
            icon.setScaleType(ImageView.ScaleType.CENTER);
            icons.addView(icon, new LinearLayout.LayoutParams(width, ViewGroup.LayoutParams.MATCH_PARENT));
        }

        View handle = inflater.inflate(id("layout", "home_handle"), root, false);
        int handleWidth = systemUi.getDimensionPixelSize(id("dimen", "navigation_home_handle_width"));
        root.addView(handle, new FrameLayout.LayoutParams(handleWidth, 48, Gravity.CENTER));
        setContentView(root);
    }

    public static int navigationWidth() {
        RenderActivity activity = (RenderActivity) android.app.UiRuntime.getActivity();
        View touchFrame = activity.navigation.findViewById(activity.id("id", "nav_buttons"));
        if (!touchFrame.getClass().getName().equals(
                "com.android.systemui.navigationbar.views.buttons.NearestTouchFrame")) {
            throw new IllegalStateException("Expected upstream NearestTouchFrame");
        }
        return touchFrame.getWidth();
    }

    public static int contentHeight() {
        RenderActivity activity = (RenderActivity) android.app.UiRuntime.getActivity();
        return ((View) activity.navigation.getParent()).getHeight();
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
