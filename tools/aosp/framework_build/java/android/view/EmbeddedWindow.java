package android.view;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.util.DisplayMetrics;
import android.view.animation.Interpolator;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.window.OnBackInvokedDispatcher;
import java.util.LinkedHashSet;
import java.util.Set;

/** Window policy for an activity drawing into a host SurfaceFlinger layer. */
public final class EmbeddedWindow extends Window {
    private final Activity context;
    private final FrameLayout decor, content;
    private boolean decorFitsSystemWindows = true;
    private int statusBarColor, navigationBarColor;
    private int volumeControlStream = android.media.AudioManager.USE_DEFAULT_STREAM_TYPE;
    private boolean statusBarContrastEnforced, navigationBarContrastEnforced;
    private final WindowInsetsController insetsController = new WindowInsetsController() {
        private int appearance;
        private int resourceAppearance;
        private int appearanceMask;
        private int behavior = BEHAVIOR_DEFAULT;
        private final Set<OnControllableInsetsChangedListener> listeners = new LinkedHashSet<>();

        @Override public void show(int types) {
            if ((types & WindowInsets.Type.ime()) == 0) return;
            var ime = context.getSystemService(InputMethodManager.class);
            View focused = getCurrentFocus();
            if (ime != null && focused != null) ime.showSoftInput(focused, InputMethodManager.SHOW_IMPLICIT);
        }
        @Override public void hide(int types) {
            if ((types & WindowInsets.Type.ime()) == 0) return;
            var ime = context.getSystemService(InputMethodManager.class);
            if (ime != null) ime.hideSoftInputFromWindow(decor.getWindowToken(), 0);
        }
        @Override public void controlWindowInsetsAnimation(int types, long durationMillis,
                Interpolator interpolator, CancellationSignal signal,
                WindowInsetsAnimationControlListener listener) {
            // The browser IME owns its animation timing.
            listener.onCancelled(null);
        }
        @Override public void setSystemDrivenInsetsAnimationLoggingListener(
                WindowInsetsAnimationControlListener listener) {}
        @Override public void setSystemBarsAppearance(int value, int mask) {
            appearance = (appearance & ~mask) | (value & mask);
            appearanceMask |= mask;
            updateSystemBars();
        }
        @Override public void setSystemBarsAppearanceFromResource(int value, int mask) {
            resourceAppearance = (resourceAppearance & ~mask) | (value & mask);
            updateSystemBars();
        }
        @Override public int getSystemBarsAppearance() {
            return appearance | (resourceAppearance & ~appearanceMask);
        }
        @Override public void setSystemBarsBehavior(int value) { behavior = value; }
        @Override public int getSystemBarsBehavior() { return behavior; }
        @Override public void setAnimationsDisabled(boolean disable) {}
        @Override public int getRequestedVisibleTypes() {
            View focused = getCurrentFocus();
            var ime = context.getSystemService(InputMethodManager.class);
            return WindowInsets.Type.defaultVisible()
                    | (focused != null && ime != null && ime.isActive(focused) ? WindowInsets.Type.ime() : 0);
        }
        @Override public void addOnControllableInsetsChangedListener(
                OnControllableInsetsChangedListener listener) {
            if (listeners.add(listener)) listener.onControllableInsetsChanged(this, WindowInsets.Type.ime());
        }
        @Override public void removeOnControllableInsetsChangedListener(
                OnControllableInsetsChangedListener listener) {
            listeners.remove(listener);
        }
    };
    public EmbeddedWindow(Activity context) {
        super(context);
        this.context = context;
        decor = new FrameLayout(context);
        content = new FrameLayout(context);
        WindowManager manager = new WindowManager() {
            @Override public Display getDefaultDisplay() { return context.getDisplay(); }
            @Override public WindowMetrics getCurrentWindowMetrics() { return displayMetrics(context); }
            @Override public WindowMetrics getMaximumWindowMetrics() { return getCurrentWindowMetrics(); }
            @Override public void addView(View view, ViewGroup.LayoutParams params) { decor.addView(view, params); }
            @Override public void updateViewLayout(View view, ViewGroup.LayoutParams params) { view.setLayoutParams(params); }
            @Override public void removeView(View view) { decor.removeView(view); }
        };
        setWindowManager(manager, null, null);
        content.setId(android.R.id.content);
        decor.addView(content, new FrameLayout.LayoutParams(-1, -1));
    }
    public View getDecorView() { return decor; }
    public View peekDecorView() { return decor; }
    public void setBackgroundDrawable(Drawable drawable) { decor.setBackground(drawable); }
    public void setBackgroundDrawableResource(int resId) {
        if (resId == android.R.color.transparent) setBackgroundDrawable(null);
        else decor.setBackgroundResource(resId);
    }
    @Override public boolean requestFeature(int feature) {
        return feature == FEATURE_NO_TITLE && super.requestFeature(feature);
    }
    public WindowInsetsController getInsetsController() { return insetsController; }
    public void setDecorFitsSystemWindows(boolean fit) {
        decorFitsSystemWindows = fit;
        decor.requestApplyInsets();
    }
    @Override public boolean decorFitsSystemWindows() { return decorFitsSystemWindows; }
    public void setStatusBarColor(int color) { statusBarColor = color; updateSystemBars(); }
    public int getStatusBarColor() { return statusBarColor; }
    public void setNavigationBarColor(int color) { navigationBarColor = color; updateSystemBars(); }
    public int getNavigationBarColor() { return navigationBarColor; }
    public void setStatusBarContrastEnforced(boolean enforced) { statusBarContrastEnforced = enforced; }
    public boolean isStatusBarContrastEnforced() { return statusBarContrastEnforced; }
    public void setNavigationBarContrastEnforced(boolean enforced) { navigationBarContrastEnforced = enforced; }
    public boolean isNavigationBarContrastEnforced() { return navigationBarContrastEnforced; }
    /** Publishes the bottom navigation inset of the system bar mounted on this display. */
    public void setNavigationBarInset(int pixels) {
        Display display = context.getDisplay();
        if (display.getNavigationBarInset() == pixels) return;
        display.setNavigationBarInset(pixels);
        decor.requestApplyInsets();
    }
    private static WindowInsets makeInsets(Context context) {
        Resources resources = context.getResources();
        float density = resources.getDisplayMetrics().density;
        Display display = context.getDisplay();
        int navigation = display.getNavigationBarInset();
        if (navigation < 0) {
            // Activity creation can query metrics before the system bar has been mounted.
            int id = resources.getIdentifier("navigation_bar_height", "dimen", "android");
            navigation = id == 0 ? (int) (22 * density + 0.5f) : resources.getDimensionPixelSize(id);
        }
        return makeInsets((int) (24 * density + 0.5f), navigation, navigation);
    }
    private static WindowInsets makeInsets(int status, int navigation, int gesture) {
        var insets = new WindowInsets.Builder();
        setInset(insets, WindowInsets.Type.statusBars() | WindowInsets.Type.tappableElement(),
                android.graphics.Insets.of(0, status, 0, 0));
        setInset(insets, WindowInsets.Type.navigationBars(),
                android.graphics.Insets.of(0, 0, 0, navigation));
        setInset(insets, WindowInsets.Type.systemGestures() | WindowInsets.Type.mandatorySystemGestures(),
                android.graphics.Insets.of(0, 0, 0, gesture));
        return insets.build();
    }
    private static void setInset(WindowInsets.Builder builder, int types, android.graphics.Insets inset) {
        builder.setInsets(types, inset)
                .setInsetsIgnoringVisibility(types, inset)
                .setVisible(types, !android.graphics.Insets.NONE.equals(inset));
    }
    public static WindowMetrics displayMetrics(Context context) {
        DisplayMetrics metrics = context.getResources().getDisplayMetrics();
        return new WindowMetrics(new Rect(0, 0, metrics.widthPixels, metrics.heightPixels),
                makeInsets(context), metrics.density);
    }
    WindowInsets makeInsets() { return makeInsets(context); }
    void applyLegacySystemBarAppearance(int visibility) {
        int status = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS;
        int navigation = WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
        int appearance = (visibility & View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR) != 0 ? status : 0;
        if ((visibility & View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR) != 0) appearance |= navigation;
        insetsController.setSystemBarsAppearance(appearance, status | navigation);
    }
    void applyInsets() {
        WindowInsets insets = makeInsets();
        android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
        View appRoot = content.getChildAt(0);
        int layout = decor.getSystemUiVisibility() | (appRoot == null ? 0 : appRoot.getSystemUiVisibility());
        boolean fitTop = decorFitsSystemWindows && (layout & View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN) == 0;
        boolean fitBottom = decorFitsSystemWindows && (layout & View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION) == 0;
        decor.setPadding(0, fitTop ? bars.top : 0, 0, fitBottom ? bars.bottom : 0);
        decor.dispatchApplyWindowInsets(makeInsets(fitTop ? 0 : bars.top,
                fitBottom ? 0 : bars.bottom, bars.bottom));
    }
    private void updateSystemBars() {
        nativeSetSystemBars(insetsController.getSystemBarsAppearance(), statusBarColor, navigationBarColor);
    }
    private static native void nativeSetSystemBars(int appearance, int statusColor, int navigationColor);
    public void setContentView(int resource) {
        content.removeAllViews();
        LayoutInflater.from(context).inflate(resource, content, true);
    }
    public void setContentView(View view) { setContentView(view, new FrameLayout.LayoutParams(-1, -1)); }
    public void setContentView(View view, ViewGroup.LayoutParams params) {
        content.removeAllViews();
        content.addView(view, params);
    }
    public void addContentView(View view, ViewGroup.LayoutParams params) { content.addView(view, params); }
    public View getCurrentFocus() { return decor.findFocus(); }
    @Override public LayoutInflater getLayoutInflater() { return LayoutInflater.from(context); }
    @Override public void clearContentView() { content.removeAllViews(); }
    @Override public OnBackInvokedDispatcher getOnBackInvokedDispatcher() {
        return context.getOnBackInvokedDispatcher();
    }

