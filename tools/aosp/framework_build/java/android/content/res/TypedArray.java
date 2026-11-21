package android.content.res;

import android.graphics.drawable.Drawable;
import android.util.TypedValue;

public final class TypedArray implements AutoCloseable {
    static final int ENTRY_SIZE = 7;
    final int[] data;
    final int[] indices;
    private final Resources resources;
    private final Resources.Theme theme;
    private XmlBlock.Parser parser;
    private boolean recycled;

    TypedArray(Resources resources, Resources.Theme theme, XmlBlock.Parser parser, int length) {
        this.resources = resources;
        this.theme = theme;
        this.parser = parser;
        data = new int[length * ENTRY_SIZE];
        indices = new int[length + 1];
    }

    public int length() { checkOpen(); return data.length / ENTRY_SIZE; }
    public int getIndexCount() { checkOpen(); return indices[0]; }

    public int getIndex(int at) {
        if (at < 0 || at >= getIndexCount()) throw new IndexOutOfBoundsException();
        return indices[at + 1];
    }

    public Resources getResources() { checkOpen(); return resources; }
    public String getPositionDescription() {
        checkOpen();
        return parser == null ? "" : parser.getPositionDescription();
    }

    private int offset(int index) {
        if (index < 0 || index >= length()) throw new IndexOutOfBoundsException();
        return index * ENTRY_SIZE;
    }

    public int getType(int index) { return data[offset(index)]; }
    public boolean hasValue(int index) { return getType(index) != TypedValue.TYPE_NULL; }
    public boolean hasValueOrEmpty(int index) {
        int offset = offset(index);
        return data[offset] != TypedValue.TYPE_NULL || data[offset + 1] == 1;
    }

    public int getSourceResourceId(int index, int fallback) {
        int source = data[offset(index) + 6];
        return source == 0 ? fallback : source;
    }

    public boolean getValue(int index, TypedValue out) {
        int offset = offset(index);
        if (data[offset] == TypedValue.TYPE_NULL) return false;
        out.type = data[offset];
        out.data = data[offset + 1];
        out.assetCookie = data[offset + 2];
        out.resourceId = data[offset + 3];
        out.changingConfigurations = data[offset + 4];
        out.density = data[offset + 5];
        out.usesFeatureFlags = false;
        if (out.type == TypedValue.TYPE_STRING) {
            out.string = out.assetCookie < 0 && parser != null ? parser.getPooledString(out.data)
                    : resources.getAssets().pooledString(out.assetCookie, out.data);
        } else {
            out.string = null;
        }
        return true;
    }

    public TypedValue peekValue(int index) {
        TypedValue result = new TypedValue();
        return getValue(index, result) ? result : null;
    }

    public int getResourceId(int index, int fallback) {
        int at = offset(index);
        if (data[at] == TypedValue.TYPE_NULL) return fallback;
        if (data[at + 3] != 0) return data[at + 3];
        return data[at] == TypedValue.TYPE_REFERENCE ? data[at + 1] : fallback;
    }

    public int getThemeAttributeId(int index, int fallback) {
        int at = offset(index);
        return data[at] == TypedValue.TYPE_ATTRIBUTE ? data[at + 1] : fallback;
    }

    public CharSequence getText(int index) {
        TypedValue value = peekValue(index);
        return value == null ? null : value.coerceToString();
    }

    public String getString(int index) {
        CharSequence value = getText(index);
        return value == null ? null : value.toString();
    }

    public String getNonResourceString(int index) {
        int at = offset(index);
        return data[at] == TypedValue.TYPE_STRING && data[at + 2] < 0 ? getString(index) : null;
    }

    public boolean getBoolean(int index, boolean fallback) {
        TypedValue value = peekValue(index);
        if (value == null) return fallback;
        if (isInteger(value.type)) return value.data != 0;
        String text = getString(index);
        return "true".equals(text == null ? null : text.toLowerCase()) || "1".equals(text);
    }

    public int getInt(int index, int fallback) {
        TypedValue value = peekValue(index);
        if (value == null) return fallback;
        return isInteger(value.type) ? value.data
                : com.android.internal.util.XmlUtils.convertValueToInt(value.coerceToString(), fallback);
    }

