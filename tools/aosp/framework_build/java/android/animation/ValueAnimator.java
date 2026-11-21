package android.animation;

import android.os.SystemClock;
import android.view.Choreographer;
import android.view.animation.AccelerateDecelerateInterpolator;
import java.util.ArrayList;

public class ValueAnimator extends Animator implements Choreographer.FrameCallback {
    public static final int INFINITE = -1, RESTART = 1, REVERSE = 2;
    public interface AnimatorUpdateListener { void onAnimationUpdate(ValueAnimator animation); }
    private final ArrayList<AnimatorUpdateListener> updates = new ArrayList<>();
    private long duration = 300, delay, startTime = -1, seekTime;
    private long iteration;
    private int repeatCount, mode = RESTART;
    private boolean started, running, reverse, seekPending;
    private float fraction;
    private float[] floats;
    private int[] ints;
    private Object value;
    private TimeInterpolator interpolator = new AccelerateDecelerateInterpolator();

    public static ValueAnimator ofFloat(float... values) {
        ValueAnimator animator = new ValueAnimator();
        animator.setFloatValues(values);
        return animator;
    }
    public static ValueAnimator ofInt(int... values) {
        ValueAnimator animator = new ValueAnimator();
        animator.setIntValues(values);
        return animator;
    }
    public void setFloatValues(float... values) {
        floats = values.length == 1 ? new float[] {0, values[0]} : values.clone();
        ints = null;
    }
    public void setIntValues(int... values) {
        ints = values.length == 1 ? new int[] {0, values[0]} : values.clone();
        floats = null;
    }
    @Override public ValueAnimator setDuration(long milliseconds) {
        if (milliseconds < 0) throw new IllegalArgumentException("Negative animation duration");
        duration = milliseconds;
        return this;
    }
    @Override public long getDuration() { return duration; }
    @Override public void setStartDelay(long milliseconds) { delay = Math.max(0, milliseconds); }
    @Override public long getStartDelay() { return delay; }
    public void setRepeatCount(int count) { repeatCount = count; }
    public int getRepeatCount() { return repeatCount; }
    public void setRepeatMode(int mode) { this.mode = mode; }
    public int getRepeatMode() { return mode; }
    @Override public void setInterpolator(TimeInterpolator value) { interpolator = value; }
    @Override public TimeInterpolator getInterpolator() { return interpolator; }
    public void addUpdateListener(AnimatorUpdateListener listener) { if (listener != null) updates.add(listener); }
    public void removeUpdateListener(AnimatorUpdateListener listener) { updates.remove(listener); }
    public void removeAllUpdateListeners() { updates.clear(); }
    public Object getAnimatedValue() { return value; }
    public Object getAnimatedValue(String name) { return value; }
    public float getAnimatedFraction() { return fraction; }
    @Override public boolean isRunning() { return running; }
    @Override public boolean isStarted() { return started; }

    @Override public void start() { start(false); }
    private void start(boolean reverse) {
        Choreographer choreographer = Choreographer.getInstance();
        choreographer.removeFrameCallback(this);
        this.reverse = reverse;
        started = true;
        running = false;
        iteration = 0;
        startTime = -1;
        if (delay == 0 || seekPending) {
            begin();
            if (started) animateTime(seekPending ? seekTime : 0);
        }
        if (started) choreographer.postFrameCallbackDelayed(this, seekPending ? 0 : delay);
    }
    private void begin() {
        if (running) return;
        running = true;
        for (AnimatorListener listener : listeners()) listener.onAnimationStart(this, reverse);
    }
    @Override public void doFrame(long frameTimeNanos) {
        if (!started) return;
        long time = frameTimeNanos / 1_000_000;
        if (startTime < 0) {
            startTime = time - (seekPending ? seekTime : 0);
            seekPending = false;
            begin();
        }
        if (!started) return;
        boolean ended = animateTime(Math.max(0, time - startTime));
        if (!started) return;
        if (ended) finish();
        else Choreographer.getInstance().postFrameCallback(this);
    }
    private boolean animateTime(long elapsed) {
        float overall = duration == 0 ? 1 : (float) elapsed / duration;
        long nextIteration = (long) overall;
        boolean ending = duration == 0 || repeatCount != INFINITE && nextIteration > repeatCount;
        if (ending) nextIteration = Math.max(0, repeatCount);
        if (started && !ending && nextIteration > iteration) {
            for (AnimatorListener listener : listeners()) listener.onAnimationRepeat(this);
        }
        iteration = nextIteration;
        float current = ending ? 1 : overall - nextIteration;
        animate(reverse ^ (mode == REVERSE && (iteration & 1) != 0) ? 1 - current : current);
        return ending;
    }
    private void animate(float progress) {
        fraction = interpolator == null ? progress : interpolator.getInterpolation(progress);
        int count = floats != null ? floats.length : ints == null ? 0 : ints.length;
        if (count > 0) {
            float i = fraction * (count - 1);
            int from = Math.max(0, Math.min(count - 2, (int) i));
            int to = Math.min(count - 1, from + 1);
            float local = i - from;
            if (floats != null) value = floats[from] + (floats[to] - floats[from]) * local;
            else value = ints[from] + (int) ((ints[to] - (float) ints[from]) * local);
        }
        AnimatorUpdateListener[] snapshot = new AnimatorUpdateListener[updates.size()];
        for (int i = 0; i < snapshot.length; i++) snapshot[i] = updates.get(i);
        for (AnimatorUpdateListener listener : snapshot) listener.onAnimationUpdate(this);
    }
    public long getCurrentPlayTime() {
        if (seekPending) return seekTime;
        return startTime < 0 ? 0 : Math.max(0, SystemClock.uptimeMillis() - startTime);
    }
    public void setCurrentPlayTime(long playTime) {
        seekTime = Math.max(0, playTime);
        seekPending = startTime < 0;
        if (!seekPending) startTime = SystemClock.uptimeMillis() - seekTime;
        animateTime(seekTime);
    }
    public void setCurrentFraction(float fraction) { setCurrentPlayTime((long) (duration * Math.max(0, fraction))); }
    public void reverse() {
        if (!started) start(true);
        else {
            long elapsed = getCurrentPlayTime();
            reverse = !reverse;
            setCurrentPlayTime(duration == 0 ? 0 : elapsed / duration * duration + duration - elapsed % duration);
        }
    }
    @Override public void cancel() {
        if (!started) return;
        begin();
        for (AnimatorListener listener : listeners()) listener.onAnimationCancel(this);
        finish();
    }
    @Override public void end() {
        if (!started) started = true;
        begin();
        long finalIteration = repeatCount == INFINITE ? iteration : Math.max(0, repeatCount);
        animate(reverse ^ (mode == REVERSE && (finalIteration & 1) != 0) ? 0 : 1);
        finish();
    }
    private void finish() {
        Choreographer.getInstance().removeFrameCallback(this);
        running = started = seekPending = false;
        startTime = -1;
        for (AnimatorListener listener : listeners()) listener.onAnimationEnd(this, reverse);
    }
}
