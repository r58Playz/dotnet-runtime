package org.teavm.classlib.java.util.concurrent;

import java.util.AbstractQueue;
import java.util.ArrayDeque;
import java.util.Collection;
import java.util.Iterator;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import wjteavm.Background;

/*
 * One thread: an ArrayDeque. take() runs background work until an element arrives; a timed poll runs background work
 * while the queue is empty and returns null once there is none (the timeout "elapsed").
 */
public class TLinkedBlockingQueue<E> extends AbstractQueue<E> implements BlockingQueue<E> {
    private final ArrayDeque<E> items = new ArrayDeque<>();
    private final int capacity;

    public TLinkedBlockingQueue() {
        this(Integer.MAX_VALUE);
    }

    public TLinkedBlockingQueue(int capacity) {
        this.capacity = capacity;
    }

    public TLinkedBlockingQueue(Collection<? extends E> c) {
        this(Integer.MAX_VALUE);
        items.addAll(c);
    }

    @Override
    public boolean offer(E e) {
        if (items.size() >= capacity) {
            return false;
        }
        return items.offer(e);
    }

    @Override
    public void put(E e) {
        items.add(e);
    }

    @Override
    public boolean offer(E e, long timeout, TimeUnit unit) {
        return offer(e);
    }

    @Override
    public E take() {
        Background.awaitUntil(() -> !items.isEmpty(), "BlockingQueue.take");
        return items.poll();
    }

    @Override
    public E poll(long timeout, TimeUnit unit) {
        while (items.isEmpty() && Background.runOne()) {
            // run queued work
        }
        return items.poll();
    }

    @Override
    public int remainingCapacity() {
        return capacity - items.size();
    }

    @Override
    public int drainTo(Collection<? super E> c) {
        return drainTo(c, Integer.MAX_VALUE);
    }

    @Override
    public int drainTo(Collection<? super E> c, int maxElements) {
        int n = 0;
        while (n < maxElements && !items.isEmpty()) {
            c.add(items.poll());
            ++n;
        }
        return n;
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
}
