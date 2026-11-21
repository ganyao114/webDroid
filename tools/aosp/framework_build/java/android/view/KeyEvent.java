package android.view;

public class KeyEvent extends InputEvent {
    public static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MULTIPLE = 2;
    public static final int KEYCODE_UNKNOWN = 0, KEYCODE_BACK = 4, KEYCODE_DPAD_UP = 19, KEYCODE_DPAD_DOWN = 20;
    public static final int KEYCODE_DPAD_LEFT = 21, KEYCODE_DPAD_RIGHT = 22, KEYCODE_DPAD_CENTER = 23;
    public static final int KEYCODE_SPACE = 62, KEYCODE_ENTER = 66, KEYCODE_DEL = 67, KEYCODE_ESCAPE = 111;
    public static final int META_SHIFT_ON = 1, META_ALT_ON = 2, META_CTRL_ON = 0x1000, META_META_ON = 0x10000;
    public static final int FLAG_CANCELED = 0x20, FLAG_LONG_PRESS = 0x80;
    private final int action, code, repeat, meta, device, scan, flags;
    private final long downTime, eventTime;
    private boolean tracking;

    public interface Callback {
        boolean onKeyDown(int keyCode, KeyEvent event);
        boolean onKeyLongPress(int keyCode, KeyEvent event);
        boolean onKeyUp(int keyCode, KeyEvent event);
        boolean onKeyMultiple(int keyCode, int count, KeyEvent event);
    }

    public KeyEvent(int action, int code) {
        this(android.os.SystemClock.uptimeMillis(), android.os.SystemClock.uptimeMillis(), action, code, 0);
    }
    public KeyEvent(long downTime, long eventTime, int action, int keyCode, int repeat) {
        this(downTime, eventTime, action, keyCode, repeat, 0);
    }
    public KeyEvent(long downTime, long eventTime, int action, int keyCode, int repeat, int meta) {
        this(downTime, eventTime, action, keyCode, repeat, meta, 0, 0, 0, 0x101);
    }
    public KeyEvent(long downTime, long eventTime, int action, int keyCode, int repeat, int meta,
                    int device, int scan, int flags, int source) {
        this.downTime = downTime; this.eventTime = eventTime; this.action = action;
        code = keyCode; this.repeat = repeat; this.meta = meta; this.device = device;
        this.scan = scan; this.flags = flags; setSource(source);
    }
    public long getDownTime() { return downTime; }
    @Override public long getEventTime() { return eventTime; }
    @Override public int getDeviceId() { return device; }
    public int getAction() { return action; }
    public int getKeyCode() { return code; }
    public int getRepeatCount() { return repeat; }
    public int getMetaState() { return meta; }
    public int getModifiers() { return meta; }
    public int getScanCode() { return scan; }
    public int getFlags() { return flags; }
    public boolean isCanceled() { return (flags & FLAG_CANCELED) != 0; }
    public boolean isLongPress() { return (flags & FLAG_LONG_PRESS) != 0; }
    public boolean isTracking() { return tracking; }
    public boolean isShiftPressed() { return (meta & META_SHIFT_ON) != 0; }
    public boolean isAltPressed() { return (meta & META_ALT_ON) != 0; }
    public boolean isCtrlPressed() { return (meta & META_CTRL_ON) != 0; }
    public boolean isMetaPressed() { return (meta & META_META_ON) != 0; }
    public void startTracking() { tracking = true; }

    public final boolean dispatch(Callback callback) {
        switch (action) {
            case ACTION_DOWN:
                if (isLongPress() && callback.onKeyLongPress(code, this)) return true;
                return callback.onKeyDown(code, this);
            case ACTION_UP: return callback.onKeyUp(code, this);
            case ACTION_MULTIPLE: return callback.onKeyMultiple(code, repeat, this);
            default: return false;
        }
    }
}
