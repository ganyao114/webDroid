package com.android.launcher3;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;
import com.android.launcher3.model.data.ItemInfo;
import com.android.launcher3.util.GridOccupancy;

public class CellLayout extends ViewGroup {
    private GridOccupancy occupancy;
    private int columns;
    private int rows;

    public CellLayout(Context context, AttributeSet attrs) { super(context, attrs); }

    public void setGrid(int columns, int rows) {
        removeAllViews();
        this.columns = columns;
        this.rows = rows;
        occupancy = new GridOccupancy(columns, rows);
    }

    public boolean addIcon(BubbleTextView child, ItemInfo item) {
        if (!occupancy.isRegionVacant(item.cellX, item.cellY, item.spanX, item.spanY)) return false;
        occupancy.markCells(item, true);
        child.setTag(item);
        addView(child, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        return true;
    }

    public boolean findVacantCell(int[] result) {
        return occupancy.findVacantCell(result, 1, 1);
    }

    private int edgeX(int cell) {
        int contentWidth = getMeasuredWidth() - getPaddingLeft() - getPaddingRight();
        int position = getLayoutDirection() == LAYOUT_DIRECTION_RTL ? columns - cell : cell;
        return getPaddingLeft() + position * contentWidth / columns;
    }

    private int edgeY(int cell) {
        int contentHeight = getMeasuredHeight() - getPaddingTop() - getPaddingBottom();
        return getPaddingTop() + cell * contentHeight / rows;
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(MeasureSpec.getSize(widthSpec), MeasureSpec.getSize(heightSpec));
        for (int index = 0; index < getChildCount(); index++) {
            View child = getChildAt(index);
            ItemInfo item = (ItemInfo) child.getTag();
            int width = Math.abs(edgeX(item.cellX + item.spanX) - edgeX(item.cellX));
            int height = Math.max(0, edgeY(item.cellY + item.spanY) - edgeY(item.cellY));
            child.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
        }
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        for (int index = 0; index < getChildCount(); index++) {
            View child = getChildAt(index);
            ItemInfo item = (ItemInfo) child.getTag();
            int left = Math.min(edgeX(item.cellX), edgeX(item.cellX + item.spanX));
            int top = edgeY(item.cellY);
            child.layout(left, top, left + child.getMeasuredWidth(), top + child.getMeasuredHeight());
        }
    }
}
