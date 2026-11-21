package com.android.launcher3;

import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.util.TypedValue;
import com.android.launcher3.deviceprofile.DeviceProperties;
import com.android.launcher3.deviceprofile.HotseatProfile;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** A public-SDK projection of the fields read from the compiled Launcher3 device profiles. */
public final class DeviceProfile {
    private static final String NS = "http://schemas.android.com/apk/res-auto";

    public final int rows;
    public final int columns;
    public final int hotseatIcons;
    public final int workspaceLayout;
    public final int iconPx;
    public final float iconTextSp;
    public final int drawerCellPx;

    private final Resources resources;
    private final DeviceProperties properties;

    private DeviceProfile(Resources resources, Grid grid, DisplayOption display) {
        this.resources = resources;
        rows = grid.rows;
        columns = grid.columns;
        hotseatIcons = grid.hotseatIcons;
        workspaceLayout = grid.layout;
        iconPx = pixels(resources, display.iconDp);
        iconTextSp = display.textSp;
        drawerCellPx = pixels(resources, display.drawerCellDp);
        properties = new DeviceProperties(resources);
    }

    private record Grid(int rows, int columns, int hotseatIcons, int layout) {}
    private record DisplayOption(float iconDp, float textSp, float drawerCellDp) {}

    public static DeviceProfile read(Resources resources) {
        Configuration config = resources.getConfiguration();
        int width = Math.min(config.screenWidthDp, config.screenHeightDp);
        int height = Math.max(config.screenWidthDp, config.screenHeightDp);
        String name = width >= 406 && height >= 694 ? "5_by_5" : "4_by_4";
        try (XmlResourceParser xml = resources.getXml(R.xml.device_profiles)) {
            Grid grid = null;
            DisplayOption nearest = null;
            double distance = Double.MAX_VALUE;
            for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
                if (event == XmlPullParser.START_TAG && "grid-option".equals(xml.getName())
                        && name.equals(xml.getAttributeValue(NS, "name"))) {
                    grid = new Grid(count(xml, "numRows"), count(xml, "numColumns"),
                            count(xml, "numHotseatIcons"),
                            xml.getAttributeResourceValue(NS, "defaultLayoutId", 0));
                } else if (grid != null && event == XmlPullParser.START_TAG
                        && "display-option".equals(xml.getName())) {
                    float icon = xml.getAttributeFloatValue(NS, "iconImageSize", 0);
                    float text = xml.getAttributeFloatValue(NS, "iconTextSize", 0);
                    float cell = xml.getAttributeFloatValue(NS, "allAppsCellHeight", 0);
                    double candidate = Math.hypot(width - xml.getAttributeFloatValue(NS, "minWidthDps", 0),
                            height - xml.getAttributeFloatValue(NS, "minHeightDps", 0));
                    if (icon > 0 && text > 0 && cell > 0 && candidate < distance) {
                        nearest = new DisplayOption(icon, text, cell);
                        distance = candidate;
                    }
                } else if (grid != null && event == XmlPullParser.END_TAG
                        && "grid-option".equals(xml.getName())) {
                    if (nearest == null || grid.layout == 0) {
                        break;
                    }
                    return new DeviceProfile(resources, grid, nearest);
                }
            }
        } catch (IOException | XmlPullParserException failure) {
            throw new IllegalStateException("Reading Launcher3 device_profiles.xml", failure);
        }
        throw new IllegalStateException("Incomplete Launcher3 grid: " + name);
    }

    private static int count(XmlResourceParser xml, String field) {
        int count = xml.getAttributeIntValue(NS, field, 0);
        if (count < 1) {
            throw new IllegalStateException("Missing device profile attribute " + field);
        }
        return count;
    }

    public static int pixels(Resources resources, float dp) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp,
                resources.getDisplayMetrics()));
    }

    public DeviceProperties getDeviceProperties() { return properties; }
    public int getQsbOffsetY() { return 0; }
    public HotseatProfile getHotseatProfile() { return new HotseatProfile(); }
    public boolean shouldFadeAdjacentWorkspaceScreens() { return false; }
    public AllAppsProfile getAllAppsProfile() { return new AllAppsProfile(); }
    public WorkspaceProfile getWorkspaceProfile() { return new WorkspaceProfile(); }

    public int getDropTargetBarHeight() {
        return resources.getDimensionPixelSize(R.dimen.dynamic_grid_drop_target_size);
    }

    public float getWorkspaceSpringLoadScale(Launcher launcher) {
        return Math.max(0.5f, 1f - (float) getDropTargetBarHeight() / Math.max(1, getCellLayoutHeight()));
    }

    public float getCellLayoutSpringLoadShrunkTop() { return getDropTargetBarHeight(); }
    public int getCellLayoutHeight() { return properties.getHeightPx() - iconPx - pixels(resources, 40); }
    public boolean isVerticalBarLayout() { return false; }

    public static final class AllAppsProfile {
        public int getOpenDuration() { return 280; }
        public int getCloseDuration() { return 220; }
    }

    public static final class WorkspaceProfile {
        public float getWorkspaceContentScale() { return 0.94f; }
    }
}
