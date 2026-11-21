package com.android.launcher3;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.app.WallpaperColors;
import android.app.WallpaperManager;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.LauncherApps;
import android.content.res.Configuration;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.UserHandle;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.Toast;
import com.android.launcher3.LauncherSettings.Favorites;
import com.android.launcher3.allapps.ActivityAllAppsContainerView;
import com.android.launcher3.dragndrop.DragLayer;
import com.android.launcher3.model.data.AppInfo;
import com.android.launcher3.model.data.WorkspaceItemInfo;
import com.android.launcher3.pageindicators.PageIndicatorDotsWithArrows;
import com.android.launcher3.statemanager.StateManager;
import com.android.launcher3.views.AccessibilityActionsView;
import com.android.launcher3.views.ActivityContext;
import com.android.launcher3.views.ScrimView;
import java.util.HashSet;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.stream.Collectors;

public final class Launcher extends Activity implements ActivityContext {
    private static final String ACTION_ALL_APPS = "android.intent.action.ALL_APPS";
    private static final String PAGE = "workspace_page";
    private static final String TAG = "Launcher3";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final LogDecelerateInterpolator drawerEasing = new LogDecelerateInterpolator(50, 0);
    private final LauncherApps.Callback packages = new LauncherApps.Callback() {
        @Override public void onPackageAdded(String name, UserHandle user) { refreshApps(); }
        @Override public void onPackageRemoved(String name, UserHandle user) { refreshApps(); }
        @Override public void onPackageChanged(String name, UserHandle user) { refreshApps(); }
        @Override public void onPackagesAvailable(String[] names, UserHandle user, boolean replacing) {
            refreshApps();
        }
        @Override public void onPackagesUnavailable(String[] names, UserHandle user, boolean replacing) {
            refreshApps();
        }
    };

    private DeviceProfile profile;
    private LauncherUiState uiState;
    private LauncherRootView root;
    private DragLayer dragLayer;
    private Workspace<?> workspace;
    private Hotseat hotseat;
    private PageIndicatorDotsWithArrows indicator;
    private ActivityAllAppsContainerView drawer;
    private AccessibilityActionsView accessibility;
    private ScrimView scrim;
    private LauncherApps launcherApps;
    private LauncherState state = LauncherState.NORMAL;
    private final StateManager<LauncherState, Launcher> states = new StateManager<>(this, state);
    private ValueAnimator transition;
    private Future<?> loading;
    private int generation;
    private int selectedPage;
    private float drawerProgress;

