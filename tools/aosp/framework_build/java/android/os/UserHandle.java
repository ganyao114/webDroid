package android.os;

public final class UserHandle implements Parcelable {
    public static final int USER_SYSTEM = 0;
    public static final UserHandle SYSTEM = new UserHandle(USER_SYSTEM);
    private final int identifier;
    public UserHandle(int identifier) { this.identifier = identifier; }
    public static UserHandle of(int id) { return id == 0 ? SYSTEM : new UserHandle(id); }
    public static UserHandle getUserHandleForUid(int uid) { return of(uid / 100000); }
    public int getIdentifier() { return identifier; }
    @Override public boolean equals(Object obj) { return obj instanceof UserHandle && ((UserHandle) obj).identifier == identifier; }
    @Override public int hashCode() { return identifier; }
    @Override public String toString() { return "UserHandle{" + identifier + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel parcel, int flags) { parcel.writeInt(identifier); }
    public static final Parcelable.Creator<UserHandle> CREATOR = new Parcelable.Creator<UserHandle>() {
        public UserHandle createFromParcel(Parcel in) { return of(in.readInt()); }
        public UserHandle[] newArray(int size) { return new UserHandle[size]; }
    };
}
