package android.app;

import android.content.ComponentName;
import android.content.ComponentCallbacks2;
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
import android.view.EmbeddedWindow;
import android.view.Window;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import java.util.ArrayList;
import java.util.function.Consumer;

public class Activity extends ContextThemeWrapper implements KeyEvent.Callback, ComponentCallbacks2 {
    private Intent intent;
    private EmbeddedWindow window;
    private ViewRootImpl root;
    private ComponentName component;
    private CharSequence title;
    private boolean finishing, started, resumed, destroyed;
    private final ArrayList<Application.ActivityLifecycleCallbacks> lifecycleCallbacks = new ArrayList<>();
    private final BackDispatcher backDispatcher = new BackDispatcher();
    public static final int RESULT_CANCELED = 0, RESULT_OK = -1;
    public static final int DEFAULT_KEYS_DISABLE = 0, DEFAULT_KEYS_DIALER = 1, DEFAULT_KEYS_SHORTCUT = 2,
            DEFAULT_KEYS_SEARCH_LOCAL = 3, DEFAULT_KEYS_SEARCH_GLOBAL = 4;
    private int defaultKeyMode;
    private int requestedOrientation = -1;

    public Activity() {}

    final void attach(Context context, Intent intent, int theme) {
        attachBaseContext(context);
        this.intent = intent;
        component = intent.getComponent();
        if (theme != 0) setTheme(theme);
        window = new EmbeddedWindow(this);
        try (TypedArray a = obtainStyledAttributes(new int[] {android.R.attr.windowBackground})) {
            if (a.hasValue(0)) window.setBackgroundDrawable(a.getDrawable(0));
        }
        root = new ViewRootImpl(window.getDecorView(), window);
    }

    protected void onCreate(Bundle state) {
        getApplication().dispatchActivityCreated(this, state);
        dispatchLifecycle(callback -> callback.onActivityCreated(this, state));
    }
    protected void onStart() {
        getApplication().dispatchActivityStarted(this);
        dispatchLifecycle(callback -> callback.onActivityStarted(this));
    }
    protected void onResume() {
        getApplication().dispatchActivityResumed(this);
        dispatchLifecycle(callback -> callback.onActivityResumed(this));
    }
    protected void onPause() {
        dispatchLifecycle(callback -> callback.onActivityPaused(this));
        getApplication().dispatchActivityPaused(this);
    }
    protected void onStop() {
        dispatchLifecycle(callback -> callback.onActivityStopped(this));
        getApplication().dispatchActivityStopped(this);
    }
    protected void onDestroy() {
        dispatchLifecycle(callback -> callback.onActivityDestroyed(this));
        getApplication().dispatchActivityDestroyed(this);
    }
    protected void onRestart() {}
    protected void onNewIntent(Intent intent) {}
    @Override public void onConfigurationChanged(android.content.res.Configuration configuration) {}
    @Override public void onLowMemory() {}
    @Override public void onTrimMemory(int level) {}
    protected void onSaveInstanceState(Bundle state) {}
    protected void onRestoreInstanceState(Bundle state) {}
    public void onContentChanged() {}
    public void onWindowFocusChanged(boolean focused) {}

