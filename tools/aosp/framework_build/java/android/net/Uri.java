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
    public String getLastPathSegment() {
        String uri = toString();
        int end = uri.length(), query = uri.indexOf('?'), fragment = uri.indexOf('#');
        if (query >= 0) end = query;
        if (fragment >= 0) end = Math.min(end, fragment);
        while (end > 0 && uri.charAt(end - 1) == '/') --end;
        int start = uri.lastIndexOf('/', end - 1) + 1;
        return end <= start ? null : decode(uri.substring(start, end));
    }
    public static String decode(String value) {
        if (value == null || value.indexOf('%') < 0) return value;
        StringBuilder decoded = new StringBuilder(value.length());
        byte[] run = new byte[value.length() / 3];
        for (int i = 0; i < value.length();) {
            if (value.charAt(i) != '%') { decoded.append(value.charAt(i++)); continue; }
            int count = 0;
            while (i + 2 < value.length() && value.charAt(i) == '%') {
                int a = Character.digit(value.charAt(i + 1), 16), b = Character.digit(value.charAt(i + 2), 16);
                if ((a | b) < 0) break;
                run[count++] = (byte) (16 * a + b);
                i += 3;
            }
            if (count == 0) { decoded.append('\ufffd'); i++; }
            else decoded.append(new String(run, 0, count, java.nio.charset.StandardCharsets.UTF_8));
        }
        return decoded.toString();
    }
    public static String encode(String value) { return encode(value, null); }
    public static String encode(String value, String allowed) {
        if (value == null) return null;
        StringBuilder encoded = new StringBuilder(value.length());
        String digits = "0123456789ABCDEF";
        for (byte item : value.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
            int c = item & 0xff;
            if (c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9'
                    || "_-!.~'()*".indexOf(c) >= 0 || c < 128 && allowed != null && allowed.indexOf(c) >= 0) {
                encoded.append((char) c);
            } else {
                encoded.append('%').append(digits.charAt(c >>> 4)).append(digits.charAt(c & 15));
            }
        }
        return encoded.toString();
    }
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
