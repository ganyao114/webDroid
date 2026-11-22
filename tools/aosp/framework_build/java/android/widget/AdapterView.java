package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;

/** Common adapter and selection API for the embedded widget containers. */
public abstract class AdapterView<T extends Adapter> extends ViewGroup {
    public static final int INVALID_POSITION = -1;
    public static final long INVALID_ROW_ID = Long.MIN_VALUE;

    protected AdapterView(Context context, AttributeSet attrs) { super(context, attrs); }

    public abstract T getAdapter();
    public abstract void setAdapter(T adapter);
    public abstract View getSelectedView();
    public abstract void setSelection(int position);
    public abstract int getSelectedItemPosition();

    public int getCount() {
        T adapter = getAdapter();
        return adapter == null ? 0 : adapter.getCount();
    }
    public Object getSelectedItem() {
        int position = getSelectedItemPosition();
        return position < 0 || position >= getCount() ? null : getAdapter().getItem(position);
    }
    public long getSelectedItemId() {
        int position = getSelectedItemPosition();
        return position < 0 || position >= getCount() ? INVALID_ROW_ID : getAdapter().getItemId(position);
    }
}
