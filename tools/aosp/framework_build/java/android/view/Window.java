package android.view;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.widget.FrameLayout;

public class Window {
    public static final int FEATURE_NO_TITLE = 1, FEATURE_ACTION_BAR = 8;
    private final Context context;
    private final FrameLayout decor, content;
    private final WindowManager.LayoutParams attributes = new WindowManager.LayoutParams();
    private boolean decorFitsSystemWindows = true;
    private int statusBarColor, navigationBarColor;
    private boolean statusBarContrastEnforced, navigationBarContrastEnforced;
    private final WindowInsetsController insetsController = new WindowInsetsController() {
        private int appearance;
        @Override public void setSystemBarsAppearance(int value, int mask) {
            appearance = (appearance & ~mask) | (value & mask);
            updateSystemBars();
        }
        @Override public int getSystemBarsAppearance() { return appearance; }
    };
    private final WindowManager manager = new WindowManager() {
        @Override public WindowMetrics getCurrentWindowMetrics() {
            DisplayMetrics metrics = context.getResources().getDisplayMetrics();
            return new WindowMetrics(new Rect(0, 0, metrics.widthPixels, metrics.heightPixels),
                    makeInsets(), metrics.density);
        }
        @Override public WindowMetrics getMaximumWindowMetrics() { return getCurrentWindowMetrics(); }
        @Override public void addView(View view, android.view.ViewGroup.LayoutParams params) { decor.addView(view, params); }
        @Override public void updateViewLayout(View view, android.view.ViewGroup.LayoutParams params) { view.setLayoutParams(params); }
        @Override public void removeView(View view) { decor.removeView(view); }
    };
    public Window(Context context) {
        this.context = context;
        decor = new FrameLayout(context);
        content = new FrameLayout(context);
        content.setId(android.R.id.content);
        decor.addView(content, new FrameLayout.LayoutParams(-1, -1));
    }
    public Context getContext() { return context; }
    public View getDecorView() { return decor; }
    public View peekDecorView() { return decor; }
    public FrameLayout getContentParent() { return content; }
    public void setBackgroundDrawable(Drawable drawable) { decor.setBackground(drawable); }
    public void setBackgroundDrawableResource(int resId) {
        if (resId == android.R.color.transparent) setBackgroundDrawable(null);
        else decor.setBackgroundResource(resId);
    }
    public boolean requestFeature(int feature) { return feature == FEATURE_NO_TITLE; }
    public WindowManager.LayoutParams getAttributes() { return attributes; }
    public WindowManager getWindowManager() { return manager; }
    public WindowInsetsController getInsetsController() { return insetsController; }
    public void addFlags(int flags) { setFlags(flags, flags); }
    public void clearFlags(int flags) { setFlags(0, flags); }
    public void setFlags(int flags, int mask) { attributes.flags = (attributes.flags & ~mask) | (flags & mask); }
    public void setSoftInputMode(int mode) { attributes.softInputMode = mode; }
    public void setDecorFitsSystemWindows(boolean fit) {
        decorFitsSystemWindows = fit;
        decor.requestApplyInsets();
    }
    public void setStatusBarColor(int color) { statusBarColor = color; updateSystemBars(); }
    public int getStatusBarColor() { return statusBarColor; }
    public void setNavigationBarColor(int color) { navigationBarColor = color; updateSystemBars(); }
    public int getNavigationBarColor() { return navigationBarColor; }
    public void setStatusBarContrastEnforced(boolean enforced) { statusBarContrastEnforced = enforced; }
    public boolean isStatusBarContrastEnforced() { return statusBarContrastEnforced; }
    public void setNavigationBarContrastEnforced(boolean enforced) { navigationBarContrastEnforced = enforced; }
    public boolean isNavigationBarContrastEnforced() { return navigationBarContrastEnforced; }
    WindowInsets makeInsets() {
        float density = context.getResources().getDisplayMetrics().density;
        return new WindowInsets((int) (24 * density + 0.5f), (int) (22 * density + 0.5f));
    }
    void applyInsets() {
        WindowInsets insets = makeInsets();
        android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
        decor.setPadding(0, decorFitsSystemWindows ? bars.top : 0,
                0, decorFitsSystemWindows ? bars.bottom : 0);
        decor.dispatchApplyWindowInsets(decorFitsSystemWindows ? WindowInsets.NONE : insets);
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
    public <T extends View> T findViewById(int id) { return decor.findViewById(id); }
    public View getCurrentFocus() { return decor.findFocus(); }
}
