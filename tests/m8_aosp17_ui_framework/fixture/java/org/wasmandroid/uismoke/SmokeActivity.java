package org.wasmandroid.uismoke;

import java.io.BufferedInputStream;
import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.io.RandomAccessFile;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.math.RoundingMode;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.app.Application;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
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
import android.database.Cursor;
import android.graphics.BitmapFactory;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.VectorDrawable;
import android.os.Bundle;
import android.os.ConditionVariable;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.util.AtomicFile;
import android.os.Parcel;
import android.os.ResultReceiver;
import android.os.UserHandle;
import android.text.Editable;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
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
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;
import java.net.URL;
import java.net.URISyntaxException;
import java.util.AbstractCollection;
import java.util.AbstractList;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountedCompleter;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RecursiveAction;
import java.util.concurrent.RecursiveTask;
import java.util.concurrent.Semaphore;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;
import java.util.concurrent.locks.ReentrantReadWriteLock;
import java.util.regex.Pattern;
import java.util.stream.Collectors;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.stream.IntStream;
import javax.xml.parsers.DocumentBuilderFactory;
import org.json.JSONException;
import org.json.JSONObject;
import org.w3c.dom.Element;
import org.xml.sax.SAXParseException;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

public final class SmokeActivity extends Activity {
    private static SmokeActivity active;
    private static int lifecycle;
    private static boolean shutdownHookRan;
    private static final int REFLECTED_VALUE = 17;
    private static final Object LEGACY_FIELD = new Object();
    private static int providerOrder, providerQueries;

    public static final class SmokeApplication extends Application {
        @Override protected void attachBaseContext(Context base) {
            super.attachBaseContext(base);
            providerOrder = 1;
        }

        @Override public void onCreate() {
            if (Boolean.getBoolean("org.wasmandroid.foreground") && providerOrder != 3) {
                throw new IllegalStateException("Main-process providers must finish before Application.onCreate");
            }
        }
    }

    public abstract static class TestProvider extends ContentProvider {
        @Override public Cursor query(Uri uri, String[] projection, String selection,
                String[] selectionArgs, String sortOrder) { return null; }
        @Override public String getType(Uri uri) { return null; }
        @Override public Uri insert(Uri uri, ContentValues values) { return null; }
        @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
        @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
    }

    public static final class DefaultProvider extends TestProvider {
        @Override public Cursor query(Uri uri, String[] projection, String selection,
                String[] selectionArgs, String sortOrder) {
            if (!"org.wasmandroid.uismoke.default".equals(uri.getAuthority())) {
                throw new IllegalArgumentException(uri.toString());
            }
            ++providerQueries;
            return null;
        }

        @Override public boolean onCreate() {
            if (providerOrder != 1 || !(getContext().getApplicationContext() instanceof SmokeApplication)) {
                throw new IllegalStateException("Default provider requires an attached application");
            }
            providerOrder = 2;
            return true;
        }
    }

    public static final class ExplicitProvider extends TestProvider {
        @Override public boolean onCreate() {
            if (providerOrder != 2) throw new IllegalStateException("Provider initOrder is incorrect");
            providerOrder = 3;
            return true;
        }
    }

