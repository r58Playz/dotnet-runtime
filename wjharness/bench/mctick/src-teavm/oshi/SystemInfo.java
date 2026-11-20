package oshi;

import oshi.hardware.HardwareAbstractionLayer;
import oshi.software.os.OperatingSystem;

/* mctick's TeaVM arm: no hardware probing. Minecraft's SystemReport wraps its use in ignoreErrors. */
public class SystemInfo {
    public SystemInfo() {
        throw new UnsupportedOperationException("OSHI on TeaVM");
    }

    public OperatingSystem getOperatingSystem() {
        throw new UnsupportedOperationException("OSHI on TeaVM");
    }

    public HardwareAbstractionLayer getHardware() {
        throw new UnsupportedOperationException("OSHI on TeaVM");
    }
}
