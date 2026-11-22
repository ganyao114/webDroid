package android.view;

import android.os.Parcel;
import android.os.Parcelable;

import java.util.ArrayList;

public final class MotionEvent extends InputEvent {
    public static final int ACTION_MASK = 0xff, ACTION_POINTER_INDEX_MASK = 0xff00, ACTION_POINTER_INDEX_SHIFT = 8;
    public static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3;
    public static final int ACTION_OUTSIDE = 4, ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6;
    public static final int ACTION_HOVER_MOVE = 7, ACTION_SCROLL = 8, ACTION_HOVER_ENTER = 9, ACTION_HOVER_EXIT = 10;
    public static final int ACTION_BUTTON_PRESS = 11, ACTION_BUTTON_RELEASE = 12;
    public static final int BUTTON_PRIMARY = 1, BUTTON_SECONDARY = 2, BUTTON_STYLUS_PRIMARY = 32;
    public static final int CLASSIFICATION_NONE = 0, CLASSIFICATION_AMBIGUOUS_GESTURE = 1,
            CLASSIFICATION_DEEP_PRESS = 2;
    public static final int FLAG_IS_GENERATED_GESTURE = 0x8;
    public static final int AXIS_X = 0, AXIS_Y = 1, AXIS_PRESSURE = 2, AXIS_SIZE = 3, AXIS_VSCROLL = 9, AXIS_HSCROLL = 10;
    public static final int TOOL_TYPE_UNKNOWN = 0, TOOL_TYPE_FINGER = 1, TOOL_TYPE_STYLUS = 2, TOOL_TYPE_MOUSE = 3;
    private final long downTime;
    private long eventTime;
    private int action, metaState, buttonState, edgeFlags, actionButton, flags;
    private int deviceId;
    private final int[] ids;
    private float[] x, y;
    private float pressure = 1f, size = 1f;
    private float offsetX, offsetY, hScroll, vScroll;
    private ArrayList<Sample> history;

    private static final class Sample {
        final long time;
        final float[] x, y;
        Sample(long time, float[] x, float[] y) { this.time = time; this.x = x; this.y = y; }
    }

    private MotionEvent(long downTime, long eventTime, int action, int[] ids,
                        float[] x, float[] y, int metaState) {
        this.downTime = downTime; this.eventTime = eventTime; this.action = action;
        this.ids = ids; this.x = x; this.y = y; this.metaState = metaState;
        setSource(0x1002);
    }
    public static MotionEvent obtain(long downTime, long time, int action, float x, float y, int metaState) {
        return new MotionEvent(downTime, time, action, new int[] {0}, new float[] {x}, new float[] {y}, metaState);
    }
    public static MotionEvent obtain(long downTime, long time, int action, float x, float y, float pressure,
                                     float size, int meta, float xPrecision, float yPrecision, int device, int edges) {
        MotionEvent event = obtain(downTime, time, action, x, y, meta);
        event.pressure = pressure; event.size = size; event.edgeFlags = edges;
        event.deviceId = device;
        return event;
    }
    public static MotionEvent obtain(MotionEvent source) { return copy(source, true); }
    public static MotionEvent obtainNoHistory(MotionEvent source) { return copy(source, false); }
    private static MotionEvent copy(MotionEvent source, boolean history) {
        MotionEvent event = new MotionEvent(source.downTime, source.eventTime, source.action,
                source.ids.clone(), source.x.clone(), source.y.clone(), source.metaState);
        event.offsetX = source.offsetX; event.offsetY = source.offsetY;
        event.hScroll = source.hScroll; event.vScroll = source.vScroll;
        event.pressure = source.pressure; event.size = source.size;
        event.edgeFlags = source.edgeFlags; event.buttonState = source.buttonState;
        event.actionButton = source.actionButton; event.flags = source.flags;
        event.deviceId = source.deviceId;
        event.setSource(source.getSource());
        event.setDisplayId(source.getDisplayId());
        event.setTainted(source.isTainted());
        if (history && source.history != null) {
            event.history = new ArrayList<>();
            for (Sample sample : source.history) {
                event.history.add(new Sample(sample.time, sample.x.clone(), sample.y.clone()));
            }
        }
        return event;
    }

    public static MotionEvent fromHost(long downTime, long time, int action, int[] ids, float[] x, float[] y,
                                       int meta, float dx, float dy) {
        MotionEvent event = new MotionEvent(downTime, time, action, ids, x, y, meta);
        event.hScroll = dx; event.vScroll = dy;
        return event;
    }