    @Override public boolean superDispatchKeyEvent(KeyEvent event) { return decor.dispatchKeyEvent(event); }
    @Override public boolean superDispatchTouchEvent(MotionEvent event) { return decor.dispatchTouchEvent(event); }
    @Override public boolean superDispatchGenericMotionEvent(MotionEvent event) {
        return decor.dispatchGenericMotionEvent(event);
    }
    @Override public boolean superDispatchKeyShortcutEvent(KeyEvent event) { return false; }
    @Override public boolean superDispatchTrackballEvent(MotionEvent event) { return false; }
    @Override public boolean isShortcutKey(int keyCode, KeyEvent event) { return false; }
    @Override public void setTitle(CharSequence title) { getAttributes().setTitle(title); }
    @Override public void setTitleColor(int color) {}
    @Override public int getVolumeControlStream() { return volumeControlStream; }
    @Override public void setVolumeControlStream(int streamType) { volumeControlStream = streamType; }
    @Override public boolean isFloating() { return false; }
    @Override public void takeSurface(SurfaceHolder.Callback2 callback) {}
    @Override public void takeInputQueue(InputQueue.Callback callback) {}
    @Override public void takeKeyEvents(boolean get) { decor.setFocusable(get); }
    @Override public void alwaysReadCloseOnTouchAttr() {}
    @Override public void openPanel(int featureId, KeyEvent event) {}
    @Override public void closePanel(int featureId) {}
    @Override public void togglePanel(int featureId, KeyEvent event) {}
    @Override public void invalidatePanelMenu(int featureId) {}
    @Override public boolean performPanelShortcut(int featureId, int keyCode, KeyEvent event, int flags) {
        return false;
    }
    @Override public boolean performPanelIdentifierAction(int featureId, int id, int flags) { return false; }
    @Override public void closeAllPanels() {}
    @Override public boolean performContextMenuIdentifierAction(int id, int flags) { return false; }
    @Override public void onConfigurationChanged(Configuration newConfig) { decor.requestApplyInsets(); }
    @Override public void setFeatureDrawableResource(int featureId, int resId) {}
    @Override public void setFeatureDrawableUri(int featureId, Uri uri) {}
    @Override public void setFeatureDrawable(int featureId, Drawable drawable) {}
    @Override public void setFeatureDrawableAlpha(int featureId, int alpha) {}
    @Override public void setFeatureInt(int featureId, int value) {}
    @Override public void setChildDrawable(int featureId, Drawable drawable) {}
    @Override public void setChildInt(int featureId, int value) {}
    @Override public Bundle saveHierarchyState() { return new Bundle(); }
    @Override public void restoreHierarchyState(Bundle savedInstanceState) {}
    @Override protected void onActive() {}
    @Override public void setDecorCaptionShade(int decorCaptionShade) {}
    @Override public void setResizingCaptionDrawable(Drawable drawable) {}
    @Override public void onMultiWindowModeChanged() {}
    @Override public void onPictureInPictureModeChanged(boolean isInPictureInPictureMode) {}
}