    public static final class RemoteProvider extends TestProvider {
        @Override public boolean onCreate() {
            throw new IllegalStateException("Remote provider entered the main process");
        }
    }
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
            overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
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
    public static int verifyFileStreams() throws IOException {
        File directory = new File("/data/user/0/org.wasmandroid.uismoke/cache");
        if (!directory.mkdirs() || !directory.isDirectory()) return 0;
        File file = new File(directory, "stream-test");
        PrintStream stdout = System.out;
        if (stdout == null || System.err == null || stdout == System.err) return 0;
        try (PrintStream output = new PrintStream(new FileOutputStream(file), true, "UTF-8")) {
            System.setOut(output);
            System.out.print("caf\u00e9");
            if (System.out.checkError()) return 0;
        } finally {
            System.setOut(stdout);
        }
        try (DataOutputStream output = new DataOutputStream(new FileOutputStream(file, true))) {
            output.writeInt(0x12345678);
        }
        try (DataInputStream input = new DataInputStream(new FileInputStream(file))) {
            byte[] text = new byte[5];
            input.readFully(text);
            if (!new String(text, "UTF-8").equals("caf\u00e9") || input.readInt() != 0x12345678 || input.read() != -1) {
                return 0;
            }
        }
        try (BufferedInputStream input = new BufferedInputStream(new FileInputStream(file), 2)) {
            input.mark(9);
            if (input.readNBytes(9).length != 9) return 0;
            input.reset();
            if (input.read() != 'c' || input.skip(7) != 7 || input.read() != 0x78 || input.read() != -1) return 0;
        }
        try (RandomAccessFile input = new RandomAccessFile("/data/mounted.bin", "r")) {
            byte[] bytes = new byte[3];
            input.seek(1);
            input.readFully(bytes);
            ByteBuffer buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
            if (buffer.getShort() != 0x4523 || buffer.get() != 0x67 || input.getFilePointer() != 4) return 0;
        }
        AtomicFile atomic = new AtomicFile(new File(directory, "atomic-test"));
        FileOutputStream committed = atomic.startWrite();
        committed.write(42);
        atomic.finishWrite(committed);
        FileOutputStream discarded = atomic.startWrite();
        discarded.write(17);
        atomic.failWrite(discarded);
        try (FileInputStream input = atomic.openRead()) {
            if (input.read() != 42 || input.read() != -1 || atomic.getLastModifiedTime() == 0) return 0;
        }
        atomic.delete();
        File temporary = File.createTempFile("sample", null, directory);
        if (!temporary.isFile() || temporary.createNewFile() || !temporary.delete()) return 0;
        File replacement = File.createTempFile("sample", null, directory);
        if (temporary.getPath().equals(replacement.getPath()) || !replacement.delete()) return 0;
        try (FileOutputStream ignored = new FileOutputStream("/data/mounted.bin")) {
            return 0;
        } catch (FileNotFoundException expected) {
            return file.length() == 9 && directory.listFiles().length == 1 && file.delete() ? 1 : 0;
        }
    }
    public static int verifyScheduledExecutor() throws Exception {
        var executor = Executors.newScheduledThreadPool(1);
        AtomicInteger rejected = new AtomicInteger();
        ((ThreadPoolExecutor) executor).setRejectedExecutionHandler((task, pool) -> rejected.incrementAndGet());
        var value = executor.schedule(() -> 42, 0, TimeUnit.MILLISECONDS);
        var cancelled = executor.schedule(() -> { throw new AssertionError("Cancelled task ran"); },
                1, TimeUnit.SECONDS);
        if (value.isDone() || !cancelled.cancel(false)) return 0;
        executor.shutdown();
        executor.execute(() -> { throw new AssertionError("Rejected task ran"); });
        if (executor.isTerminated()) return 0;
        Looper.getMainLooper().dispatchDueMessages(SystemClock.uptimeMillis() + 2000);
        return value.isDone() && value.get() == 42 && cancelled.isCancelled()
                && executor.isTerminated() && rejected.get() == 1 ? 1 : 0;
    }

    public static int verifyScheduledWorkerWait() throws Exception {
        var executor = Executors.newScheduledThreadPool(1);
        var handler = new Handler(Looper.getMainLooper());
        var lock = new Object();
        var notified = new boolean[1];
        var result = executor.schedule(() -> {
            synchronized (lock) {
                handler.post(() -> {
                    synchronized (lock) {
                        notified[0] = true;
                        lock.notifyAll();
                    }
                });
                lock.wait(500);
                return notified[0];
            }
        }, 0, TimeUnit.MILLISECONDS);
        executor.shutdown();
        long now = SystemClock.uptimeMillis() + 1000;
        Looper.dispatchAllDueMessages(now);
        Looper.dispatchAllDueMessages(now);
        return result.isDone() && result.get() && executor.isTerminated() ? 1 : 0;
    }

    public static int verifyForkJoin() throws Exception {
        ForkJoinPool common = ForkJoinPool.commonPool();
        AtomicInteger counter = new AtomicInteger();
        common.execute(counter::incrementAndGet);
        var action = new RecursiveAction() {
            @Override protected void compute() { counter.addAndGet(2); }
        };
        action.fork().join();
        if (common != ForkJoinPool.commonPool() ||
                !common.awaitQuiescence(1, TimeUnit.SECONDS) || counter.get() != 3) return 0;

        ForkJoinPool pool = new ForkJoinPool(1);
        RecursiveTask<Integer> parent = new RecursiveTask<Integer>() {
            @Override protected Integer compute() {
                var child = new RecursiveTask<Integer>() {
                    @Override protected Integer compute() { return 21; }
                };
                return getPool() == pool ? child.fork().join() * 2 : 0;
            }
        };
        if (pool.invoke(parent) != 42) return 0;

        var completer = new CountedCompleter<Void>() {
            @Override public void compute() {
                setPendingCount(1);
                new CountedCompleter<Void>(this) {
                    @Override public void compute() {
                        counter.incrementAndGet();
                        tryComplete();
                    }
                }.fork();
                tryComplete();
            }
        };
        pool.submit(completer).get(1, TimeUnit.SECONDS);
        Semaphore ready = new Semaphore(0);
        var blocked = pool.submit(() -> {
            pool.execute(ready::release);
            ForkJoinPool.managedBlock(new ForkJoinPool.ManagedBlocker() {
                @Override public boolean isReleasable() { return ready.tryAcquire(); }
                @Override public boolean block() throws InterruptedException {
                    ready.acquire();
                    return true;
                }
            });
            return counter.get();
        });
        if (blocked.get(1, TimeUnit.SECONDS) != 4) return 0;
        pool.shutdown();
        return pool.awaitTermination(1, TimeUnit.SECONDS) ? 1 : 0;
    }

