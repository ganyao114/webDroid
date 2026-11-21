package android.view;

public abstract class InputEvent {
    private int source;
    public abstract long getEventTime();
    public int getDeviceId() { return 0; }
    public int getSource() { return source; }
    public void setSource(int source) { this.source = source; }
    public boolean isFromSource(int source) { return (getSource() & source) == source; }
    public void recycle() {}
}
