package com.mojang.logging;

import java.util.function.Supplier;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.slf4j.Marker;
import org.slf4j.MarkerFactory;
import org.slf4j.event.Level;

/*
 * mctick's TeaVM arm: Mojang's LogUtils without log4j or StackWalker (the logger is named after no class: names only
 * label lines). Same public API as logging-1.7.12's.
 */
public class LogUtils {
    public static final String FATAL_MARKER_ID = "FATAL";
    public static final Marker FATAL_MARKER = MarkerFactory.getMarker(FATAL_MARKER_ID);

    public LogUtils() {
    }

    public static boolean isLoggerActive() {
        return true;
    }

    public static void configureRootLoggingLevel(Level level) {
    }

    public static Object defer(Supplier<Object> result) {
        return new Object() {
            @Override
            public String toString() {
                return String.valueOf(result.get());
            }
        };
    }

    public static Logger getLogger() {
        return LoggerFactory.getLogger("minecraft");
    }
}