    public static int verifyLocalSockets() throws Exception {
        String name = "ui-framework-smoke";
        CountDownLatch finished = new CountDownLatch(1);
        AtomicInteger reply = new AtomicInteger(-1);
        try (LocalServerSocket listener = new LocalServerSocket(name)) {
            try (LocalServerSocket duplicate = new LocalServerSocket(name)) {
                throw new IllegalStateException("A second listener acquired the same address");
            } catch (IOException expected) {
                // The address stays bound until the first listener closes.
            }
            new Thread(() -> {
                try (LocalSocket client = new LocalSocket()) {
                    client.connect(new LocalSocketAddress(name));
                    client.getOutputStream().write(73);
                    client.shutdownOutput();
                    reply.set(client.getInputStream().read());
                } catch (IOException error) {
                    throw new IllegalStateException(error);
                } finally {
                    finished.countDown();
                }
            }).start();
            try (LocalSocket socket = listener.accept()) {
                if (socket.getInputStream().read() != 73)
                    throw new IllegalStateException("Listener did not receive the client's byte");
                if (socket.getInputStream().read() != -1)
                    throw new IllegalStateException("Listener did not observe the client's shutdownOutput");
                socket.getOutputStream().write(91);
                if (!finished.await(1, TimeUnit.SECONDS))
                    throw new IllegalStateException("Client did not resume after the listener wrote");
                if (reply.get() != 91)
                    throw new IllegalStateException("Client did not receive the listener's byte");
            }
        }
        try (LocalServerSocket rebound = new LocalServerSocket(name)) {
            return rebound.getFileDescriptor().valid() ? 1 : 0;
        }
    }

    public static int verifyCrc32() {
        CRC32 checksum = new CRC32();
        checksum.update('1');
        ByteBuffer rest = ByteBuffer.wrap("23456789".getBytes()).asReadOnlyBuffer();
        checksum.update(rest);
        if (rest.hasRemaining() || checksum.getValue() != 0xcbf43926L) return 0;
        checksum.reset();
        return checksum.getValue() == 0 ? 1 : 0;
    }

    public static int verifyDeflater() {
        byte[] expected = {(byte) 0x78, (byte) 0x9c, (byte) 0xcb, (byte) 0x48,
                (byte) 0xcd, (byte) 0xc9, (byte) 0xc9, (byte) 0x07, (byte) 0x00,
                (byte) 0x06, (byte) 0x2c, (byte) 0x02, (byte) 0x15};
        Deflater deflater = new Deflater();
        try {
            ByteBuffer source = ByteBuffer.wrap("hello".getBytes()).asReadOnlyBuffer();
            deflater.reset();
            deflater.setInput(source);
            deflater.finish();
            byte[] result = new byte[expected.length + 2];
            int written = 0;
            for (int i = 0; i < 16 && !deflater.finished(); i++) {
                written += deflater.deflate(result, written, Math.min(2, result.length - written));
            }
            return deflater.finished() && !source.hasRemaining() && deflater.getBytesRead() == 5
                    && deflater.getBytesWritten() == expected.length && deflater.getAdler() == 0x062c0215
                    && java.util.Arrays.equals(expected, java.util.Arrays.copyOf(result, written)) ? 1 : 0;
        } finally {
            deflater.end();
        }
    }

    public static int verifyNestedLooper() {
        Looper main = Looper.getMainLooper();
        Handler handler = new Handler(main);
        long now = SystemClock.uptimeMillis();
        int[] caught = {0};
        handler.post(() -> {
            for (int attempt = 1; attempt <= 2; ++attempt) {
                try {
                    Looper.loop();
                    throw new AssertionError("Main Looper returned before a message failed");
                } catch (IllegalArgumentException expected) {
                    caught[0] += attempt;
                }
            }
        });
        main.dispatchDueMessages(now);
        if (caught[0] != 0) return 0;
        Runnable failure = () -> { throw new IllegalArgumentException("Queued message failed"); };
        handler.post(failure);
        main.dispatchDueMessages(now);
        if (caught[0] != 1) return 0;
        handler.post(failure);
        handler.post(() -> ++caught[0]);
        main.dispatchDueMessages(now);
        return caught[0] == 4 ? 1 : 0;
    }
    public static int verifyThreadStack() {
        StackTraceElement[] trace = Thread.currentThread().getStackTrace();
        return trace.length > 0 && "verifyThreadStack".equals(trace[0].getMethodName())
                && new Thread().getStackTrace().length == 0 ? 1 : 0;
    }

