package android.os;

public final class ServiceManager {
    public static IBinder getService(String name) {
        if (!"platform_compat".equals(name)) {
            throw new AssertionError("Unexpected service " + name);
        }
        return null;
    }
}
