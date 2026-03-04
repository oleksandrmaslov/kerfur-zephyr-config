package nodomain.freeyourgadget.gadgetbridge.service.devices.kerfur;

import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.nio.charset.StandardCharsets;

/*
 * Scaffold only.
 *
 * Adapt imports/base class names to your Gadgetbridge branch. Different
 * Gadgetbridge versions use slightly different support abstractions.
 */
public class KerfurDeviceSupport /* extends AbstractBTLEDeviceSupport */ {
    private static final Logger LOG = LoggerFactory.getLogger(KerfurDeviceSupport.class);

    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx;
    private BluetoothGattCharacteristic tx;

    public void onServicesDiscovered(BluetoothGatt gatt) {
        this.gatt = gatt;

        final BluetoothGattService service = gatt.getService(KerfurUuids.SERVICE);
        if (service == null) {
            LOG.warn("Kerfur companion service not found");
            return;
        }

        rx = service.getCharacteristic(KerfurUuids.RX);
        tx = service.getCharacteristic(KerfurUuids.TX);

        if (rx == null || tx == null) {
            LOG.warn("Kerfur companion characteristics are missing");
            return;
        }

        // TODO: enable notifications on TX characteristic using your branch helper.
        // After notifications are enabled, send ping.
        sendPing();
    }

    public void sendPing() {
        writeRx(new byte[] { KerfurUuids.OP_PING });
    }

    public void sendAndroidNotification(int category, String previewText) {
        if (category < 0) {
            category = 0;
        }
        if (category > 255) {
            category = 255;
        }

        byte[] preview = new byte[0];
        if (previewText != null && !previewText.isEmpty()) {
            preview = previewText.getBytes(StandardCharsets.UTF_8);
        }

        // ATT payload is kept small in protocol v1.
        final int maxPreview = Math.min(preview.length, 18);
        final byte[] payload = new byte[2 + maxPreview];
        payload[0] = KerfurUuids.OP_ANDROID_NOTIFICATION;
        payload[1] = (byte) category;
        if (maxPreview > 0) {
            System.arraycopy(preview, 0, payload, 2, maxPreview);
        }

        writeRx(payload);
    }

    public void onTxNotification(byte[] value) {
        if (value == null || value.length < 1) {
            return;
        }

        final int opcode = value[0] & 0xFF;
        switch (opcode) {
            case 0x81:
                LOG.info("Kerfur pong received");
                break;
            case 0x82:
                if (value.length > 1) {
                    LOG.info("Kerfur notification ACK category={}", value[1] & 0xFF);
                }
                break;
            case 0x90:
                if (value.length > 1) {
                    LOG.info("Kerfur test notification category={}", value[1] & 0xFF);
                }
                break;
            case 0xE0:
                LOG.warn("Kerfur companion error response");
                break;
            default:
                LOG.info("Kerfur companion event opcode=0x{}", Integer.toHexString(opcode));
                break;
        }
    }

    private void writeRx(byte[] payload) {
        if (gatt == null || rx == null || payload == null) {
            return;
        }

        rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
        rx.setValue(payload);
        final boolean queued = gatt.writeCharacteristic(rx);
        if (!queued) {
            LOG.warn("Failed to queue Kerfur RX write");
        }
    }
}
