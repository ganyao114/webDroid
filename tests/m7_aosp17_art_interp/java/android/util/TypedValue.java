package android.util;

public final class TypedValue {
    public static final int TYPE_NULL = 0;
    public static final int TYPE_REFERENCE = 1;
    public static final int TYPE_STRING = 3;
    public static final int TYPE_FLOAT = 4;
    public static final int TYPE_FIRST_INT = 16;
    public static final int TYPE_LAST_INT = 31;

    public int type;
    public int data;
    public int assetCookie;
    public int resourceId;
    public int changingConfigurations;
    public int density;

    public void setFields(int type, int assetCookie, int data, int resourceId,
                          int changingConfigurations, int density, boolean clearString) {
        this.type = type;
        this.data = data;
        this.assetCookie = assetCookie;
        this.resourceId = resourceId;
        this.changingConfigurations = changingConfigurations;
        this.density = density;
        if (clearString) string = null;
    }

    public CharSequence string;

    public static String coerceToString(int type, int data) {
        switch (type) {
            case TYPE_NULL: return null;
            case TYPE_REFERENCE: return "@" + data;
            case 2: return "?" + data;
            case TYPE_FLOAT: return Float.toString(Float.intBitsToFloat(data));
            case 17: return "0x" + Integer.toHexString(data);
            case 18: return data != 0 ? "true" : "false";
            default:
                return type >= TYPE_FIRST_INT && type <= TYPE_LAST_INT
                        ? Integer.toString(data) : null;
        }
    }
}
