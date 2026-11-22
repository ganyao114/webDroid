package android.window;

public final class BackEvent {
    public static final int EDGE_LEFT = 0, EDGE_RIGHT = 1, EDGE_NONE = 2;
    private final float x, y, progress;
    private final int edge;
    private final long frameTimeMillis;

    public BackEvent(float x, float y, float progress, int edge) { this(x, y, progress, edge, 0); }
    public BackEvent(float x, float y, float progress, int edge, long frameTimeMillis) {
        this.x = x;
        this.y = y;
        this.progress = progress;
        this.edge = edge;
        this.frameTimeMillis = frameTimeMillis;
    }

    public float getTouchX() { return x; }
    public float getTouchY() { return y; }
    public float getProgress() { return progress; }
    public int getSwipeEdge() { return edge; }
    public long getFrameTimeMillis() { return frameTimeMillis; }
}
