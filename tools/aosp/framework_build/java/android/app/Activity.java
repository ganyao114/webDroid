package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.res.TypedArray;
import android.os.Bundle;
import android.view.ContextThemeWrapper;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;
import android.view.Window;

public class Activity extends ContextThemeWrapper implements KeyEvent.Callback {
    private Intent intent;
    private Window window;
    private ViewRootImpl root;
    private ComponentName component;
    private CharSequence title;
    private boolean finishing, started, resumed, destroyed;
    public static final int RESULT_CANCELED = 0, RESULT_OK = -1;

    public Activity() {}

    final void attach(Context context, Intent intent, int theme) {
        attachBaseContext(context);
        this.intent = intent;
        component = intent.getComponent();
        if (theme != 0) setTheme(theme);
        window = new Window(this);
        try (TypedArray a = obtainStyledAttributes(new int[] {android.R.attr.windowBackground})) {
            if (a.hasValue(0)) window.setBackgroundDrawable(a.getDrawable(0));
        }
        root = new ViewRootImpl(window.getDecorView(), window);
    }

    protected void onCreate(Bundle state) {}
    protected void onStart() {}
    protected void onResume() {}
    protected void onPause() {}
    protected void onStop() {}
    protected void onDestroy() {}
    protected void onRestart() {}
    protected void onNewIntent(Intent intent) {}
    public void onConfigurationChanged(android.content.res.Configuration configuration) {}
    protected void onSaveInstanceState(Bundle state) {}
    protected void onRestoreInstanceState(Bundle state) {}
    public void onContentChanged() {}
    public void onWindowFocusChanged(boolean focused) {}

    final void performCreate(Bundle state) {
        onCreate(state);
        if (!finishing) performResume();
    }
    final void performResume() {
        if (destroyed || finishing || resumed) return;
        if (!started) {
            if (stopped) onRestart();
            started = true;
            onStart();
        }
        if (finishing) return;
        resumed = true;
        onResume();
        onWindowFocusChanged(true);
    }
    final void performPause() {
        if (!resumed) return;
        resumed = false;
        onWindowFocusChanged(false);
        onPause();
    }
    private boolean stopped;
    final void performStop() {
        performPause();
        if (!started) return;
        started = false;
        stopped = true;
        onStop();
    }
    final void performDestroy() {
        if (destroyed) return;
        try {
            performStop();
            destroyed = true;
            onDestroy();
        } finally {
            if (root != null) root.detach();
        }
    }

    public Window getWindow() { return window; }
    public android.view.WindowManager getWindowManager() { return window.getWindowManager(); }
    @Override public Object getSystemService(String name) {
        if (WINDOW_SERVICE.equals(name) && window != null) return getWindowManager();
        return super.getSystemService(name);
    }
    public LayoutInflater getLayoutInflater() { return LayoutInflater.from(this); }
    public ComponentName getComponentName() { return component; }
    public String getLocalClassName() { return component.getShortClassName(); }
    public Intent getIntent() { return intent; }
    public void setIntent(Intent intent) { this.intent = intent; }
    public void setContentView(int resource) { window.setContentView(resource); onContentChanged(); }
    public void setContentView(View view) { window.setContentView(view); onContentChanged(); }
    public void setContentView(View view, ViewGroup.LayoutParams params) { window.setContentView(view, params); onContentChanged(); }
    public void addContentView(View view, ViewGroup.LayoutParams params) { window.addContentView(view, params); onContentChanged(); }
    public <T extends View> T findViewById(int id) { return window.findViewById(id); }
    public final <T extends View> T requireViewById(int id) { return window.getDecorView().requireViewById(id); }
    public View getCurrentFocus() { return window == null ? null : window.getCurrentFocus(); }
    public void setTitle(CharSequence title) { this.title = title; }
    public void setTitle(int id) { setTitle(getText(id)); }
    public final CharSequence getTitle() { return title; }
    public final boolean requestWindowFeature(int feature) { return window.requestFeature(feature); }
    public void runOnUiThread(Runnable action) { action.run(); }
    public void finish() { finishing = true; }
    public boolean isFinishing() { return finishing; }
    public boolean isDestroyed() { return destroyed; }
    final boolean isResumed() { return resumed; }
    final ViewRootImpl viewRoot() { return root; }

    public boolean dispatchTouchEvent(MotionEvent event) { return root.dispatchTouchEvent(event) || onTouchEvent(event); }
    public boolean onTouchEvent(MotionEvent event) { return false; }
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        return root.dispatchGenericMotionEvent(event) || onGenericMotionEvent(event);
    }
    public boolean onGenericMotionEvent(MotionEvent event) { return false; }
    public boolean dispatchKeyEvent(KeyEvent event) { return root.dispatchKeyEvent(event) || event.dispatch(this); }
    public boolean dispatchImeText(CharSequence text, boolean composing) { return root.dispatchImeText(text, composing); }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) { return keyCode == KeyEvent.KEYCODE_BACK; }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && !event.isCanceled()) { onBackPressed(); return true; }
        return false;
    }
    @Override public boolean onKeyLongPress(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyMultiple(int keyCode, int count, KeyEvent event) { return false; }
    public void onBackPressed() { finish(); }
}