    public int getInteger(int index, int fallback) {
        int at = offset(index);
        if (data[at] == TypedValue.TYPE_NULL) return fallback;
        if (isInteger(data[at])) return data[at + 1];
        throw wrongType(index, "integer");
    }

    public float getFloat(int index, float fallback) {
        int at = offset(index);
        if (data[at] == TypedValue.TYPE_NULL) return fallback;
        if (data[at] == TypedValue.TYPE_FLOAT) return Float.intBitsToFloat(data[at + 1]);
        if (isInteger(data[at])) return data[at + 1];
        if (data[at] == TypedValue.TYPE_STRING) return org.wasmandroid.ui.NativeMath.parseFloat(getString(index));
        throw wrongType(index, "float");
    }

    public int getColor(int index, int fallback) {
        ColorStateList list = getColorStateList(index);
        return list == null ? fallback : list.getDefaultColor();
    }

    public ColorStateList getColorStateList(int index) {
        TypedValue value = peekValue(index);
        if (value == null) return null;
        if (isInteger(value.type)) return ColorStateList.valueOf(value.data);
        if (value.type == TypedValue.TYPE_STRING && value.resourceId != 0) {
            return resources.getColorStateList(value.resourceId, theme);
        }
        throw wrongType(index, "color");
    }

    public Drawable getDrawable(int index) {
        TypedValue value = peekValue(index);
        if (value == null) return null;
        return resources.loadDrawable(value.resourceId, value, theme);
    }

    public float getDimension(int index, float fallback) {
        int at = offset(index);
        if (data[at] == TypedValue.TYPE_NULL) return fallback;
        if (data[at] != TypedValue.TYPE_DIMENSION) throw wrongType(index, "dimension");
        return TypedValue.complexToDimension(data[at + 1], resources.getDisplayMetrics());
    }

    public int getDimensionPixelSize(int index, int fallback) {
        int at = offset(index);
        if (data[at] == TypedValue.TYPE_NULL) return fallback;
        if (data[at] != TypedValue.TYPE_DIMENSION) throw wrongType(index, "dimension");
        return TypedValue.complexToDimensionPixelSize(data[at + 1], resources.getDisplayMetrics());
    }

    public int getDimensionPixelOffset(int index, int fallback) {
        int at = offset(index);
        if (data[at] == TypedValue.TYPE_NULL) return fallback;
        if (data[at] != TypedValue.TYPE_DIMENSION) throw wrongType(index, "dimension");
        return TypedValue.complexToDimensionPixelOffset(data[at + 1], resources.getDisplayMetrics());
    }

    public int getLayoutDimension(int index, int fallback) {
        int at = offset(index);
        if (isInteger(data[at])) return data[at + 1];
        return getDimensionPixelSize(index, fallback);
    }

    public int getLayoutDimension(int index, String name) {
        if (!hasValue(index)) throw new IllegalArgumentException("Missing " + name + " at " + getPositionDescription());
        return getLayoutDimension(index, 0);
    }

    public float getFraction(int index, int base, int parentBase, float fallback) {
        int at = offset(index);
        if (data[at] == TypedValue.TYPE_NULL) return fallback;
        if (data[at] != TypedValue.TYPE_FRACTION) throw wrongType(index, "fraction");
        return TypedValue.complexToFraction(data[at + 1], base, parentBase);
    }

    public int getChangingConfigurations() {
        checkOpen();
        int configs = 0;
        for (int i = 0; i < data.length; i += ENTRY_SIZE) {
            if (data[i] != TypedValue.TYPE_NULL) configs |= data[i + 4];
        }
        return configs;
    }

    private static boolean isInteger(int type) {
        return type >= TypedValue.TYPE_FIRST_INT && type <= TypedValue.TYPE_LAST_INT;
    }

    private UnsupportedOperationException wrongType(int index, String type) {
        return new UnsupportedOperationException("Attribute " + index + " is not a " + type);
    }

    private void checkOpen() {
        if (recycled) throw new IllegalStateException("TypedArray is recycled");
    }

    public void recycle() {
        checkOpen();
        recycled = true;
        parser = null;
    }

    @Override public void close() { recycle(); }
}
