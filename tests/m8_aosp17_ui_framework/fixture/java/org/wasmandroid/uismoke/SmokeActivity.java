package org.wasmandroid.uismoke;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

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
import android.os.SystemClock;
import android.util.AtomicFile;
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
import java.net.URL;
import java.net.URISyntaxException;
import java.util.HashSet;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;
import java.util.concurrent.locks.ReentrantReadWriteLock;
import java.util.regex.Pattern;
import java.util.stream.Collectors;
import java.util.stream.IntStream;
import org.json.JSONException;
import org.json.JSONObject;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

public final class SmokeActivity extends Activity {
    private static SmokeActivity active;
    private static int lifecycle;
    private static boolean shutdownHookRan;
    private static final int REFLECTED_VALUE = 17;
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
        var value = executor.schedule(() -> 42, 0, TimeUnit.MILLISECONDS);
        var cancelled = executor.schedule(() -> { throw new AssertionError("Cancelled task ran"); },
                1, TimeUnit.SECONDS);
        if (value.isDone() || !cancelled.cancel(false)) return 0;
        executor.shutdown();
        if (executor.isTerminated()) return 0;
        Looper.getMainLooper().dispatchDueMessages(SystemClock.uptimeMillis() + 2000);
        return value.isDone() && value.get() == 42 && cancelled.isCancelled() && executor.isTerminated() ? 1 : 0;
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
    public static int verifyRegex() {
        var placeholders = Pattern.compile("\\$\\{(.*?)\\}").matcher("${first} and ${second}");
        if (!placeholders.find() || !placeholders.group(1).equals("first") || placeholders.start(1) != 2) return 0;
        if (!placeholders.find() || !placeholders.group(1).equals("second") || placeholders.find()) return 0;
        String letters = Pattern.compile("(?<word>[a-z]+)").matcher("12abc34").replaceAll("[${word}]");
        String[] words = Pattern.compile("\\s+", Pattern.UNICODE_CHARACTER_CLASS).split("two\u00a0words");
        Pattern absolute = Pattern.compile("\\A[a-z-]+\\z");
        if (!absolute.matcher("ab-c").find() || absolute.matcher("ab-c\n").find()) return 0;
        if (!Pattern.compile("[^\\p{Alnum}]").matcher("a1_b❤").replaceAll("").equals("a1b")) return 0;
        return letters.equals("12[abc]34") && words.length == 2 && words[1].equals("words") ? 1 : 0;
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
        if (!MessageDigest.isEqual(digest.digest(), expected) || digest.getDigestLength() != expected.length) return 0;
        return MessageDigest.isEqual(clone.digest(data), expected) ? 1 : 0;
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
        AtomicInteger ran = new AtomicInteger();
        executor.execute(ran::incrementAndGet);
        executor.shutdown();
        executor.execute(ran::incrementAndGet);
        Thread hook = new Thread(() -> shutdownHookRan = "smoke-hook".equals(Thread.currentThread().getName()),
                "smoke-hook");
        Runtime runtime = Runtime.getRuntime();
        runtime.addShutdownHook(hook);
        if (!runtime.removeShutdownHook(hook) || runtime.removeShutdownHook(hook)) return 0;
        runtime.addShutdownHook(hook);
        return ran.get() == 1 && executor.awaitTermination(0, TimeUnit.SECONDS) ? 1 : 0;
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
