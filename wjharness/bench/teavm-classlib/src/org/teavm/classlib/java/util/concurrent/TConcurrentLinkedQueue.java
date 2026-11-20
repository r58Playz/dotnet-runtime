package org.teavm.classlib.java.util.concurrent;

import java.util.AbstractQueue;
import java.util.ArrayDeque;
import java.util.Collection;
import java.util.Iterator;

/* One thread: an ArrayDeque (which, unlike this class on the JDK, rejects nulls the same way). */
public class TConcurrentLinkedQueue<E> extends AbstractQueue<E> {
    private final ArrayDeque<E> items = new ArrayDeque<>();

    public TConcurrentLinkedQueue() {
    }

    public TConcurrentLinkedQueue(Collection<? extends E> c) {
        items.addAll(c);
    }

    @Override
    public boolean offer(E e) {
        return items.offer(e);
    }

    @Override
    public E poll() {
        return items.poll();
    }

    @Override
    public E peek() {
        return items.peek();
    }

    @Override
    public Iterator<E> iterator() {
        return items.iterator();
    }

    @Override
    public int size() {
        return items.size();
    }

    @Override
    public boolean isEmpty() {
        return items.isEmpty();
    }

    @Override
    public boolean contains(Object o) {
        return items.contains(o);
    }

    @Override
    public boolean remove(Object o) {
        return items.remove(o);
    }

    @Override
    public void clear() {
        items.clear();
    }
}