    public static int verifyHandlerThread() {
        HandlerThread worker = new HandlerThread("startup-worker");
        ConditionVariable entered = new ConditionVariable();
        ConditionVariable release = new ConditionVariable();
        ConditionVariable finished = new ConditionVariable();
        worker.start();
        Looper looper = worker.getLooper();
        new Handler(looper).post(() -> {
            if (Thread.currentThread() != worker || Looper.myLooper() != looper) return;
            entered.open();
            release.block();
            finished.open();
        });
        entered.block();
        if (finished.block(1)) return 0;
        release.open();
        finished.block();
        worker.quitSafely();
        return 1;
    }

    public static int verifyReadWriteLock() {
        var lock = new ReentrantReadWriteLock();
        lock.writeLock().lock();
        lock.writeLock().lock();
        if (!lock.readLock().tryLock() || lock.getReadHoldCount() != 1 || lock.getWriteHoldCount() != 2) return 0;
        lock.readLock().unlock();
        lock.writeLock().unlock();
        lock.writeLock().unlock();
        Thread thread = Thread.currentThread();
        LockSupport.unpark(thread);
        LockSupport.park(lock);
        return !lock.isWriteLocked() && lock.getReadLockCount() == 0 && LockSupport.getBlocker(thread) == null ? 1 : 0;
    }

    public static int verifyReferenceQueue() throws InterruptedException {
        var queue = new ReferenceQueue<Object>();
        var reference = new WeakReference<Object>(null, queue);
        var received = new AtomicReference<Reference<?>>();
        var waiting = new ConditionVariable();
        var finished = new ConditionVariable();
        new Thread(() -> {
            waiting.open();
            try {
                received.set(queue.remove());
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
            } finally {
                finished.open();
            }
        }).start();
        waiting.block();
        if (finished.block(1) || !reference.enqueue() || !reference.isEnqueued()) return 0;
        if (reference.enqueue() || !finished.block(1000) || received.get() != reference
                || reference.isEnqueued() || queue.poll() != null) return 0;
        var first = new WeakReference<Object>(null, queue);
        var second = new WeakReference<Object>(null, queue);
        return first.enqueue() && second.enqueue() && queue.poll() == first && queue.remove() == second
                && queue.remove(1) == null ? 1 : 0;
    }

    public static int verifyResultReceiver() {
        var result = new AtomicInteger();
        var main = Looper.getMainLooper();
        var receiver = new ResultReceiver(new Handler(main)) {
            @Override protected void onReceiveResult(int code, Bundle data) {
                if ("from parcel".equals(data.getString("source"))) result.set(code);
            }
        };
        Parcel parcel = Parcel.obtain();
        try {
            receiver.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            var copy = ResultReceiver.CREATOR.createFromParcel(parcel);
            var data = new Bundle();
            data.putString("source", "from parcel");
            copy.send(42, data);
            if (result.get() != 0) return 0;
            main.dispatchDueMessages(SystemClock.uptimeMillis());
            return result.get() == 42 ? 1 : 0;
        } finally {
            parcel.recycle();
        }
    }

    public static int verifyRunningTask() {
        var manager = (android.app.ActivityManager) active.getSystemService(Context.ACTIVITY_SERVICE);
        var tasks = manager.getRunningTasks(10);
        if (tasks.size() != 1 || !manager.getRunningTasks(0).isEmpty()) return 0;
        var task = tasks.get(0);
        return active.isTaskRoot() && active.getTaskId() > 0 && active.getTaskId() == task.taskId
                && task.id == task.taskId && task.numActivities == 1 && task.isRunning
                && active.getComponentName().equals(task.topActivity)
                && task.topActivity.equals(task.baseActivity) ? 1 : 0;
    }

    public static int verifyProviderQuery() {
        var resolver = active.getContentResolver();
        resolver.query(Uri.parse("content://0@org.wasmandroid.uismoke.default/test"), null, null, null, null);
        return providerQueries == 1
                && resolver.query(Uri.parse("content://10@org.wasmandroid.uismoke.default"), null, null, null, null) == null
                && resolver.query(Uri.parse("content://org.wasmandroid.uismoke.remote"), null, null, null, null) == null
                && providerQueries == 1 ? 1 : 0;
    }

    public static class MainService extends android.app.Service {
        static int created;

        @Override public void onCreate() {
            super.onCreate();
            created++;
        }

        @Override public android.os.IBinder onBind(Intent intent) {
            return null;
        }
    }

