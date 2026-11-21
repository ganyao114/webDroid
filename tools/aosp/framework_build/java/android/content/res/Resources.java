package android.content.res;

import android.graphics.BitmapFactory;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.io.IOException;
import java.util.ArrayList;

public class Resources implements AutoCloseable {
    public static final int ID_NULL = 0;
    private final AssetManager assets;
    private final DisplayMetrics metrics = new DisplayMetrics();
    private final Configuration configuration = new Configuration();
    private final ArrayList<Theme> themes = new ArrayList<>();
    private final ArrayList<Integer> loadingDrawables = new ArrayList<>();
    private final ApkAssets[] ownedAssets;
    private boolean closed;

    public static class NotFoundException extends RuntimeException {
        public NotFoundException() {}
        public NotFoundException(String message) { super(message); }
        public NotFoundException(String message, Throwable cause) { super(message, cause); }
    }

    public Resources(AssetManager assets, DisplayMetrics metrics, Configuration configuration) {
        this(assets, metrics, configuration, null);
    }

    private Resources(AssetManager assets, DisplayMetrics metrics, Configuration configuration,
                      ApkAssets[] ownedAssets) {
        this.assets = assets;
        this.ownedAssets = ownedAssets;
        updateConfiguration(configuration, metrics);
    }

    public static Resources forApk(String path, String locale, int dpi) throws IOException {
        return forApk(new String[] {path}, locale, dpi);
    }

    /** Load base and split APKs in Android asset-cookie order. */
    public static Resources forApk(String[] paths, String locale, int dpi) throws IOException {
        ApkAssets[] apks = new ApkAssets[paths.length];
        AssetManager assets = new AssetManager();
        try {
            for (int i = 0; i < paths.length; ++i) apks[i] = ApkAssets.loadFromPath(paths[i]);
            assets.setApkAssets(apks);
            Configuration config = new Configuration();
            config.setLocaleTag(locale);
            config.densityDpi = dpi;
            return new Resources(assets, new DisplayMetrics(), config, apks);
        } catch (IOException | RuntimeException error) {
            assets.close();
            for (ApkAssets apk : apks) if (apk != null) apk.close();
            throw error;
        }
    }

    public AssetManager getAssets() { return assets; }
    public DisplayMetrics getDisplayMetrics() { return metrics; }
    public Configuration getConfiguration() { return configuration; }

    public void updateConfiguration(Configuration config, DisplayMetrics display) {
        if (config != null) configuration.setTo(config);
        if (display != null) metrics.setTo(display);
        if (configuration.densityDpi > 0) metrics.densityDpi = configuration.densityDpi;
        metrics.density = metrics.densityDpi / 160f;
        metrics.scaledDensity = metrics.density * configuration.fontScale;
        metrics.xdpi = metrics.ydpi = metrics.densityDpi;
        assets.configure(configuration, metrics);
    }

    public void updateDisplay(int width, int height, int density) {
        if (metrics.widthPixels == width && metrics.heightPixels == height
                && metrics.densityDpi == density) return;
        metrics.widthPixels = width;
        metrics.heightPixels = height;
        configuration.densityDpi = density;
        configuration.screenWidthDp = (int) (width * 160f / density);
        configuration.screenHeightDp = (int) (height * 160f / density);
        configuration.smallestScreenWidthDp = Math.min(configuration.screenWidthDp,
                configuration.screenHeightDp);
        configuration.orientation = width > height ? Configuration.ORIENTATION_LANDSCAPE
                : Configuration.ORIENTATION_PORTRAIT;
        updateConfiguration(null, null);
    }

    public void getValue(int id, TypedValue out, boolean resolveRefs) {
        getValueForDensity(id, 0, out, resolveRefs);
    }

    public void getValueForDensity(int id, int density, TypedValue out, boolean resolveRefs) {
        if (!assets.getResourceValue(id, density, out, resolveRefs)) throw missing(id, "value");
    }

    public void getValue(String name, TypedValue out, boolean resolveRefs) {
        getValue(getIdentifier(name, null, null), out, resolveRefs);
    }

    private TypedValue value(int id) {
        TypedValue result = new TypedValue();
        getValue(id, result, true);
        return result;
    }

    public int getIdentifier(String name, String type, String pkg) {
        if (name == null) throw new NullPointerException("name");
        return assets.getResourceIdentifier(name, type, pkg);
    }

    public String getResourceName(int id) { return requireName(id, assets.resourceName(id)); }
    public String getResourcePackageName(int id) { return requireName(id, assets.resourcePackageName(id)); }
    public String getResourceTypeName(int id) { return requireName(id, assets.resourceTypeName(id)); }
    public String getResourceEntryName(int id) { return requireName(id, assets.resourceEntryName(id)); }

    private String requireName(int id, String name) {
        if (name == null) throw missing(id, "name");
        return name;
    }

    public CharSequence getText(int id) {
        CharSequence text = value(id).coerceToString();
        if (text == null) throw missing(id, "text");
        return text;
    }

