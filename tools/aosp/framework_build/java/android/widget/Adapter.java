package android.widget;

import android.database.DataSetObserver;
import android.view.View;
import android.view.ViewGroup;

public interface Adapter {
    int getCount();
    Object getItem(int position);
    long getItemId(int position);
    View getView(int position, View recycled, ViewGroup parent);
    boolean hasStableIds();
    void registerDataSetObserver(DataSetObserver observer);
    void unregisterDataSetObserver(DataSetObserver observer);
}