    public void registerActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks callback) {
        lifecycleCallbacks.add(callback);
    }
    public void unregisterActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks callback) {
        lifecycleCallbacks.remove(callback);
    }
    private void dispatchLifecycle(Consumer<Application.ActivityLifecycleCallbacks> event) {
        if (lifecycleCallbacks.isEmpty()) return;
        for (var callback : new ArrayList<>(lifecycleCallbacks)) event.accept(callback);
    }
    final void performCreate(Bundle state) {
        Application app = getApplication();
        app.dispatchActivityPreCreated(this, state);
        dispatchLifecycle(callback -> callback.onActivityPreCreated(this, state));
        onCreate(state);
        dispatchLifecycle(callback -> callback.onActivityPostCreated(this, state));
        app.dispatchActivityPostCreated(this, state);
        if (!finishing) performResume();
    }
    final void performResume() {
        if (destroyed || finishing || resumed) return;
        Application app = getApplication();
        if (!started) {
            if (stopped) onRestart();
            started = true;
            app.dispatchActivityPreStarted(this);
            dispatchLifecycle(callback -> callback.onActivityPreStarted(this));
            onStart();
            dispatchLifecycle(callback -> callback.onActivityPostStarted(this));
            app.dispatchActivityPostStarted(this);
        }
        if (finishing) return;
        resumed = true;
        app.dispatchActivityPreResumed(this);
        dispatchLifecycle(callback -> callback.onActivityPreResumed(this));
        onResume();
        dispatchLifecycle(callback -> callback.onActivityPostResumed(this));
        app.dispatchActivityPostResumed(this);
        root.setWindowFocus(true);
        onWindowFocusChanged(true);
    }
    final void performPause() {
        if (!resumed) return;
        resumed = false;
        Application app = getApplication();
        app.dispatchActivityPrePaused(this);
        dispatchLifecycle(callback -> callback.onActivityPrePaused(this));
        root.setWindowFocus(false);
        onWindowFocusChanged(false);
        onPause();
        dispatchLifecycle(callback -> callback.onActivityPostPaused(this));
        app.dispatchActivityPostPaused(this);
    }
    private boolean stopped;
    final void performStop() {
        performPause();
        if (!started) return;
        started = false;
        stopped = true;
        Application app = getApplication();
        app.dispatchActivityPreStopped(this);
        dispatchLifecycle(callback -> callback.onActivityPreStopped(this));
        onStop();
        dispatchLifecycle(callback -> callback.onActivityPostStopped(this));
        app.dispatchActivityPostStopped(this);
    }
    final void performDestroy() {
        if (destroyed) return;
        try {
            performStop();
            destroyed = true;
            Application app = getApplication();
            app.dispatchActivityPreDestroyed(this);
            dispatchLifecycle(callback -> callback.onActivityPreDestroyed(this));
            onDestroy();
            dispatchLifecycle(callback -> callback.onActivityPostDestroyed(this));
            app.dispatchActivityPostDestroyed(this);
        } finally {
            if (root != null) root.detach();
        }
    }

    public Window getWindow() { return window; }
    public OnBackInvokedDispatcher getOnBackInvokedDispatcher() { return backDispatcher; }
    final void performBackPressed() {
        OnBackInvokedCallback callback = backDispatcher.getTopCallback();
        if (callback == null) onBackPressed();
        else callback.onBackInvoked();
    }

    private static final class BackDispatcher implements OnBackInvokedDispatcher {
        private final ArrayList<OnBackInvokedCallback> callbacks = new ArrayList<>();
        private final ArrayList<Integer> priorities = new ArrayList<>();
        @Override public void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback) {
            if (callback == null) throw new NullPointerException("callback");
            unregisterOnBackInvokedCallback(callback);
            int i = callbacks.size();
            while (i > 0 && priorities.get(i - 1) > priority) i--;
            callbacks.add(i, callback);
            priorities.add(i, priority);
        }
        @Override public void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback) {
            int i = callbacks.indexOf(callback);
            if (i >= 0) {
                callbacks.remove(i);
                priorities.remove(i);
            }
        }
        OnBackInvokedCallback getTopCallback() {
            return callbacks.isEmpty() ? null : callbacks.get(callbacks.size() - 1);
        }
    }
    public final Application getApplication() {
        Context context = getApplicationContext();
        return context instanceof Application application ? application : null;
    }
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
    public boolean isChangingConfigurations() { return false; }
    public boolean isDestroyed() { return destroyed; }
    public void setRequestedOrientation(int orientation) { requestedOrientation = orientation; }
    public int getRequestedOrientation() { return requestedOrientation; }
    public boolean hasWindowFocus() { return root != null && root.hasWindowFocus(); }
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
    public final void setDefaultKeyMode(int mode) {
        if (mode < DEFAULT_KEYS_DISABLE || mode > DEFAULT_KEYS_SEARCH_GLOBAL) throw new IllegalArgumentException();
        defaultKeyMode = mode;
    }
    public boolean onSearchRequested() {
        SearchManager search = getSystemService(SearchManager.class);
        ComponentName target = search == null ? null : search.getGlobalSearchActivity();
        if (target == null) return false;
        startActivity(new Intent(SearchManager.INTENT_ACTION_GLOBAL_SEARCH).setComponent(target));
        return true;
    }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        return keyCode == KeyEvent.KEYCODE_BACK || keyCode == KeyEvent.KEYCODE_SEARCH
                && (defaultKeyMode == DEFAULT_KEYS_SEARCH_LOCAL || defaultKeyMode == DEFAULT_KEYS_SEARCH_GLOBAL)
                && onSearchRequested();
    }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && !event.isCanceled()) { onBackPressed(); return true; }
        return false;
    }
    @Override public boolean onKeyLongPress(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyMultiple(int keyCode, int count, KeyEvent event) { return false; }
    public void onBackPressed() { finish(); }
}
