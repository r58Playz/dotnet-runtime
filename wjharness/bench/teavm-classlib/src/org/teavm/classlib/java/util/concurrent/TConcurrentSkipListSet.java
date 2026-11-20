package org.teavm.classlib.java.util.concurrent;

import java.util.Collection;
import java.util.Comparator;
import java.util.TreeSet;

/* One thread: a TreeSet. */
public class TConcurrentSkipListSet<E> extends TreeSet<E> {
    private static final long serialVersionUID = 1L;

    public TConcurrentSkipListSet() {
    }

    public TConcurrentSkipListSet(Comparator<? super E> comparator) {
        super(comparator);
    }

    public TConcurrentSkipListSet(Collection<? extends E> c) {
        super(c);
    }
}
