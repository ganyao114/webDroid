package org.wasmandroid.uismoke;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.LauncherActivityInfo;
import android.content.pm.LauncherApps;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.VectorDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.UserHandle;
import android.text.Editable;
import android.text.Layout;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.RemoteViews;
import android.widget.TextView;
import android.window.BackEvent;
import android.window.BackMotionEvent;
import com.android.launcher3.fixture.TileView;
import java.io.IOException;
import java.net.URISyntaxException;
import java.util.HashSet;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.Collectors;
import java.util.stream.IntStream;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

public final class SmokeActivity extends Activity {
    private static SmokeActivity active;
    private static int lifecycle;
    private TextView headline;
    private TileView tile;
    private int clicks, animationUpdates, handled;
    private boolean resourceCheck, packageCheck;
    private ValueAnimator animator;

    @Override protected void onCreate(Bundle state) {
        active = this;
        record(1);
        setContentView(R.layout.activity_ui_smoke);
        headline = findViewById(R.id.headline);
        tile = findViewById(R.id.tile);
        if (!tile.requestFocus()) throw new IllegalStateException("Tile focus");
        tile.setOnClickListener(view -> {
            clicks++;
            headline.setText(R.string.clicked);
            ResolveInfo app = getPackageManager().resolveActivity(launcherIntent(), 0);
            startActivity(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                    .setClassName(app.activityInfo.packageName, app.activityInfo.name)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            animator = ValueAnimator.ofFloat(0f, 1f);
            animator.setDuration(160);
            animator.addUpdateListener(animation -> {
                animationUpdates++;
                tile.setPulse((Float) animation.getAnimatedValue());
            });
            animator.start();
        });
        Handler handler = new Handler(Looper.getMainLooper(), message -> {
            handled += message.what;
            return true;
        });
        handler.sendEmptyMessageDelayed(7, 5);
        resourceCheck = inspectResources();
        packageCheck = inspectPackages();
        inspectTextLayout();
        inspectInputAndWindow();
        inspectRemoteViews();
        inspectPreferences();
        inspectCoreLibrary();
    }
    @Override protected void onStart() { record(2); }
    @Override protected void onResume() { record(3); }
    @Override protected void onPause() { record(4); }
    @Override protected void onStop() { record(5); }
    @Override protected void onDestroy() {
        record(6);
        if (animator != null && animator.isStarted()) animator.cancel();
        active = null;
    }
    @Override protected void onRestart() { record(7); }
    private static void record(int event) { lifecycle = (int) ((lifecycle * 10L + event) % 1_000_000_000L); }

    private boolean inspectResources() {
        Resources res = getResources();
        int density = res.getDisplayMetrics().densityDpi;
        java.util.Locale locale = res.getConfiguration().getLocales().get(0);
        verify(locale != null && locale.equals(java.util.Locale.getDefault()), "configured locale");
        TextView footer = findViewById(R.id.footer);
        verify(res.getIdentifier("ready", "string", getPackageName()) == R.string.ready, "resource name lookup");
        verify(res.getInteger(R.integer.columns) == 4 && res.getBoolean(R.bool.show_tile), "integer and boolean");
        verify(res.getDimensionPixelSize(R.dimen.outer_padding) == 12 * density / 160, "density-scaled dp");
        verify(res.getDimensionPixelSize(R.dimen.fixed_px) == 11, "unscaled px");
        verify(res.getString(R.string.formatted, 1, 4).equals("Page 1 of 4"), "formatted resource");
        verify(res.getResourceEntryName(R.drawable.ic_star_vector).equals("ic_star_vector"), "resource entry name");
        verify(res.getStringArray(R.array.page_labels).length == 2, "resource string array");
        verify(headline != null && footer != null && findViewById(R.id.original_headline) == null,
                "include with overridden ID and merge");
        verify(findViewById(R.id.root) instanceof LinearLayout, "linear root");
        verify(findViewById(R.id.hidden).getVisibility() == View.GONE, "compiled visibility");
        verify(headline.getTextSize() == 18f * density / 160, "style text size " + headline.getTextSize());
        verify(headline.getCurrentTextColor() == res.getColor(R.color.heading), "style text color");
        verify(((ImageView) findViewById(R.id.star)).getDrawable() instanceof VectorDrawable, "vector drawable");
        verify(((ImageView) findViewById(R.id.icon)).getDrawable() instanceof AdaptiveIconDrawable, "adaptive drawable");
        verify(((ImageView) findViewById(R.id.pixel)).getDrawable() instanceof BitmapDrawable, "bitmap drawable");
        try (XmlResourceParser xml = res.getXml(R.xml.ui_metadata);
             TypedArray style = getTheme().obtainStyledAttributes(new int[] {R.attr.tileAccent})) {
            verify(style.getColor(0, 0) == res.getColor(R.color.accent), "theme resource");
            int event;
            do { event = xml.next(); } while (event == XmlPullParser.START_DOCUMENT);
            verify(event == XmlPullParser.START_TAG && "ui-fixture".equals(xml.getName())
                    && "aapt2".equals(xml.getAttributeValue(null, "name")), "compiled res/xml");
            return true;
        } catch (IOException | XmlPullParserException error) {
            throw new IllegalStateException("Smoke verification failed", error);
        }
    }

    private static void verify(boolean success, String description) {
        if (!success) throw new IllegalStateException(description);
    }

    private void inspectPreferences() {
        SharedPreferences preferences = getSharedPreferences("ui-smoke", MODE_PRIVATE);
        AtomicInteger notifications = new AtomicInteger();
        SharedPreferences.OnSharedPreferenceChangeListener listener =
                (source, key) -> notifications.incrementAndGet();
        preferences.registerOnSharedPreferenceChangeListener(listener);
        try {
            HashSet<String> values = new HashSet<>();
            values.add("stored");
            verify(preferences.edit().putStringSet("test", values).commit(), "Preferences commit");
            values.clear();
            preferences.getStringSet("test", null).clear();
            verify(preferences.getStringSet("test", null).contains("stored")
                    && notifications.get() == 1, "Preferences copy sets and notify listeners");
            preferences.edit().clear().apply();
            verify(preferences.getAll().isEmpty() && notifications.get() == 2,
                    "Preferences clear and notify listeners");
        } finally {
            preferences.unregisterOnSharedPreferenceChangeListener(listener);
        }
    }

    private static void inspectCoreLibrary() {
        AtomicBoolean ready = new AtomicBoolean();
        AtomicReference<String> value = new AtomicReference<>("pending");
        verify(!ready.compareAndExchange(false, true) && ready.get()
                && value.compareAndSet("pending", "ready") && "ready".equals(value.getAcquire()),
                "Libcore atomics use the interpreter's field updaters");

        CopyOnWriteArrayList<String> entries = new CopyOnWriteArrayList<>();
        entries.add("first");
        var snapshot = entries.iterator();
        entries.add("second");
        verify("first".equals(snapshot.next()) && !snapshot.hasNext(), "Copy-on-write snapshot");

        CompletableFuture<Integer> pending = new CompletableFuture<>();
        var result = pending.thenApplyAsync(number -> number + 1, Runnable::run);
        verify(pending.complete(41) && result.join() == 42, "CompletableFuture continuation");
        CountDownLatch latch = new CountDownLatch(1);
        latch.countDown();
        verify(TimeUnit.NANOSECONDS.convert(3, TimeUnit.MILLISECONDS) == 3_000_000,
                "TimeUnit conversion avoids loading optional java.time classes");
        try {
            verify(latch.await(0, TimeUnit.MILLISECONDS), "CountDownLatch reached zero");
        } catch (InterruptedException error) {
            throw new IllegalStateException(error);
        }

        var buckets = List.of("a", "b", "cc").stream()
                .collect(Collectors.groupingBy(String::length, Collectors.joining(",")));
        verify("a,b".equals(buckets.get(1)) && "cc".equals(buckets.get(2)), "Libcore collectors");
        verify(IntStream.range(0, 10).skip(3).limit(2).sum() == 7, "Libcore primitive stream");
        verify("test at index 0: %".equals(new URISyntaxException("%", "test", 0).getMessage()),
                "Libcore URI parse diagnostics");
    }

    private void inspectTextLayout() {
        TextView label = new TextView(this);
        label.setSingleLine();
        label.setHorizontallyScrolling(false);
        label.setEllipsize(TextUtils.TruncateAt.END);
        for (String text : new String[] {"Apps", "Modes", "Communal", "Search"}) {
            label.setText(text);
            int width = (int) Math.ceil(label.getPaint().measureText(text))
                    + label.getCompoundPaddingLeft() + label.getCompoundPaddingRight();
            label.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            Layout layout = label.getLayout();
            verify(layout.getLineCount() == 1 && layout.getEllipsisCount(0) == 0
                    && layout.getLineWidth(0) <= width - label.getCompoundPaddingLeft()
                            - label.getCompoundPaddingRight(),
                    "Calibrated TextView fits the full label " + text);
        }
    }

    private void inspectInputAndWindow() {
        EditText editor = new EditText(this);
        editor.setText("ab");
        editor.setSelection(1);
        verify(editor.onImeText("n", true) && editor.onImeText("ni", true)
                && editor.onImeText("你", false) && "a你b".contentEquals(editor.getText()),
                "IME replaces the composing range");
        editor.setSelection(1, 2);
        verify(editor.onKeyDown(KeyEvent.KEYCODE_DEL, new KeyEvent(KeyEvent.ACTION_DOWN,
                KeyEvent.KEYCODE_DEL)) && "ab".contentEquals(editor.getText())
                && editor.getSelectionStart() == 1, "Backspace removes the selection");
        editor.setSingleLine(false);
        editor.onKeyDown(KeyEvent.KEYCODE_ENTER, new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER));
        editor.onKeyUp(KeyEvent.KEYCODE_ENTER, new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER));
        verify("a\nb".contentEquals(editor.getText()), "Enter inserts a newline in multiline text");

