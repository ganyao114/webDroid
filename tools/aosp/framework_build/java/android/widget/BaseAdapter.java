package android.widget;

import android.database.DataSetObserver;
import java.util.ArrayList;

public abstract class BaseAdapter implements ListAdapter {
    private final ArrayList<DataSetObserver> observers = new ArrayList<>();
    public boolean areAllItemsEnabled() { return true; }
    public boolean isEnabled(int position) { return true; }
    public boolean hasStableIds() { return false; }
    public boolean isEmpty() { return getCount() == 0; }
    public int getItemViewType(int position) { return 0; }
    public int getViewTypeCount() { return 1; }
    public void registerDataSetObserver(DataSetObserver observer) {
        if (observer == null) throw new NullPointerException("observer");
        if (!observers.contains(observer)) observers.add(observer);
    }
    public void unregisterDataSetObserver(DataSetObserver observer) { observers.remove(observer); }
    public void notifyDataSetChanged() {
        for (DataSetObserver observer : new ArrayList<>(observers)) observer.onChanged();
    }
    public void notifyDataSetInvalidated() {
        for (DataSetObserver observer : new ArrayList<>(observers)) observer.onInvalidated();
    }
}
