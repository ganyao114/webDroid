package java.util.concurrent;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.ListIterator;

/** Snapshot iteration on the interpreter thread. */
public class CopyOnWriteArrayList<E> extends ArrayList<E> {
    public CopyOnWriteArrayList() {}
    public CopyOnWriteArrayList(Collection<? extends E> values) { super(values); }
    public CopyOnWriteArrayList(E[] values) { super(Arrays.asList(values)); }

    public boolean addIfAbsent(E value) { return !contains(value) && add(value); }

    public int addAllAbsent(Collection<? extends E> values) {
        int added = 0;
        for (E value : values) if (addIfAbsent(value)) added++;
        return added;
    }

    final Object[] getArray() { return super.toArray(); }

    @SuppressWarnings("unchecked")
    private List<E> snapshot() {
        return Collections.unmodifiableList(Arrays.asList((E[]) getArray()));
    }

    @Override public Iterator<E> iterator() { return snapshot().iterator(); }
    @Override public ListIterator<E> listIterator() { return listIterator(0); }
    @Override public ListIterator<E> listIterator(int index) { return snapshot().listIterator(index); }
}