    @Override public long getEventTime() { return eventTime; }
    @Override public int getDeviceId() { return deviceId; }
    @Override public MotionEvent copy() { return obtain(this); }
    @Override public void cancel() { action = ACTION_CANCEL; }
    public long getDownTime() { return downTime; }
    public int getAction() { return action; }
    public int getActionMasked() { return action & ACTION_MASK; }
    public int getActionIndex() { return (action & ACTION_POINTER_INDEX_MASK) >> ACTION_POINTER_INDEX_SHIFT; }
    public void setAction(int action) { this.action = action; }
    public int getMetaState() { return metaState; }
    public int getButtonState() { return buttonState; }
    public int getActionButton() { return actionButton; }
    public void setActionButton(int actionButton) { this.actionButton = actionButton; }
    public int getFlags() { return flags; }
    public int getClassification() { return CLASSIFICATION_NONE; }
    public int getEdgeFlags() { return edgeFlags; }
    public void setEdgeFlags(int flags) { edgeFlags = flags; }
    public int getPointerCount() { return ids.length; }
    public int getPointerId(int index) { return ids[index]; }
    public int findPointerIndex(int id) {
        for (int i = 0; i < ids.length; ++i) if (ids[i] == id) return i;
        return -1;
    }
    public int getToolType(int pointerIndex) { return TOOL_TYPE_FINGER; }
    public float getX() { return getX(0); }
    public float getY() { return getY(0); }
    public float getX(int index) { return x[index] + offsetX; }
    public float getY(int index) { return y[index] + offsetY; }
    public float getRawX() { return getRawX(0); }
    public float getRawY() { return getRawY(0); }
    public float getRawX(int index) { return x[index]; }
    public float getRawY(int index) { return y[index]; }
    public float getPressure() { return pressure; }
    public float getPressure(int index) { return pressure; }
    public float getSize() { return size; }
    public float getSize(int index) { return size; }
    public float getXPrecision() { return 1; }
    public float getYPrecision() { return 1; }
    public void offsetLocation(float dx, float dy) { offsetX += dx; offsetY += dy; }
    public void setLocation(float x, float y) { offsetLocation(x - getX(), y - getY()); }
    public float getAxisValue(int axis) { return getAxisValue(axis, 0); }
    public float getAxisValue(int axis, int index) {
        switch (axis) {
            case AXIS_X: return getX(index);
            case AXIS_Y: return getY(index);
            case AXIS_HSCROLL: return hScroll;
            case AXIS_VSCROLL: return vScroll;
            case AXIS_PRESSURE: return pressure;
            case AXIS_SIZE: return size;
            default: return 0;
        }
    }
    public int getHistorySize() { return history == null ? 0 : history.size(); }
    public long getHistoricalEventTime(int pos) { return history.get(pos).time; }
    public float getHistoricalX(int pos) { return getHistoricalX(0, pos); }
    public float getHistoricalY(int pos) { return getHistoricalY(0, pos); }
    public float getHistoricalX(int index, int pos) { return history.get(pos).x[index] + offsetX; }
    public float getHistoricalY(int index, int pos) { return history.get(pos).y[index] + offsetY; }
    public void addBatch(long time, float x, float y, float pressure, float size, int metaState) {
        if (ids.length != 1) throw new IllegalStateException("addBatch requires one pointer");
        if (history == null) history = new ArrayList<>();
        history.add(new Sample(eventTime, this.x, this.y));
        eventTime = time;
        this.x = new float[] {x - offsetX}; this.y = new float[] {y - offsetY};
        this.pressure = pressure; this.size = size; this.metaState |= metaState;
    }

    @Override public void writeToParcel(Parcel out, int parcelFlags) {
        out.writeInt(PARCEL_TOKEN_MOTION_EVENT);
        out.writeLong(downTime);
        out.writeLong(eventTime);
        out.writeInt(action);
        out.writeIntArray(ids);
        out.writeFloatArray(x);
        out.writeFloatArray(y);
        out.writeInt(metaState);
        out.writeInt(deviceId);
        out.writeInt(getSource());
        out.writeInt(getDisplayId());
        out.writeBoolean(isTainted());
        out.writeInt(buttonState);
        out.writeInt(edgeFlags);
        out.writeInt(actionButton);
        out.writeInt(flags);
        out.writeFloat(pressure);
        out.writeFloat(size);
        out.writeFloat(offsetX);
        out.writeFloat(offsetY);
        out.writeFloat(hScroll);
        out.writeFloat(vScroll);
        out.writeInt(getHistorySize());
        if (history != null) {
            for (Sample sample : history) {
                out.writeLong(sample.time);
                out.writeFloatArray(sample.x);
                out.writeFloatArray(sample.y);
            }
        }
    }

    public static final Parcelable.Creator<MotionEvent> CREATOR =
            new Parcelable.Creator<MotionEvent>() {
        @Override public MotionEvent createFromParcel(Parcel in) {
            if (in.readInt() != PARCEL_TOKEN_MOTION_EVENT) {
                throw new android.os.BadParcelableException("Expected a motion event");
            }
            MotionEvent event = new MotionEvent(in.readLong(), in.readLong(), in.readInt(),
                    in.createIntArray(), in.createFloatArray(), in.createFloatArray(), in.readInt());
            event.deviceId = in.readInt();
            event.setSource(in.readInt());
            event.setDisplayId(in.readInt());
            event.setTainted(in.readBoolean());
            event.buttonState = in.readInt();
            event.edgeFlags = in.readInt();
            event.actionButton = in.readInt();
            event.flags = in.readInt();
            event.pressure = in.readFloat();
            event.size = in.readFloat();
            event.offsetX = in.readFloat();
            event.offsetY = in.readFloat();
            event.hScroll = in.readFloat();
            event.vScroll = in.readFloat();
            int count = in.readInt();
            if (count > 0) {
                event.history = new ArrayList<>();
                for (int i = 0; i < count; i++) {
                    event.history.add(new Sample(in.readLong(), in.createFloatArray(),
                            in.createFloatArray()));
                }
            }
            return event;
        }

        @Override public MotionEvent[] newArray(int size) { return new MotionEvent[size]; }
    };
}
