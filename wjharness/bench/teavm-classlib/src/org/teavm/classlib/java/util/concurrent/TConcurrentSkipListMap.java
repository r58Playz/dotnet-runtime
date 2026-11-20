package org.teavm.classlib.java.util.concurrent;

import java.util.Comparator;
import java.util.Map;
import java.util.SortedMap;
import java.util.TreeMap;

/* One thread: a TreeMap. */
public class TConcurrentSkipListMap<K, V> extends TreeMap<K, V> implements TConcurrentNavigableMap<K, V> {
    private static final long serialVersionUID = 1L;

    public TConcurrentSkipListMap() {
    }

    public TConcurrentSkipListMap(Comparator<? super K> comparator) {
        super(comparator);
    }

    public TConcurrentSkipListMap(Map<? extends K, ? extends V> m) {
        super(m);
    }

    public TConcurrentSkipListMap(SortedMap<K, ? extends V> m) {
        super(m);
    }

    @Override
    public V putIfAbsent(K key, V value) {
        V v = get(key);
        return v != null ? v : put(key, value);
    }

    @Override
    public boolean remove(Object key, Object value) {
        if (containsKey(key) && java.util.Objects.equals(get(key), value)) {
            remove(key);
            return true;
        }
        return false;
    }

    @Override
    public boolean replace(K key, V oldValue, V newValue) {
        if (containsKey(key) && java.util.Objects.equals(get(key), oldValue)) {
            put(key, newValue);
            return true;
        }
        return false;
    }

    @Override
    public V replace(K key, V value) {
        return containsKey(key) ? put(key, value) : null;
    }
}
