package nodomain.freeyourgadget.gadgetbridge.service.devices.kerfur;

import java.util.UUID;

public final class KerfurUuids {
    private KerfurUuids() {
    }

    public static final UUID SERVICE = UUID.fromString("4a7b1001-1e1a-4d52-a2ef-14bb5f420001");
    public static final UUID RX = UUID.fromString("4a7b1002-1e1a-4d52-a2ef-14bb5f420001");
    public static final UUID TX = UUID.fromString("4a7b1003-1e1a-4d52-a2ef-14bb5f420001");

    public static final byte OP_PING = 0x01;
    public static final byte OP_ANDROID_NOTIFICATION = 0x10;

    public static final byte EVT_PONG = (byte) 0x81;
    public static final byte EVT_NOTIFICATION_ACK = (byte) 0x82;
    public static final byte EVT_TEST_NOTIFICATION = (byte) 0x90;
    public static final byte EVT_ERROR = (byte) 0xE0;
}