    public CharSequence getText(int id, CharSequence fallback) {
        try {
            return id == 0 ? fallback : getText(id);
        } catch (NotFoundException error) {
            return fallback;
        }
    }

    public String getString(int id) { return getText(id).toString(); }

    public String getString(int id, Object... args) {
        return org.wasmandroid.ui.StringFormat.format(getString(id), args);
    }

    public String[] getStringArray(int id) {
        String[] result = assets.resourceStrings(id);
        if (result == null) throw missing(id, "string array");
        return result;
    }

    public CharSequence[] getTextArray(int id) { return getStringArray(id); }

    public int[] getIntArray(int id) {
        int[] result = assets.resourceInts(id);
        if (result == null) throw missing(id, "integer array");
        return result;
    }

    public int getInteger(int id) {
        TypedValue value = value(id);
        if (value.type < TypedValue.TYPE_FIRST_INT || value.type > TypedValue.TYPE_LAST_INT) {
            throw missing(id, "integer");
        }
        return value.data;
    }

    public boolean getBoolean(int id) { return getInteger(id) != 0; }

    public float getFloat(int id) {
        TypedValue value = value(id);
        if (value.type != TypedValue.TYPE_FLOAT) throw missing(id, "float");
        return value.getFloat();
    }

    private int dimensionData(int id) {
        TypedValue value = value(id);
        if (value.type != TypedValue.TYPE_DIMENSION) throw missing(id, "dimension");
        return value.data;
    }

    public float getDimension(int id) {
        return TypedValue.complexToDimension(dimensionData(id), metrics);
    }

    public int getDimensionPixelSize(int id) {
        return TypedValue.complexToDimensionPixelSize(dimensionData(id), metrics);
    }

    public int getDimensionPixelOffset(int id) {
        return TypedValue.complexToDimensionPixelOffset(dimensionData(id), metrics);
    }

    public float getFraction(int id, int base, int parentBase) {
        TypedValue value = value(id);
        if (value.type != TypedValue.TYPE_FRACTION) throw missing(id, "fraction");
        return value.getFraction(base, parentBase);
    }

    public int getColor(int id) { return getColor(id, null); }

    public int getColor(int id, Theme theme) { return getColorStateList(id, theme).getDefaultColor(); }

    public ColorStateList getColorStateList(int id) { return getColorStateList(id, null); }

    public ColorStateList getColorStateList(int id, Theme theme) {
        TypedValue value = value(id);
        if (value.isColorType()) return ColorStateList.valueOf(value.data);
        try (XmlResourceParser parser = openXml(id, value)) {
            return ColorStateList.createFromXml(this, parser, theme);
        } catch (IOException | org.xmlpull.v1.XmlPullParserException error) {
            throw new NotFoundException("Cannot load color " + id, error);
        }
    }

    public Drawable getDrawable(int id) { return getDrawable(id, null); }

    public Drawable getDrawable(int id, Theme theme) { return getDrawableForDensity(id, 0, theme); }

    public Drawable getDrawableForDensity(int id, int density, Theme theme) {
        TypedValue value = new TypedValue();
        getValueForDensity(id, density, value, true);
        return loadDrawable(id, value, theme);
    }

    Drawable loadDrawable(int id, TypedValue value, Theme theme) {
        if (id == ID_NULL) return loadDrawableValue(id, value, theme);
        if (loadingDrawables.contains(id)) throw new NotFoundException("Recursive drawable " + id);
        loadingDrawables.add(id);
        try {
            return loadDrawableValue(id, value, theme);
        } finally {
            loadingDrawables.remove(loadingDrawables.size() - 1);
        }
    }

    private Drawable loadDrawableValue(int id, TypedValue value, Theme theme) {
        if (value.isColorType()) return new ColorDrawable(value.data);
        if (value.type != TypedValue.TYPE_STRING || value.string == null) throw missing(id, "drawable");
        String path = value.string.toString();
        try {
            if (path.endsWith(".xml")) {
                try (XmlResourceParser parser = openXml(id, value)) {
                    return Drawable.createFromXml(this, parser, theme);
                }
            }
            byte[] bytes = assets.readNonAsset(value.assetCookie, path);
            android.graphics.Bitmap bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
            if (bitmap == null) throw missing(id, "bitmap");
            bitmap.setDensity(value.density == TypedValue.DENSITY_NONE ? 0
                    : value.density == TypedValue.DENSITY_DEFAULT ? 160 : value.density);
            return new BitmapDrawable(this, bitmap);
        } catch (IOException | org.xmlpull.v1.XmlPullParserException error) {
            throw new NotFoundException("Cannot load drawable " + path, error);
        }
    }

    public XmlResourceParser getLayout(int id) { return getXml(id); }
    public XmlResourceParser getAnimation(int id) { return getXml(id); }

    public XmlResourceParser getXml(int id) {
        try {
            return openXml(id, value(id));
        } catch (IOException error) {
            throw new NotFoundException("Cannot open binary XML " + id, error);
        }
    }

