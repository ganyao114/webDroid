package org.wasmandroid.uismoke;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.Intent;
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
import android.os.UserHandle;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.android.launcher3.fixture.TileView;
import java.io.IOException;
import java.util.List;
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
