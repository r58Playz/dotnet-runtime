package org.teavm.classlib.java.util.concurrent;

import java.util.ArrayDeque;
import java.util.Collection;

/* One thread: an ArrayDeque. */
public class TConcurrentLinkedDeque<E> extends ArrayDeque<E> {
    private static final long serialVersionUID = 1L;

    public TConcurrentLinkedDeque() {
    }

    public TConcurrentLinkedDeque(Collection<? extends E> c) {
        super(c);
    }
}
