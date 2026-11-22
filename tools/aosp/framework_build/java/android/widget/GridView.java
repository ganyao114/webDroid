package android.widget;

import android.content.Context;
import android.database.DataSetObserver;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import java.util.ArrayList;

public class GridView extends AbsListView {
    public static final int NO_STRETCH = 0, STRETCH_SPACING = 1;
    public static final int STRETCH_COLUMN_WIDTH = 2, STRETCH_SPACING_UNIFORM = 3;
    private int columns = 1, rowHeight, selected;
    private int firstPosition, boundCount;
    private final int touchSlop;
    private float downY, lastY;
    private boolean dirty = true, dragging;
    private View empty;
    private ListAdapter adapter;
    private final DataSetObserver observer = new DataSetObserver() {
        @Override public void onChanged() { refresh(); }
        @Override public void onInvalidated() { refresh(); }
    };

    public GridView(Context context) { this(context, null); }
    public GridView(Context context, AttributeSet attrs) {
        super(context, attrs);
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
    }

    @Override public void setAdapter(ListAdapter value) {
        if (adapter != null) adapter.unregisterDataSetObserver(observer);
        adapter = value;
        selected = 0;
        rowHeight = 0;
        scrollTo(0, 0);
        if (adapter != null) adapter.registerDataSetObserver(observer);
        refresh();
    }
    @Override public ListAdapter getAdapter() { return adapter; }
    public void setEmptyView(View value) { empty = value; refresh(); }
    public View getEmptyView() { return empty; }
    public void setNumColumns(int columns) {
        if (columns <= 0) throw new IllegalArgumentException("column count");
        if (this.columns != columns) {
            this.columns = columns;
            refresh();
        }
    }
    public int getNumColumns() { return columns; }
    public void setStretchMode(int mode) {
        if (mode < NO_STRETCH || mode > STRETCH_SPACING_UNIFORM) throw new IllegalArgumentException("stretch mode");
    }
    @Override public void setSelection(int position) {
        selected = Math.max(0, Math.min(position, count() - 1));
        if (rowHeight > 0) scrollTo(0, Math.min(maxScroll(getHeight()), selected / columns * rowHeight));
        requestLayout();
    }
    @Override public int getSelectedItemPosition() { return selected; }
    @Override public View getSelectedView() {
        int child = selected - firstPosition;
        return child < 0 || child >= getChildCount() ? null : getChildAt(child);
    }

    private int count() { return adapter == null ? 0 : adapter.getCount(); }
    private void refresh() {
        dirty = true;
        if (empty != null) {
            boolean hasItems = count() != 0;
            empty.setVisibility(hasItems ? GONE : VISIBLE);
            setVisibility(hasItems ? VISIBLE : GONE);
        }
        requestLayout();
    }
    private int totalHeight() {
        return getPaddingTop() + (count() + columns - 1) / columns * rowHeight + getPaddingBottom();
    }
    private int maxScroll(int height) { return Math.max(0, totalHeight() - height); }
    @Override protected LayoutParams generateDefaultLayoutParams() { return new LayoutParams(-1, -2); }

    @Override protected void onMeasure(int w, int h) {
        int width = MeasureSpec.getSize(w), height = MeasureSpec.getSize(h);
        int columnWidth = Math.max(0, width - getPaddingLeft() - getPaddingRight()) / columns;
        int items = count();
        if (items > 0 && (dirty || rowHeight == 0)) {
            View trial = adapter.getView(0, null, this);
            int size = trial.getLayoutParams().height;
            trial.measure(MeasureSpec.makeMeasureSpec(columnWidth, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(Math.max(size, 0), size > 0 ? MeasureSpec.EXACTLY : MeasureSpec.UNSPECIFIED));
            rowHeight = Math.max(1, trial.getMeasuredHeight());
        }
        if (MeasureSpec.getMode(h) != MeasureSpec.EXACTLY) height = resolveSize(totalHeight(), h);
        int scroll = Math.min(getScrollY(), maxScroll(height));
        if (scroll != getScrollY()) scrollTo(0, scroll);
        int first = rowHeight > 0 ? scroll / rowHeight * columns : 0;
        int visible = rowHeight > 0 ? (height / rowHeight + 2) * columns : 0;
        int rows = Math.min(visible, Math.max(0, items - first));
        if (dirty || first != firstPosition || rows != boundCount) bind(first, rows);
        for (int i = 0; i < getChildCount(); i++) {
            getChildAt(i).measure(MeasureSpec.makeMeasureSpec(columnWidth, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(rowHeight, MeasureSpec.EXACTLY));
        }
        setMeasuredDimension(width, height);
    }

    private void bind(int first, int count) {
        ArrayList<View> recycled = new ArrayList<>(getChildCount());
        for (int i = 0; i < getChildCount(); i++) recycled.add(getChildAt(i));
        removeAllViews();
        for (int i = 0; i < count; i++) {
            View child = adapter.getView(first + i, i < recycled.size() ? recycled.get(i) : null, this);
            child.setEnabled(adapter.isEnabled(first + i));
            addView(child);
        }
        dirty = false;
        firstPosition = first;
        boundCount = count;
    }

    @Override protected void onLayout(boolean change, int l, int t, int r, int b) {
        int width = Math.max(0, r - l - getPaddingLeft() - getPaddingRight()) / columns;
        for (int i = 0; i < getChildCount(); i++) {
            int index = firstPosition + i;
            int left = getPaddingLeft() + (index % columns) * width;
            int top = getPaddingTop() + (index / columns) * rowHeight;
            getChildAt(i).layout(left, top, left + width, top + rowHeight);
        }
    }

    private boolean scrollByPixels(float delta) {
        int next = Math.max(0, Math.min(maxScroll(getHeight()), getScrollY() + (int) delta));
        if (next == getScrollY()) return false;
        scrollTo(0, next);
        requestLayout();
        return true;
    }

    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_SCROLL)
            return scrollByPixels(-event.getAxisValue(MotionEvent.AXIS_VSCROLL) * rowHeight);
        return super.onGenericMotionEvent(event);
    }

    @Override public boolean onInterceptTouchEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            downY = lastY = event.getY();
            dragging = false;
        } else if (event.getActionMasked() == MotionEvent.ACTION_MOVE && maxScroll(getHeight()) != 0
                && Math.abs(event.getY() - downY) > touchSlop) {
            dragging = true;
            return true;
        }
        return false;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (maxScroll(getHeight()) == 0) return super.onTouchEvent(event);
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: downY = lastY = event.getY(); return true;
            case MotionEvent.ACTION_MOVE:
                dragging |= Math.abs(event.getY() - downY) > touchSlop;
                if (dragging) scrollByPixels(lastY - event.getY());
                lastY = event.getY();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: dragging = false; return true;
            default: return false;
        }
    }
}
