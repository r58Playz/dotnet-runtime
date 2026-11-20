package org.teavm.classlib.java.lang.reflect;

import java.lang.reflect.InvocationHandler;

/* No dynamic proxies on the linear-memory target (Guava's Reflection.newProxy links to this and is not reached). */
public class TProxy {
    protected InvocationHandler h;

    protected TProxy(InvocationHandler h) {
        this.h = h;
    }

    public static Object newProxyInstance(ClassLoader loader, Class<?>[] interfaces, InvocationHandler h) {
        throw new UnsupportedOperationException("java.lang.reflect.Proxy on TeaVM");
    }

    public static boolean isProxyClass(Class<?> cl) {
        return false;
    }

    public static InvocationHandler getInvocationHandler(Object proxy) {
        throw new IllegalArgumentException("not a proxy instance");
    }
}
