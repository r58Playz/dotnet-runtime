package wjbench.mctick;

import org.slf4j.Marker;
import org.slf4j.event.Level;
import org.slf4j.helpers.LegacyAbstractLogger;
import org.slf4j.helpers.MessageFormatter;

/* The TeaVM arm's slf4j logger (log4j is not on its class path): INFO and above to the console, log4j's line shape. */
final class ConsoleLogger extends LegacyAbstractLogger {
    private static final long serialVersionUID = 1L;
    static boolean debug;

    ConsoleLogger(String name) {
        this.name = name;
    }

    @Override
    public boolean isTraceEnabled() {
        return false;
    }

    @Override
    public boolean isDebugEnabled() {
        return debug;
    }

    @Override
    public boolean isInfoEnabled() {
        return true;
    }

    @Override
    public boolean isWarnEnabled() {
        return true;
    }

    @Override
    public boolean isErrorEnabled() {
        return true;
    }

    @Override
    protected String getFullyQualifiedCallerName() {
        return null;
    }

    @Override
    protected void handleNormalizedLoggingCall(Level level, Marker marker, String pattern, Object[] args, Throwable t) {
        String msg = MessageFormatter.basicArrayFormat(pattern, args);
        String shortName = name == null ? "?" : name.substring(name.lastIndexOf('.') + 1);
        // the stdout captured before Minecraft's Bootstrap points System.out at a logger (which would loop back here)
        Platform.STDOUT.println("[main/" + level + "] [" + shortName + "]: " + msg);
        if (t != null) {
            t.printStackTrace(Platform.STDOUT);
        }
    }
}
