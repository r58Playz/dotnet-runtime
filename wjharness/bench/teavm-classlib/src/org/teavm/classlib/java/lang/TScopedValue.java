package org.teavm.classlib.java.lang;

import java.util.NoSuchElementException;
import java.util.function.Supplier;

/* java.lang.ScopedValue for one thread: a binding is a field saved and restored around run/call. */
public final class TScopedValue<T> {
    private static final Object UNBOUND = new Object();
    private Object value = UNBOUND;

    private TScopedValue() {
    }

    public static <T> TScopedValue<T> newInstance() {
        return new TScopedValue<>();
    }

    public static <T> Carrier where(TScopedValue<T> key, T value) {
        return new Carrier(null, key, value);
    }

    @SuppressWarnings("unchecked")
    public T get() {
        if (value == UNBOUND) {
            throw new NoSuchElementException("ScopedValue not bound");
        }
        return (T) value;
    }

    public boolean isBound() {
        return value != UNBOUND;
    }

    @SuppressWarnings("unchecked")
    public T orElse(T other) {
        return value == UNBOUND ? other : (T) value;
    }

    public <X extends Throwable> T orElseThrow(Supplier<? extends X> supplier) throws X {
        if (value == UNBOUND) {
            throw supplier.get();
        }
        return get();
    }

    public interface CallableOp<T, X extends Throwable> {
        T call() throws X;
    }

    public static final class Carrier {
        private final Carrier prev;
        private final TScopedValue<?> key;
        private final Object value;

        Carrier(Carrier prev, TScopedValue<?> key, Object value) {
            this.prev = prev;
            this.key = key;
            this.value = value;
        }

        public <T> Carrier where(TScopedValue<T> k, T v) {
            return new Carrier(this, k, v);
        }

        @SuppressWarnings("unchecked")
        public <T> T get(TScopedValue<T> k) {
            for (Carrier c = this; c != null; c = c.prev) {
                if (c.key == k) {
                    return (T) c.value;
                }
            }
            throw new NoSuchElementException();
        }

        public void run(Runnable op) {
            Object[] saved = bind();
            try {
                op.run();
            } finally {
                unbind(saved);
            }
        }

        public <R, X extends Throwable> R call(CallableOp<? extends R, X> op) throws X {
            Object[] saved = bind();
            try {
                return op.call();
            } finally {
                unbind(saved);
            }
        }

        private Object[] bind() {
            int n = 0;
            for (Carrier c = this; c != null; c = c.prev) {
                ++n;
            }
            Object[] saved = new Object[n];
            int i = 0;
            for (Carrier c = this; c != null; c = c.prev) {
                saved[i++] = c.key.value;
            }
            for (Carrier c = this; c != null; c = c.prev) {
                c.key.value = c.value;
            }
            return saved;
        }

        private void unbind(Object[] saved) {
            int i = 0;
            for (Carrier c = this; c != null; c = c.prev) {
                c.key.value = saved[i++];
            }
        }
    }
}
