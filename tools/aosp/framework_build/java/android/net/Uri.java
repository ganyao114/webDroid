package android.net;

import android.os.Parcel;
import android.os.Parcelable;

public abstract class Uri implements Parcelable, Comparable<Uri> {
    public static final Uri EMPTY = parse("");
    protected Uri() {}
    public static Uri parse(String value) {
        if (value == null) throw new NullPointerException("uri");
        return new StringUri(value);
    }
    public abstract String getScheme();
    @Override public abstract String toString();
    @Override public final int compareTo(Uri other) { return toString().compareTo(other.toString()); }
    @Override public final boolean equals(Object other) { return other instanceof Uri && toString().equals(other.toString()); }
    @Override public final int hashCode() { return toString().hashCode(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel parcel, int flags) { parcel.writeString(toString()); }
    public static final Parcelable.Creator<Uri> CREATOR = new Parcelable.Creator<Uri>() {
        public Uri createFromParcel(Parcel parcel) { return parse(parcel.readString()); }
        public Uri[] newArray(int size) { return new Uri[size]; }
    };
    private static final class StringUri extends Uri {
        private final String value;
        StringUri(String value) { this.value = value; }
        @Override public String getScheme() { int at = value.indexOf(':'); return at < 0 ? null : value.substring(0, at); }
        @Override public String toString() { return value; }
    }
}