    private XmlResourceParser openXml(int id, TypedValue value) throws IOException {
        if (value.type != TypedValue.TYPE_STRING || value.string == null) throw missing(id, "XML");
        return assets.openXmlResourceParser(value.assetCookie, value.string.toString(), id);
    }

    public TypedArray obtainAttributes(AttributeSet set, int[] attrs) {
        return resolveAttributes(asParser(set), attrs, null, 0, 0);
    }

    private TypedArray resolveAttributes(XmlBlock.Parser parser, int[] attrs, Theme theme,
                                        int defaultAttr, int defaultStyle) {
        TypedArray result = new TypedArray(this, theme, parser, attrs.length);
        if (theme == null && parser == null) return result;

        // AssetManager's native attribute finders scan IDs in ascending order.
        // Preserve the caller's array order in the returned TypedArray.
        int[] order = attributeOrder(attrs);
        int[] ids = attrs;
        int[] values = result.data;
        int[] indices = result.indices;
        if (order != null) {
            ids = new int[attrs.length];
            values = new int[result.data.length];
            indices = new int[result.indices.length];
            for (int i = 0; i < ids.length; i++) ids[i] = attrs[order[i]];
        }
        if (theme == null) assets.retrieveAttributes(parser, ids, values, indices);
        else assets.applyStyle(theme.ptr(), defaultAttr, defaultStyle, parser, ids, values, indices);

        if (order != null) {
            for (int i = 0; i < attrs.length; i++) {
                System.arraycopy(values, i * TypedArray.ENTRY_SIZE, result.data,
                        order[i] * TypedArray.ENTRY_SIZE, TypedArray.ENTRY_SIZE);
            }
            for (int i = 0; i < attrs.length; i++) {
                if (result.hasValueOrEmpty(i)) result.indices[++result.indices[0]] = i;
            }
        }
        return result;
    }

    private static int[] attributeOrder(int[] attrs) {
        boolean sorted = true;
        for (int i = 1; i < attrs.length; i++) {
            if ((attrs[i - 1] ^ Integer.MIN_VALUE) > (attrs[i] ^ Integer.MIN_VALUE)) {
                sorted = false;
                break;
            }
        }
        if (sorted) return null;
        int[] order = new int[attrs.length];
        for (int i = 0; i < order.length; i++) {
            int j = i;
            int id = attrs[i] ^ Integer.MIN_VALUE;
            while (j > 0 && (attrs[order[j - 1]] ^ Integer.MIN_VALUE) > id) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
        return order;
    }

    public TypedArray obtainTypedArray(int resId) {
        int[] data = assets.resourceArray(resId);
        if (data == null) throw missing(resId, "typed array");
        TypedArray result = new TypedArray(this, null, null, data.length / TypedArray.ENTRY_SIZE);
        System.arraycopy(data, 0, result.data, 0, data.length);
        for (int i = 0; i < result.length(); i++) {
            if (result.hasValueOrEmpty(i)) result.indices[++result.indices[0]] = i;
        }
        return result;
    }

    private static XmlBlock.Parser asParser(AttributeSet set) {
        if (set == null) return null;
        if (set instanceof XmlBlock.Parser) return (XmlBlock.Parser) set;
        throw new IllegalArgumentException("Expected a binary resource AttributeSet");
    }

    public Theme newTheme() {
        if (closed) throw new IllegalStateException("Resources are closed");
        Theme theme = new Theme();
        themes.add(theme);
        return theme;
    }

    public final class Theme {
        private long nativeTheme = assets.createTheme();
        private Theme() {}
        public Resources getResources() { return Resources.this; }
        public void applyStyle(int id, boolean force) { assets.applyThemeStyle(ptr(), id, force); }
        public boolean resolveAttribute(int id, TypedValue out, boolean resolveRefs) {
            return assets.getThemeValue(ptr(), id, out, resolveRefs);
        }
        public void setTo(Theme other) {
            assets.copyTheme(ptr(), other.getResources().assets, other.ptr());
        }
        public TypedArray obtainStyledAttributes(int[] attrs) {
            return obtainStyledAttributes(null, attrs, 0, 0);
        }
        public TypedArray obtainStyledAttributes(int id, int[] attrs) {
            return obtainStyledAttributes(null, attrs, 0, id);
        }
        public TypedArray obtainStyledAttributes(AttributeSet set, int[] attrs, int defAttr, int defRes) {
            return resolveAttributes(asParser(set), attrs, this, defAttr, defRes);
        }
        private long ptr() {
            if (nativeTheme == 0) throw new IllegalStateException("Theme is closed");
            return nativeTheme;
        }
        private void release() {
            AssetManager.releaseTheme(nativeTheme);
            nativeTheme = 0;
        }
    }

    @Override public void close() {
        if (closed) return;
        closed = true;
        for (Theme theme : themes) theme.release();
        themes.clear();
        if (ownedAssets != null) {
            assets.close();
            for (ApkAssets apk : ownedAssets) apk.close();
        }
    }

    private static NotFoundException missing(int id, String type) {
        return new NotFoundException("No " + type + " for resource #0x" + Integer.toHexString(id));
    }
}