        editor.setSingleLine(true);
        editor.setImeOptions(EditorInfo.IME_ACTION_DONE);
        int[] action = {EditorInfo.IME_NULL};
        editor.setOnEditorActionListener((view, actionId, event) -> {
            if (event.getAction() == KeyEvent.ACTION_UP) action[0] = actionId;
            return true;
        });
        editor.onKeyDown(KeyEvent.KEYCODE_ENTER, new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER));
        editor.onKeyUp(KeyEvent.KEYCODE_ENTER, new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER));
        verify(action[0] == EditorInfo.IME_ACTION_DONE && "a\nb".contentEquals(editor.getText()),
                "Single-line Enter dispatches the configured IME action");

        InputMethodManager ime = getSystemService(InputMethodManager.class);
        verify(!ime.showSoftInput(editor, InputMethodManager.SHOW_IMPLICIT), "Detached editor has no IME");
        LinearLayout root = findViewById(R.id.root);
        root.addView(editor);
        verify(editor.requestFocus() && ime.showSoftInput(editor, InputMethodManager.SHOW_IMPLICIT)
                && ime.isActive(editor), "Focused embedded editor accepts soft input");
        verify(editor.getRootWindowInsets().isVisible(WindowInsets.Type.ime()), "Root reports IME visibility");
        verify(ime.hideSoftInputFromWindow(editor.getWindowToken(), 0) && !ime.isActive(editor),
                "Embedded window token hides soft input");
        root.removeView(editor);

        editor.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence text, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence text, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable text) {
                if (text.length() > 0) text.clear();
            }
        });
        editor.setSelection(1);
        verify(editor.onImeText("x", false) && editor.length() == 0 && editor.getSelectionStart() == 0,
                "IME keeps selection valid when a TextWatcher clears the buffer");
        verify(tile.requestFocus(), "Tile regains focus after editor removal");

        BackEvent back = BackEvent.fromBackMotionEvent(
                new BackMotionEvent(12f, 40f, 120L, .5f, true, BackEvent.EDGE_LEFT));
        verify(back.getTouchX() == 12f && back.getProgress() == .5f
                && back.getFrameTimeMillis() == 120L, "Predictive back preserves motion data");

        WindowManager.LayoutParams params = new WindowManager.LayoutParams();
        params.x = 12;
        params.setTitle("Embedded window");
        Parcel parcel = Parcel.obtain();
        params.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        WindowManager.LayoutParams restored = WindowManager.LayoutParams.CREATOR.createFromParcel(parcel);
        verify(restored.x == 12 && "Embedded window".contentEquals(restored.getTitle()),
                "Window layout parameters survive a parcel round trip");
        parcel.recycle();

        AccessibilityEvent event = new AccessibilityEvent(AccessibilityEvent.TYPE_VIEW_FOCUSED);
        event.setSource(tile);
        event.setSealed(true);
        AccessibilityNodeInfo source = event.getSource();
        verify(source != null && source.isClickable(), "Accessibility event resolves its local node");
    }

    private void inspectRemoteViews() {
        RemoteViews views = new RemoteViews(getPackageName(), R.layout.header);
        views.setTextViewText(R.id.original_headline, "Remote widget");
        Parcel parcel = Parcel.obtain();
        try {
            views.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            RemoteViews restored = RemoteViews.CREATOR.createFromParcel(parcel);
            TextView label = (TextView) restored.apply(this, null);
            verify("Remote widget".contentEquals(label.getText()), "RemoteViews inflates and applies text");
            restored.setTextViewText(R.id.original_headline, "Updated widget");
            restored.reapply(this, label);
            verify("Updated widget".contentEquals(label.getText()), "RemoteViews reapplies text");
        } finally {
            parcel.recycle();
        }
    }

    private static Intent launcherIntent() {
        return new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
    }
    private boolean inspectPackages() {
        PackageManager pm = getPackageManager();
        List<ResolveInfo> activities = pm.queryIntentActivities(launcherIntent(), 0);
        LauncherApps launcher = getSystemService(LauncherApps.class);
        List<LauncherActivityInfo> apps = launcher.getActivityList(null, UserHandle.of(10));
        try {
            ApplicationInfo info = pm.getApplicationInfo("org.wasmandroid.notes", 0);
            return activities.size() == 1 && apps.size() == 1
                && apps.get(0).getUser().getIdentifier() == 10
                && "Notes".equals(apps.get(0).getLabel())
                && apps.get(0).getIcon(240) instanceof VectorDrawable
                && launcher.getApplicationInfo("org.wasmandroid.notes", 0, UserHandle.of(10)).enabled
                && "Notes".equals(pm.getApplicationLabel(info))
                && pm.getApplicationIcon(info) instanceof VectorDrawable;
        } catch (PackageManager.NameNotFoundException error) {
            throw new IllegalStateException("Smoke verification failed", error);
        }
    }

    public static int iconResource() { return R.drawable.ic_star_vector; }
    public static int lifecycle() { return lifecycle; }
    public static int clickCount() { return active.clicks; }
    public static int animationUpdates() { return active.animationUpdates; }
    public static int handlerValue() { return active.handled; }
    public static boolean resourcesVerified() { return active.resourceCheck; }
    public static boolean packagesVerified() { return active.packageCheck; }
    public static boolean platformResources() {
        Resources resources = active.getResources();
        return resources.getResourcePackageName(android.R.color.white).equals("android")
                && resources.getColor(android.R.color.white) == 0xffffffff;
    }
    public static float headingAdvance() { return active.headline.getPaint().measureText("XX"); }
    public static float titleAdvance(String title) {
        return active.headline.getPaint().measureText(title);
    }
    public static float headingAscent() { return active.headline.getPaint().getFontMetrics().ascent; }
    public static int tileWidth() { return active.tile.getWidth(); }
    public static int tileHeight() { return active.tile.getHeight(); }
    public static int tileX() { return origin(active.tile, true); }
    public static int tileY() { return origin(active.tile, false); }
    public static int swatchPixel() { return active.tile.swatchPixel(); }
    public static int scroll() { return active.tile.getScrollX(); }
    public static boolean flinging() { return active.tile.isFlinging(); }
    public static int keyCount() { return active.tile.keyCount(); }
    public static int motionEvents() { return active.tile.motionEvents(); }
    public static String composed() { return active.tile.composed(); }
    public static String committed() { return active.tile.committed(); }
    private static int origin(View view, boolean horizontal) {
        int position = horizontal ? view.getLeft() : view.getTop();
        while (view.getParent() instanceof View) {
            view = (View) view.getParent();
            position += horizontal ? view.getLeft() - view.getScrollX() : view.getTop() - view.getScrollY();
        }
        return position;
    }
}
