package android.text;

/** Mutable text backing the cooperative editor. Offsets count UTF-16 code units. */
public interface Editable extends CharSequence, Appendable {
    Editable replace(int start, int end, CharSequence text, int from, int to);
    Editable replace(int start, int end, CharSequence text);
    Editable insert(int where, CharSequence text, int start, int end);
    Editable insert(int where, CharSequence text);
    Editable delete(int start, int end);
    @Override Editable append(CharSequence text);
    @Override Editable append(CharSequence text, int start, int end);
    @Override Editable append(char value);
    void clear();
}
