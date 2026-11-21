package android.content;

import android.os.Parcel;
import android.os.Parcelable;

public final class ComponentName implements Parcelable, Cloneable, Comparable<ComponentName> {
    private final String packageName, className;
    public ComponentName(String pkg, String cls) {
        if (pkg == null || cls == null) throw new NullPointerException();
        packageName = pkg; className = cls;
    }
    public ComponentName(Context context, String cls) { this(context.getPackageName(), cls); }
    public ComponentName(Context context, Class<?> cls) { this(context, cls.getName()); }
    public ComponentName(Parcel parcel) { this(parcel.readString(), parcel.readString()); }
    public static ComponentName createRelative(String pkg, String cls) {
        if (cls == null || cls.length() == 0) throw new IllegalArgumentException("Empty class name");
        return new ComponentName(pkg, cls.charAt(0) == '.' ? pkg + cls : cls);
    }
    public static ComponentName createRelative(Context context, String cls) { return createRelative(context.getPackageName(), cls); }
    public String getPackageName() { return packageName; }
    public String getClassName() { return className; }
    public String getShortClassName() {
        int length = packageName.length();
        return className.length() > length && className.startsWith(packageName) && className.charAt(length) == '.'
                ? className.substring(length) : className;
    }
    public String flattenToString() { return packageName + "/" + className; }
    public String flattenToShortString() { return packageName + "/" + getShortClassName(); }
    public String toShortString() { return "{" + flattenToShortString() + "}"; }
    public static ComponentName unflattenFromString(String value) {
        int slash = value.indexOf('/');
        if (slash < 1 || slash == value.length() - 1) return null;
        return createRelative(value.substring(0, slash), value.substring(slash + 1));
    }
    @Override public String toString() { return "ComponentInfo{" + flattenToString() + "}"; }
    @Override public ComponentName clone() { return new ComponentName(packageName, className); }
    @Override public boolean equals(Object obj) {
        if (!(obj instanceof ComponentName)) return false;
        ComponentName other = (ComponentName) obj;
        return packageName.equals(other.packageName) && className.equals(other.className);
    }
    @Override public int hashCode() { return packageName.hashCode() + className.hashCode(); }
    @Override public int compareTo(ComponentName other) {
        int result = packageName.compareTo(other.packageName);
        return result != 0 ? result : className.compareTo(other.className);
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) { out.writeString(packageName); out.writeString(className); }
    public static final Parcelable.Creator<ComponentName> CREATOR = new Parcelable.Creator<ComponentName>() {
        public ComponentName createFromParcel(Parcel in) { return new ComponentName(in); }
        public ComponentName[] newArray(int size) { return new ComponentName[size]; }
    };
}
