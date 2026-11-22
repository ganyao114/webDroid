package android.view;

import android.util.SparseArray;

public final class VelocityTracker {
    public static final int VELOCITY_TRACKER_STRATEGY_DEFAULT = -1;
    public @interface VelocityTrackerStrategy {}
    private static final int CAPACITY = 20;
    private static final int HORIZON_MS = 100;
    private static final int STOP_MS = 40;
    private final SparseArray<History> pointers = new SparseArray<>();
    private int activeId = -1;
    private long previousTime;

    private static final class History {
        final long[] time = new long[CAPACITY];
        final float[] x = new float[CAPACITY], y = new float[CAPACITY];
        int count, next;
        float vx, vy;
        void add(long millis, float x, float y) {
            if (count != 0 && time[(next + CAPACITY - 1) % CAPACITY] == millis) {
                int i = (next + CAPACITY - 1) % CAPACITY;
                this.x[i] = x; this.y[i] = y;
                return;
            }
            time[next] = millis; this.x[next] = x; this.y[next] = y;
            next = (next + 1) % CAPACITY;
            count = Math.min(CAPACITY, count + 1);
        }
        void compute(int units, float max) {
            vx = vy = 0;
            if (count < 2) return;
            int last = (next + CAPACITY - 1) % CAPACITY;
            long since = time[last];
            double st = 0, stt = 0, sx = 0, sy = 0, stx = 0, sty = 0;
            int n = 0;
            for (int i = 0; i < count; i++) {
                int index = (last + CAPACITY - i) % CAPACITY;
                double t = time[index] - since;
                if (t < -HORIZON_MS) break;
                st += t; stt += t * t; sx += x[index]; sy += y[index];
                stx += t * x[index]; sty += t * y[index];
                n++;
            }
            double denom = n * stt - st * st;
            if (denom <= 0) return;
            vx = clamp((float) (units * (n * stx - st * sx) / denom), max);
            vy = clamp((float) (units * (n * sty - st * sy) / denom), max);
        }
        private static float clamp(float value, float max) { return Math.max(-max, Math.min(max, value)); }
    }

    private VelocityTracker() {}
    public static VelocityTracker obtain() { return new VelocityTracker(); }
    public static VelocityTracker obtain(int strategy) { return obtain(); }
    public void recycle() { clear(); }
    public void clear() { pointers.clear(); activeId = -1; previousTime = 0; }
    public void addMovement(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_CANCEL) clear();
        if (action == MotionEvent.ACTION_CANCEL) return;
        long time = event.getEventTime();
        if (activeId != -1 && time - previousTime > STOP_MS && event.getHistorySize() == 0) clear();
        previousTime = time;
        if (activeId == -1) activeId = event.getPointerId(0);
        if (action == MotionEvent.ACTION_POINTER_DOWN) pointers.remove(event.getPointerId(event.getActionIndex()));
        for (int i = 0; i < event.getPointerCount(); i++) {
            int id = event.getPointerId(i);
            History history = pointers.get(id);
            if (history == null) { history = new History(); pointers.put(id, history); }
            for (int j = 0; j < event.getHistorySize(); j++) {
                history.add(event.getHistoricalEventTime(j), event.getHistoricalX(i, j), event.getHistoricalY(i, j));
            }
            history.add(time, event.getX(i), event.getY(i));
        }
        if (action == MotionEvent.ACTION_POINTER_UP && event.getPointerId(event.getActionIndex()) == activeId) {
            activeId = event.getPointerId(event.getActionIndex() == 0 ? 1 : 0);
        }
    }
    public void computeCurrentVelocity(int units) { computeCurrentVelocity(units, Float.MAX_VALUE); }
    public void computeCurrentVelocity(int units, float maxVelocity) {
        if (units <= 0 || maxVelocity < 0) throw new IllegalArgumentException("Invalid velocity scaling");
        for (int i = 0; i < pointers.size(); i++) pointers.valueAt(i).compute(units, maxVelocity);
    }
    public float getXVelocity() { return getXVelocity(activeId); }
    public float getYVelocity() { return getYVelocity(activeId); }
    public float getXVelocity(int id) { History h = pointers.get(id == -1 ? activeId : id); return h == null ? 0 : h.vx; }
    public float getYVelocity(int id) { History h = pointers.get(id == -1 ? activeId : id); return h == null ? 0 : h.vy; }
}
