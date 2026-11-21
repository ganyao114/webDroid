package android.widget;

import android.content.Context;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.inputmethod.InputMethodManager;
import java.util.ArrayList;

public class EditText extends TextView {
    private final ArrayList<TextWatcher> watchers = new ArrayList<>();
    private final Buffer editable = new Buffer();
    private OnEditorActionListener editor;
    private int selectionStart, selectionEnd, composingStart = -1;

    public EditText(Context context) { this(context, null); }
    public EditText(Context context, AttributeSet attrs) { this(context, attrs, android.R.attr.editTextStyle); }
    public EditText(Context context, AttributeSet attrs, int style) {
        super(context, attrs, style);
        editable.value = super.getText().toString();
        setSelection(editable.length());
        setFocusableInTouchMode(true);
        setClickable(true);
    }

    @Override public Editable getText() { return editable; }
    @Override public void setText(CharSequence text, BufferType type) {
        if (editable == null) {
            super.setText(text, type);
            return;
        }
        editable.replace(0, editable.length(), text == null ? "" : text);
    }

    public void addTextChangedListener(TextWatcher watcher) { if (watcher != null) watchers.add(watcher); }
    public void removeTextChangedListener(TextWatcher watcher) { watchers.remove(watcher); }
    public void setOnEditorActionListener(OnEditorActionListener listener) { editor = listener; }
    public int getSelectionStart() { return selectionStart; }
    public int getSelectionEnd() { return selectionEnd; }
    public void setSelection(int start, int end) {
        if (start < 0 || end < 0 || start > editable.length() || end > editable.length())
            throw new IndexOutOfBoundsException();
        composingStart = -1;
        selectionStart = start;
        selectionEnd = end;
    }
    public void setSelection(int index) { setSelection(index, index); }

    @Override public boolean onImeText(CharSequence text, boolean composing) {
        if (!isEnabled()) return false;
        int start = composingStart < 0 ? Math.min(selectionStart, selectionEnd) : composingStart;
        editable.replace(start, Math.max(selectionStart, selectionEnd), text);
        if (composing) composingStart = start;
        return true;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN && isEnabled() && requestFocus()) {
            var keyboard = getContext().getSystemService(InputMethodManager.class);
            if (keyboard != null) keyboard.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        }
        return super.onTouchEvent(event);
    }

    @Override public boolean onKeyDown(int code, KeyEvent event) {
        if (code == KeyEvent.KEYCODE_DEL) {
            int start = Math.min(selectionStart, selectionEnd), end = Math.max(selectionStart, selectionEnd);
            if (start == end && start > 0) {
                --start;
                if (start > 0 && Character.isLowSurrogate(editable.charAt(start))
                        && Character.isHighSurrogate(editable.charAt(start - 1))) --start;
            }
            if (start != end) editable.delete(start, end);
            return true;
        }
        return super.onKeyDown(code, event);
    }

    @Override public boolean onKeyUp(int code, KeyEvent event) {
        if (code == KeyEvent.KEYCODE_ENTER && editor != null && editor.onEditorAction(this, 0, event)) return true;
        return super.onKeyUp(code, event);
    }

    private final class Buffer implements Editable {
        private String value = "";
        @Override public int length() { return value.length(); }
        @Override public char charAt(int point) { return value.charAt(point); }
        @Override public CharSequence subSequence(int start, int end) { return value.substring(start, end); }
        @Override public String toString() { return value; }
        @Override public Editable replace(int start, int end, CharSequence text, int from, int to) {
            if (start < 0 || end < start || end > value.length()) throw new IndexOutOfBoundsException();
            String replacement = text.subSequence(from, to).toString();
            String next = value.substring(0, start) + replacement + value.substring(end);
            TextWatcher[] listeners = watchers.toArray(new TextWatcher[watchers.size()]);
            for (TextWatcher watcher : listeners) watcher.beforeTextChanged(value, start, end - start, to - from);
            value = next;
            setSelection(start + replacement.length());
            EditText.super.setText(value, BufferType.EDITABLE);
            for (TextWatcher watcher : listeners) watcher.onTextChanged(value, start, end - start, to - from);
            for (TextWatcher watcher : listeners) watcher.afterTextChanged(this);
            return this;
        }
        @Override public Editable replace(int start, int end, CharSequence text) { return replace(start, end, text, 0, text.length()); }
        @Override public Editable insert(int where, CharSequence text, int start, int end) { return replace(where, where, text, start, end); }
        @Override public Editable insert(int where, CharSequence text) { return replace(where, where, text); }
        @Override public Editable delete(int start, int end) { return replace(start, end, ""); }
        @Override public Editable append(CharSequence text) { return insert(length(), text == null ? "null" : text); }
        @Override public Editable append(CharSequence text, int start, int end) { return insert(length(), text == null ? "null" : text, start, end); }
        @Override public Editable append(char value) { return append(String.valueOf(value)); }
        @Override public void clear() { delete(0, length()); }
    }
}
