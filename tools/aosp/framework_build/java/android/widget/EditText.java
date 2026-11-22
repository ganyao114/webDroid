package android.widget;

import android.content.Context;
import android.text.Editable;
import android.text.Selection;
import android.text.SpannableStringBuilder;
import android.text.TextWatcher;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.inputmethod.InputMethodManager;
import java.util.ArrayList;

public class EditText extends TextView {
    private final ArrayList<TextWatcher> watchers = new ArrayList<>();
    private final Buffer editable = new Buffer(super.getText());
    private OnEditorActionListener editor;
    private int selectionStart, selectionEnd, composingStart = -1;

    public EditText(Context context) { this(context, null); }
    public EditText(Context context, AttributeSet attrs) { this(context, attrs, android.R.attr.editTextStyle); }
    public EditText(Context context, AttributeSet attrs, int style) {
        super(context, attrs, style);
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
        Selection.setSelection(editable, start, end);
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

    private final class Buffer extends SpannableStringBuilder {
        Buffer(CharSequence initial) { super(initial); }

        @Override public SpannableStringBuilder replace(int start, int end, CharSequence text, int from, int to) {
            if (start < 0 || end < start || end > length()) throw new IndexOutOfBoundsException();
            int previousLength = length();
            TextWatcher[] listeners = watchers.toArray(new TextWatcher[watchers.size()]);
            for (TextWatcher watcher : listeners) watcher.beforeTextChanged(this, start, end - start, to - from);
            super.replace(start, end, text, from, to);
            int inserted = length() - previousLength + (end - start);
            setSelection(start + inserted);
            EditText.super.setText(this, BufferType.EDITABLE);
            for (TextWatcher watcher : listeners) watcher.onTextChanged(this, start, end - start, inserted);
            for (TextWatcher watcher : listeners) watcher.afterTextChanged(this);
            return this;
        }
    }
}
