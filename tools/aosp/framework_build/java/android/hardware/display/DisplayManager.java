package android.hardware.display;

import android.os.Handler;
import android.view.Display;
import java.util.LinkedHashMap;
import java.util.Map;

/** Maps the runtime's host surface to Android's primary display. */
public final class DisplayManager {
    public interface DisplayListener {
        void onDisplayAdded(int id);
        void onDisplayRemoved(int id);
        void onDisplayChanged(int id);
    }

    private final Display display;
    private final Map<DisplayListener, Handler> listeners = new LinkedHashMap<>();

    public DisplayManager(Display display) { this.display = display; }

    public Display getDisplay(int id) { return id == Display.DEFAULT_DISPLAY ? display : null; }
    public Display[] getDisplays() { return new Display[] {display}; }

    public void registerDisplayListener(DisplayListener listener, Handler handler) {
        if (listener == null) throw new IllegalArgumentException("listener");
        if (!listeners.containsKey(listener)) listeners.put(listener, handler == null ? new Handler() : handler);
    }
    public void unregisterDisplayListener(DisplayListener listener) { listeners.remove(listener); }
    public void dispatchDisplayChanged() {
        for (Map.Entry<DisplayListener, Handler> entry : listeners.entrySet()) {
            DisplayListener listener = entry.getKey();
            Handler handler = entry.getValue();
            handler.post(() -> {
                if (listeners.get(listener) == handler) listener.onDisplayChanged(display.getDisplayId());
            });
        }
    }
}