    @Override
    public void onCreate(Bundle savedState) {
        if ((getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES) {
            setTheme(R.style.AppTheme_Dark);
        }
        super.onCreate(savedState);
        setTitle(getText(R.string.derived_app_name));
        configureWindow();
        root = (LauncherRootView) getLayoutInflater().inflate(R.layout.launcher, null);
        dragLayer = root.findViewById(R.id.drag_layer);
        workspace = root.findViewById(R.id.workspace);
        hotseat = root.findViewById(R.id.hotseat);
        indicator = root.findViewById(R.id.page_indicator);
        drawer = root.findViewById(R.id.apps_view);
        accessibility = root.findViewById(R.id.accessibility_action_view);
        scrim = root.findViewById(R.id.scrim_view);

        setContentView(root);
        root.requestApplyInsets();
        selectedPage = savedState == null ? 0 : savedState.getInt(PAGE);
        launcherApps = getSystemService(LauncherApps.class);

        workspace.initParentViews(root);
        indicator.setActions(step -> workspace.selectPage(workspace.getNextPage() + step, true), this::openDrawer);
        accessibility.setOpenDrawer(this::openDrawer);
        dragLayer.setOpenDrawer(this::openDrawer);

        configureLayout();
        scrim.setVisibility(View.GONE);
        drawer.addOnLayoutChangeListener((view, l, t, r, b, ol, ot, or, ob) -> applyDrawerProgress(drawerProgress));
        if (ACTION_ALL_APPS.equals(getIntent().getAction())) drawer.post(this::openDrawer);
    }

    private void configureWindow() {
        Window window = getWindow();
        window.setDecorFitsSystemWindows(false);
        window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER
                | WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        window.setBackgroundDrawableResource(android.R.color.transparent);
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setStatusBarContrastEnforced(false);
        window.setNavigationBarContrastEnforced(false);
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        updateSystemBars(false);
    }

    private void updateSystemBars(boolean appsVisible) {
        boolean darkText = (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                != Configuration.UI_MODE_NIGHT_YES;
        if (!appsVisible) {
            try {
                WallpaperColors colors = WallpaperManager.getInstance(this).getWallpaperColors(WallpaperManager.FLAG_SYSTEM);
                if (colors != null) {
                    darkText = (colors.getColorHints() & WallpaperColors.HINT_SUPPORTS_DARK_TEXT) != 0;
                }
            } catch (SecurityException unavailable) {
                Log.d(TAG, "Wallpaper hints unavailable");
            }
        }
        WindowInsetsController bars = getWindow().getInsetsController();
        int mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
        if (bars != null) bars.setSystemBarsAppearance(darkText ? mask : 0, mask);
    }

    private void configureLayout() {
        profile = DeviceProfile.read(getResources());
        uiState = new LauncherUiState(profile);
        int margin = DeviceProfile.pixels(getResources(), 16);
        int dockHeight = profile.iconPx + DeviceProfile.pixels(getResources(), 24);
        int indicatorHeight = Math.max(DeviceProfile.pixels(getResources(), 40),
                getResources().getDimensionPixelSize(R.dimen.workspace_page_indicator_height));
        FrameLayout.LayoutParams dock = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, dockHeight, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        dock.setMargins(margin, 0, margin, margin / 2);
        hotseat.setLayoutParams(dock);

        FrameLayout.LayoutParams dots = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, indicatorHeight, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        dots.bottomMargin = dockHeight + margin;
        indicator.setLayoutParams(dots);

        FrameLayout.LayoutParams cells = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.TOP);
        cells.setMargins(margin, margin, margin, dockHeight + indicatorHeight + margin * 2);
        workspace.setLayoutParams(cells);

        hotseat.configure(profile);
        drawer.configure(profile, this::launchApp, this::closeDrawer);
        int tint = LauncherState.ALL_APPS.getWorkspaceScrimColor(this).getBackgroundColor();
        scrim.setBackgroundColor(Color.argb(160, Color.red(tint) / 4,
                Color.green(tint) / 4, Color.blue(tint) / 4));
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (launcherApps != null) launcherApps.registerCallback(packages, main);
        refreshApps();
    }

    @Override
    protected void onStop() {
        selectedPage = workspace.getNextPage();
        if (launcherApps != null) launcherApps.unregisterCallback(packages);
        ++generation;
        if (loading != null) loading.cancel(true);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (transition != null) transition.cancel();
        worker.shutdownNow();
        super.onDestroy();
    }

    @Override
    public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        selectedPage = workspace.getNextPage();
        configureLayout();
        refreshApps();
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        state.putInt(PAGE, workspace.getNextPage());
        super.onSaveInstanceState(state);
    }

    private void refreshApps() {
        if (worker.isShutdown()) return;
        if (loading != null) loading.cancel(true);
        int request = ++generation;
        DeviceProfile requestedProfile = profile;
        loading = worker.submit(() -> {
            try {
                AppCatalog.Snapshot snapshot = AppCatalog.load(this, requestedProfile);
                main.post(() -> {
                    if (request == generation && !isDestroyed()) bindApps(snapshot);
                });
            } catch (RuntimeException failure) {
                Log.e(TAG, "Loading Launcher3 applications", failure);
            }
        });
    }

    private void bindApps(AppCatalog.Snapshot snapshot) {
        int currentPage = workspace.getChildCount() == 0 ? selectedPage : workspace.getNextPage();
        workspace.resetPages();
        hotseat.configure(profile);

        CellLayout home = addWorkspacePage();
        CellLayout next = addWorkspacePage();
        HashSet<String> pinned = new HashSet<>();
        for (AppCatalog.Placement placement : snapshot.defaults()) {
            WorkspaceItemInfo item = placement.item();
            CellLayout container = item.container == Favorites.CONTAINER_HOTSEAT ? hotseat : home;
            if (addIcon(container, placement.app(), item, container != hotseat)) {
                pinned.add(placement.app().item().componentName.flattenToString());
            }
        }

        int[] cell = new int[2];
        int remaining = snapshot.apps().size() - pinned.size();
        for (AppCatalog.App app : snapshot.apps()) {
            if (pinned.contains(app.item().componentName.flattenToString())) continue;
            CellLayout target;
            if (home.getChildCount() == 0 && remaining > 1 && home.findVacantCell(cell)) {
                target = home;
            } else if ((remaining > 1 || next.getChildCount() > 0) && hotseat.findVacantCell(cell)) {
                target = hotseat;
            } else if (next.findVacantCell(cell)) {
                target = next;
            } else if (home.findVacantCell(cell)) {
                target = home;
            } else {
                break;
            }
            boolean dock = target == hotseat;
            WorkspaceItemInfo icon = app.item().makeWorkspaceItem(this);
            icon.container = dock ? Favorites.CONTAINER_HOTSEAT : Favorites.CONTAINER_DESKTOP;
            icon.screenId = dock ? cell[0] : target == next ? 1 : 0;
            icon.cellX = cell[0];
            icon.cellY = cell[1];
            addIcon(target, app, icon, !dock);
            remaining--;
        }
        workspace.selectPage(currentPage, false);
        drawer.setApps(snapshot.apps().stream()
                .map(app -> new ActivityAllAppsContainerView.Entry(app.item(), app::icon))
                .collect(Collectors.toList()));
    }