    public static final class RemoteService extends MainService {}

    public static int verifyManifestServiceProcess() {
        var remote = new Intent(active, RemoteService.class);
        var connection = new android.content.ServiceConnection() {
            @Override public void onServiceConnected(android.content.ComponentName name, android.os.IBinder service) {}
            @Override public void onServiceDisconnected(android.content.ComponentName name) {}
        };
        if (active.startService(remote) != null || active.bindService(remote, connection, Context.BIND_AUTO_CREATE)
                || MainService.created != 0) {
            return 0;
        }
        var local = new Intent(active, MainService.class);
        return active.startService(local) != null && MainService.created == 1 && active.stopService(local) ? 1 : 0;
    }

    public static int verifyConcurrentKeySet() {
        var map = new java.util.concurrent.ConcurrentHashMap<String, Integer>();
        map.put("existing", 3);
        var view = map.keySet(7);
        if (view.add("existing") || !view.add("new") || map.get("new") != 7) return 0;
        var keys = java.util.concurrent.ConcurrentHashMap.<String>newKeySet(2);
        keys.add("one");
        var iterator = keys.iterator();
        keys.add("two");
        if (!iterator.next().equals("one")) return 0;
        iterator.remove();
        return keys.size() == 1 && keys.contains("two") ? 1 : 0;
    }
    public static int verifyCopiesList() {
        var copies = Collections.nCopies(Integer.MAX_VALUE, "same");
        if (copies.size() != Integer.MAX_VALUE || !copies.get(Integer.MAX_VALUE - 1).equals("same")) return 0;
        var tail = copies.subList(Integer.MAX_VALUE - 2, Integer.MAX_VALUE);
        var iterator = tail.iterator();
        if (!iterator.next().equals("same") || !iterator.next().equals("same") || iterator.hasNext()) return 0;
        try {
            tail.set(0, "different");
            return 0;
        } catch (UnsupportedOperationException expected) {
            return 1;
        }
    }
    public static int verifyPriorityQueue() {
        var queue = new java.util.PriorityQueue<Integer>(1, (left, right) -> Integer.compare(right, left));
        for (int i = 1; i <= 100; i++) queue.offer(i);
        if (queue.peek() != 100 || !queue.remove(95)) return 0;
        for (int i = 100; i > 0; i--) {
            if (i != 95 && queue.remove() != i) return 0;
        }
        return queue.poll() == null ? 1 : 0;
    }
    public static int verifyUtf32() {
        var little = java.nio.charset.Charset.forName("UTF-32LE");
        var big = java.nio.charset.Charset.forName("UTF-32BE");
        var scalar = "\ud83d\ude42";
        byte[] littleBytes = {0x42, (byte) 0xf6, 0x01, 0};
        byte[] bigBytes = {0, 0x01, (byte) 0xf6, 0x42};
        return java.util.Arrays.equals(scalar.getBytes(little), littleBytes)
                && java.util.Arrays.equals(scalar.getBytes(big), bigBytes)
                && scalar.equals(new String(littleBytes, little))
                && scalar.equals(new String(bigBytes, big)) ? 1 : 0;
    }
    public static int verifyTlsProviderUnavailable() throws java.security.NoSuchAlgorithmException {
        try {
            javax.net.ssl.TrustManagerFactory.getInstance(javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
            return 0;
        } catch (java.security.ProviderException expected) {
            return 1;
        }
    }
    public static int verifyWebp() {
        // Lossless 2-by-1 image with one translucent and one opaque pixel.
        byte[] webp = {
            82, 73, 70, 70, 32, 0, 0, 0, 87, 69, 66, 80,
            86, 80, 56, 76, 19, 0, 0, 0, 47, 1, 0, 0,
            16, 15, 48, -128, -5, 63, -1, 15, -4, 15, 45, 100,
            68, -1, 3, 0
        };
        var options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        if (BitmapFactory.decodeByteArray(webp, 0, webp.length, options) != null
                || options.outWidth != 2 || options.outHeight != 1 || !"image/webp".equals(options.outMimeType)) {
            return 0;
        }
        options.inJustDecodeBounds = false;
        var bitmap = BitmapFactory.decodeByteArray(webp, 0, webp.length, options);
        if (bitmap == null || (bitmap.getPixel(0, 0) >>> 24) != 128 || bitmap.getPixel(1, 0) != 0xffff0000) {
            return 0;
        }
        bitmap.recycle();
        return 1;
    }
    public static int verifyNioPaths() throws java.io.IOException {
        var dir = java.nio.file.Paths.get("/data", "nio-probe", "nested");
        if (!"nested".equals(dir.getFileName().toString()) || !dir.getParent().resolve("nested").equals(dir)) {
            return 0;
        }
        var created = java.nio.file.Files.createDirectories(dir);
        if (created != dir || !java.nio.file.Files.isDirectory(dir, java.nio.file.LinkOption.NOFOLLOW_LINKS)
                || !dir.toFile().isDirectory() || !java.nio.file.Files.createDirectories(dir).equals(dir)) {
            return 0;
        }
        return dir.toFile().delete() && dir.getParent().toFile().delete() ? 1 : 0;
    }
    public static int verifyOsStat() throws java.io.IOException, android.system.ErrnoException {
        var dir = new java.io.File("/data/stat-probe");
        if (!dir.mkdir()) return 0;
        var file = new java.io.File(dir, "file");
        try (var stream = new java.io.FileOutputStream(file)) {
            stream.write(new byte[]{1, 2, 3});
        }
        var bytes = (byte[]) dalvik.system.VMRuntime.getRuntime().newNonMovableArray(byte.class, 2);
        try (var stream = new java.io.FileInputStream(file)) {
            if (android.system.Os.pread(stream.getFD(), bytes, 0, 2, 1L) != 2
                    || bytes[0] != 2 || bytes[1] != 3 || stream.read() != 1) {
                return 0;
            }
        }
        var directory = android.system.Os.stat(dir.getPath());
        var regular = android.system.Os.lstat(file.getPath());
        if (!android.system.OsConstants.S_ISDIR(directory.st_mode)
                || !android.system.OsConstants.S_ISREG(regular.st_mode)
                || android.system.OsConstants.S_ISLNK(regular.st_mode)
                || regular.st_size != 3 || regular.st_blocks != 1
                || regular.st_mtime <= 0 || regular.st_mtim.tv_sec != regular.st_mtime) {
            return 0;
        }
        boolean missing = false;
        boolean notDirectory = false;
        try {
            android.system.Os.stat(dir.getPath() + "/missing");
        } catch (android.system.ErrnoException e) {
            missing = e.errno == android.system.OsConstants.ENOENT && e.getMessage().contains("ENOENT");
        }
        try {
            android.system.Os.lstat(file.getPath() + "/child");
        } catch (android.system.ErrnoException e) {
            notDirectory = e.errno == android.system.OsConstants.ENOTDIR;
        }
        return missing && notDirectory && file.delete() && dir.delete() ? 1 : 0;
    }
    public static int verifyPicture() {
        var picture = new android.graphics.Picture();
        var recording = picture.beginRecording(4, 3);
        var paint = new android.graphics.Paint();
        paint.setColor(0xffff0000);
        recording.drawRect(0, 0, 4, 3, paint);
        var copy = new android.graphics.Picture(picture);
        picture.endRecording();
        picture.beginRecording(3, 2).drawColor(0xff00ff00);
        var bitmap = android.graphics.Bitmap.createBitmap(12, 9, android.graphics.Bitmap.Config.ARGB_8888);
        var canvas = new android.graphics.Canvas(bitmap);
        canvas.drawPicture(copy, new android.graphics.Rect(2, 1, 10, 7));
        picture.draw(canvas);
        return copy.getWidth() == 4 && copy.getHeight() == 3
                && bitmap.getPixel(1, 1) == 0xff00ff00
                && bitmap.getPixel(8, 5) == 0xffff0000 && bitmap.getPixel(11, 8) == 0 ? 1 : 0;
    }
    public static int verifyVirtualKeyboard() {
        var map = android.view.KeyCharacterMap.load(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD);
        int key = android.view.KeyEvent.KEYCODE_A;
        if (map.getKeyboardType() != android.view.KeyCharacterMap.FULL || map.get(key, 0) != 'a'
                || map.get(key, android.view.KeyEvent.META_SHIFT_ON) != 'A'
                || map.getNumber(android.view.KeyEvent.KEYCODE_9) != '9'
                || map.getDisplayLabel(key) != 'A') return 0;
        var events = map.getEvents(new char[]{'A', '9'});
        return events != null && events.length == 4
                && events[0].getUnicodeChar() == 'A' && events[2].getUnicodeChar() == '9' ? 1 : 0;
    }
    public static int verifyRegex() {
        var placeholders = Pattern.compile("\\$\\{(.*?)\\}").matcher("${first} and ${second}");
        if (!placeholders.find() || !placeholders.group(1).equals("first") || placeholders.start(1) != 2) return 0;
        if (!placeholders.find() || !placeholders.group(1).equals("second") || placeholders.find()) return 0;
        String letters = Pattern.compile("(?<word>[a-z]+)").matcher("12abc34").replaceAll("[${word}]");
        String[] words = Pattern.compile("\\s+", Pattern.UNICODE_CHARACTER_CLASS).split("two\u00a0words");
        Pattern absolute = Pattern.compile("\\A[a-z-]+\\z");
        if (!absolute.matcher("ab-c").find() || absolute.matcher("ab-c\n").find()) return 0;
        if (!Pattern.compile("[^\\p{Alnum}]").matcher("a1_b❤").replaceAll("").equals("a1b")) return 0;
        Pattern intersection = Pattern.compile("[a-c[x-z]&&[^b]]+");
        if (!intersection.matcher("acxyz").matches() || intersection.matcher("ab").matches()) return 0;
        return letters.equals("12[abc]34") && words.length == 2 && words[1].equals("words") ? 1 : 0;
    }
    public static int verifyLegacyTime() {
        android.text.format.Time time = new android.text.format.Time("GMT+05:30");
        time.set(0L);
        return time.year == 1970 && time.month == 0 && time.monthDay == 1 && time.hour == 5
                && time.minute == 30 && time.gmtoff == 19_800L && time.toMillis(false) == 0L ? 1 : 0;
    }
    private static final class StringList extends java.util.ArrayList<String> {}
    public static int verifyGenericSuperclass() {
        var type = StringList.class.getGenericSuperclass();
        return type instanceof java.lang.reflect.ParameterizedType generic
                && generic.getRawType() == java.util.ArrayList.class
                && generic.getActualTypeArguments()[0] == String.class ? 1 : 0;
    }
    public static int verifyFieldReflection() throws ReflectiveOperationException {
        var field = SmokeActivity.class.getDeclaredField("REFLECTED_VALUE");
        field.setAccessible(true);
        if (field.getType() != int.class || field.getLong(null) != 17 || !field.getName().equals("REFLECTED_VALUE")) {
            return 0;
        }
        try {
            field.setInt(null, 42);
            return 0;
        } catch (IllegalAccessException expected) {
            return 1;
        }
    }
    public static int verifyLegacyFieldReflection() throws ReflectiveOperationException {
        var field = SmokeActivity.class.getDeclaredField("LEGACY_FIELD");
        field.setAccessible(true);
        Object original = field.get(null);
        Object replacement = new Object();
        field.set(null, replacement);
        boolean changed = field.get(null) == replacement && LEGACY_FIELD == replacement;
        field.set(null, original);
        return changed ? 1 : 0;
    }

    public static int verifyBigInteger() {
        BigInteger a = BigInteger.ONE.shiftLeft(3201).add(BigInteger.valueOf(23));
        BigInteger b = BigInteger.ONE.shiftLeft(1733).add(BigInteger.valueOf(17));
        BigInteger[] quotient = a.multiply(b).add(BigInteger.valueOf(123)).divideAndRemainder(b);
        return quotient[0].equals(a) && quotient[1].intValueExact() == 123
                && BigInteger.valueOf(7).modPow(BigInteger.valueOf(37), BigInteger.valueOf(101)).intValue() == 35
                ? 1 : 0;
    }

    public static int verifyDecimalRounding() {
        return "17.38".equals(BigDecimal.valueOf(17.375).setScale(2, RoundingMode.HALF_UP).toString()) ? 1 : 0;
    }

    public static int verifyArrayListHierarchy() {
        AbstractCollection<String> items = new ArrayList<>();
        return items.add("login") && items.contains("login")
                && items.getClass().getSuperclass() == AbstractList.class ? 1 : 0;
    }

    public static int verifySlashWrapping() {
        var paint = new TextPaint();
        paint.setTextSize(20);
        String text = "Country/Region";
        int width = (int) Math.ceil(paint.measureText("Country/R"));
        var layout = StaticLayout.Builder.obtain(text, 0, text.length(), paint, width).build();
        return layout.getLineCount() == 2 && layout.getLineEnd(0) == "Country/".length() ? 1 : 0;
    }

    public static int verifyEmbeddedDom() throws Exception {
        var builder = DocumentBuilderFactory.newInstance().newDocumentBuilder();
        String xml = "<countries><country code='1'>A &amp; B</country></countries>";
        var document = builder.parse(new ByteArrayInputStream(xml.getBytes(StandardCharsets.UTF_8)));
        document.getDocumentElement().normalize();
        var country = (Element) document.getElementsByTagName("country").item(0);
        if (!"1".equals(country.getAttribute("code")) || !"A & B".equals(country.getTextContent())) return 0;
        try {
            builder.parse(new ByteArrayInputStream("<country>".getBytes(StandardCharsets.UTF_8)));
            return 0;
        } catch (SAXParseException expected) {
            return expected.getLineNumber() >= 1 ? 1 : 0;
        }
    }

    public static int verifyEmbeddedPackage() throws PackageManager.NameNotFoundException {
        String name = active.getPackageName();
        PackageManager manager = active.getPackageManager();
        var info = manager.getPackageInfo(name, 0);
        return name.equals(info.packageName) && "test-release".equals(info.versionName)
                && info.getLongVersionCode() == 31 && manager.getInstallerPackageName(name) == null ? 1 : 0;
    }
    public static int verifyDigest() throws NoSuchAlgorithmException, CloneNotSupportedException {
        MessageDigest digest = MessageDigest.getInstance("SHA1");
        digest.update((byte) 'a');
        MessageDigest clone = (MessageDigest) digest.clone();
        byte[] data = "bcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".getBytes();
        digest.update(data, 0, data.length);
        byte[] expected = {(byte) 0x84, (byte) 0x98, 0x3e, 0x44, 0x1c, 0x3b, (byte) 0xd2, 0x6e,
                (byte) 0xba, (byte) 0xae, 0x4a, (byte) 0xa1, (byte) 0xf9, 0x51, 0x29, (byte) 0xe5,
                (byte) 0xe5, 0x46, 0x70, (byte) 0xf1};
        if (!MessageDigest.isEqual(digest.digest(), expected) || digest.getDigestLength() != expected.length) {
            throw new IllegalStateException("SHA-1 streaming vector");
        }
        if (!MessageDigest.isEqual(clone.digest(data), expected)) {
            throw new IllegalStateException("Cloned SHA-1 state");
        }
        byte[] abc = {'a', 'b', 'c'};
        UUID uuid = UUID.nameUUIDFromBytes(abc);
        if (!uuid.equals(new UUID(0x900150983cd23fb0L, 0x96963f7d28e17f72L))) {
            throw new IllegalStateException("MD5 name-based UUID: " + uuid);
        }
        byte[] sha256 = {(byte) 0xba, 0x78, 0x16, (byte) 0xbf, (byte) 0x8f, 0x01, (byte) 0xcf, (byte) 0xea,
                0x41, 0x41, 0x40, (byte) 0xde, 0x5d, (byte) 0xae, 0x22, 0x23,
                (byte) 0xb0, 0x03, 0x61, (byte) 0xa3, (byte) 0x96, 0x17, 0x7a, (byte) 0x9c,
                (byte) 0xb4, 0x10, (byte) 0xff, 0x61, (byte) 0xf2, 0x00, 0x15, (byte) 0xad};
        return MessageDigest.isEqual(MessageDigest.getInstance("SHA-256").digest(abc), sha256) ? 1 : 0;
    }
    public static int verifyJson() throws JSONException {
        JSONObject data = new JSONObject("{\"items\":[7,{\"name\":\"first\"}]}");
        if (data.getJSONArray("items").getInt(0) != 7) return 0;
        data.put("ready", true);
        var copy = new JSONObject(data.toString());
        return copy.getBoolean("ready") && "first".equals(copy.getJSONArray("items")
                .getJSONObject(1).getString("name")) ? 1 : 0;
    }
    public static int verifyEmbeddedUrl() throws IOException {
        URL base = new URL("https://example.test/alpha/beta");
        URL resolved = new URL(base, "../page?q=1#fragment");
        if (!"https://example.test/page?q=1#fragment".equals(resolved.toExternalForm())
                || resolved.getDefaultPort() != 443) return 0;
        try {
            resolved.openConnection();
            return 0;
        } catch (IOException expected) {
            return 1;
        }
    }
    public static int verifyExecutorAndRegisterShutdownHook() throws InterruptedException {
        var executor = new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS, new LinkedBlockingQueue<>(),
                Executors.defaultThreadFactory(), new ThreadPoolExecutor.DiscardPolicy());
        executor.setMaximumPoolSize(2);
        if (executor.getMaximumPoolSize() != 2) return 0;
        AtomicInteger ran = new AtomicInteger();
        AtomicInteger rejected = new AtomicInteger();
        executor.setRejectedExecutionHandler((task, pool) -> rejected.incrementAndGet());
        executor.execute(ran::incrementAndGet);
        executor.shutdown();
        executor.execute(ran::incrementAndGet);
        Thread hook = new Thread(() -> shutdownHookRan = "smoke-hook".equals(Thread.currentThread().getName()),
                "smoke-hook");
        Runtime runtime = Runtime.getRuntime();
        runtime.addShutdownHook(hook);
        if (!runtime.removeShutdownHook(hook) || runtime.removeShutdownHook(hook)) return 0;
        runtime.addShutdownHook(hook);
        return ran.get() == 1 && rejected.get() == 1 && executor.awaitTermination(0, TimeUnit.SECONDS) ? 1 : 0;
    }
    public static boolean shutdownHookRan() { return shutdownHookRan; }
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
