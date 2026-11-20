package org.teavm.classlib.java.util.concurrent.locks;

import java.util.concurrent.TimeUnit;

public class TReentrantReadWriteLock implements TReadWriteLock {
    private final ReadLock readerLock = new ReadLock(this);
    private final WriteLock writerLock = new WriteLock(this);
    int reads;
    int writes;

    public TReentrantReadWriteLock() {
    }

    public TReentrantReadWriteLock(boolean fair) {
    }

    @Override
    public ReadLock readLock() {
        return readerLock;
    }

    @Override
    public WriteLock writeLock() {
        return writerLock;
    }

    public final boolean isFair() {
        return false;
    }

    public int getReadLockCount() {
        return reads;
    }

    public boolean isWriteLocked() {
        return writes > 0;
    }

    public boolean isWriteLockedByCurrentThread() {
        return writes > 0;
    }

    public int getWriteHoldCount() {
        return writes;
    }

    public int getReadHoldCount() {
        return reads;
    }

    public final boolean hasQueuedThreads() {
        return false;
    }

    public final int getQueueLength() {
        return 0;
    }

    public static class ReadLock implements TLock {
        private final TReentrantReadWriteLock owner;

        protected ReadLock(TReentrantReadWriteLock lock) {
            this.owner = lock;
        }

        @Override
        public void lock() {
            ++owner.reads;
        }

        @Override
        public void lockInterruptibly() {
            ++owner.reads;
        }

        @Override
        public boolean tryLock() {
            ++owner.reads;
            return true;
        }

        @Override
        public boolean tryLock(long timeout, TimeUnit unit) {
            ++owner.reads;
            return true;
        }

        @Override
        public void unlock() {
            if (owner.reads == 0) {
                throw new IllegalMonitorStateException();
            }
            --owner.reads;
        }

        @Override
        public TCondition newCondition() {
            throw new UnsupportedOperationException();
        }
    }

    public static class WriteLock implements TLock {
        private final TReentrantReadWriteLock owner;

        protected WriteLock(TReentrantReadWriteLock lock) {
            this.owner = lock;
        }

        @Override
        public void lock() {
            ++owner.writes;
        }

        @Override
        public void lockInterruptibly() {
            ++owner.writes;
        }

        @Override
        public boolean tryLock() {
            ++owner.writes;
            return true;
        }

        @Override
        public boolean tryLock(long timeout, TimeUnit unit) {
            ++owner.writes;
            return true;
        }

        @Override
        public void unlock() {
            if (owner.writes == 0) {
                throw new IllegalMonitorStateException();
            }
            --owner.writes;
        }

        @Override
        public TCondition newCondition() {
            return new TCondition() {
            };
        }

        public boolean isHeldByCurrentThread() {
            return owner.writes > 0;
        }

        public int getHoldCount() {
            return owner.writes;
        }
    }
}