    private CellLayout addWorkspacePage() {
        CellLayout page = (CellLayout) getLayoutInflater().inflate(R.layout.workspace_screen, workspace, false);
        page.setGrid(profile.columns, profile.rows);
        workspace.addPage(page);
        return page;
    }

    private boolean addIcon(CellLayout container, AppCatalog.App app, WorkspaceItemInfo item, boolean label) {
        BubbleTextView icon = (BubbleTextView) getLayoutInflater().inflate(R.layout.app_icon, container, false);
        icon.bind(item, app.icon(container.getContext()), profile, label, view -> launchApp(app.item()));
        return container.addIcon(icon, item);
    }

    private void launchApp(AppInfo app) {
        try {
            startActivity(app.getIntent());
            if (isDrawerOpen()) closeDrawer();
        } catch (ActivityNotFoundException | SecurityException missing) {
            Toast.makeText(this, R.string.activity_not_found, Toast.LENGTH_SHORT).show();
            refreshApps();
        }
    }

    private boolean isDrawerOpen() { return state == LauncherState.ALL_APPS; }
    private void openDrawer() { if (!isDrawerOpen()) states.goToState(LauncherState.ALL_APPS, null); }
    private void closeDrawer() { if (isDrawerOpen()) states.goToState(LauncherState.NORMAL, null); }

    public void applyState(LauncherState target, Animator.AnimatorListener listener) {
        boolean show = target == LauncherState.ALL_APPS;
        state = target;
        if (transition != null) transition.cancel();
        dragLayer.setWorkspaceVisible(!show);
        int a11y = show ? View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
                : View.IMPORTANT_FOR_ACCESSIBILITY_AUTO;
        for (View view : new View[]{workspace, hotseat, indicator, accessibility}) {
            view.setImportantForAccessibility(a11y);
        }
        if (show) {
            scrim.setVisibility(View.VISIBLE);
            drawer.setVisibility(View.VISIBLE);
            drawer.takeDrawerFocus();
        } else {
            drawer.finishSearch();
        }
        updateSystemBars(show);
        transition = ValueAnimator.ofFloat(drawerProgress, show ? 1 : 0);
        transition.setDuration(LauncherState.ALL_APPS.getTransitionDuration(this, show));
        transition.setInterpolator(drawerEasing);
        transition.addUpdateListener(animation -> applyDrawerProgress((float) animation.getAnimatedValue()));
        transition.addListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator animation) {
                if (!isDrawerOpen()) {
                    drawer.setVisibility(View.GONE);
                    scrim.setVisibility(View.GONE);
                }
            }
        });
        if (listener != null) transition.addListener(listener);
        transition.start();
    }

    private void applyDrawerProgress(float progress) {
        drawerProgress = progress;
        drawer.setTranslationY((1f - progress) * drawer.getHeight());
        scrim.setAlpha(progress);
        float scale = 1 + (profile.getWorkspaceProfile().getWorkspaceContentScale() - 1) * progress;
        workspace.setScaleX(scale);
        workspace.setScaleY(scale);
    }

    public void setBackProgress(float progress) {
        if (isDrawerOpen()) applyDrawerProgress(1f - progress * 0.5f);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (ACTION_ALL_APPS.equals(intent.getAction())) openDrawer();
        else showHome();
    }

    private void showHome() {
        closeDrawer();
        workspace.selectPage(0, false);
        selectedPage = 0;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_HOME) {
            if (event.getAction() == KeyEvent.ACTION_UP) showHome();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        if (isDrawerOpen()) state.onBackInvoked(this);
        else if (workspace.getNextPage() != 0) workspace.selectPage(0, true);
    }

    @Override public DeviceProfile getDeviceProfile() { return profile; }
    @Override public DragLayer getDragLayer() { return dragLayer; }
    public LauncherUiState getLauncherUiState() { return uiState; }
    public Workspace<?> getWorkspace() { return workspace; }
    public Hotseat getHotseat() { return hotseat; }
    public StateManager<LauncherState, Launcher> getStateManager() { return states; }
    public float[] getNormalOverviewScaleAndOffset() { return new float[]{1f, 0f}; }
}
