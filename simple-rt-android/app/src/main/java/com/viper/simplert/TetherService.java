/*
 * SimpleRT: Reverse tethering utility for Android
 * Copyright (C) 2016-2017 Konstantin Menyaev
 * Copyright (C) 2017 Aleksander Morgado <aleksander@aleksander.es>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

package com.viper.simplert;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.net.VpnService;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.widget.Toast;

public class TetherService extends VpnService {
    private static final String TAG = "TetherService";
    private static final String ACTION_USB_PERMISSION = "com.viper.simplert.TetherService.action.USB_PERMISSION";

    private final BroadcastReceiver mUsbReceiver = new BroadcastReceiver() {
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();

            if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(action)) {
                Log.d(TAG,"Accessory detached");

                UsbAccessory accessory = intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY);
                Native.stop();
                unregisterReceiver(mUsbReceiver);
            }
        }
    };

    @Override
    public int onStartCommand(final Intent intent, int flags, final int startId) {
        Log.w(TAG, "onStartCommand");

        if (intent == null) {
            Log.i(TAG, "Intent is null");
            return START_NOT_STICKY;
        }

        if (Native.is_running()) {
            Log.e(TAG, "already running!");
            return START_NOT_STICKY;
        }

        final UsbAccessory accessory = intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY);

        if (accessory == null) {
            showErrorDialog(getString(R.string.accessory_error));
            stopSelf();
            return START_NOT_STICKY;
        }

        Log.d(TAG, "Got accessory: " + accessory.getModel());

        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
        registerReceiver(mUsbReceiver, filter);

        Builder builder = new Builder();
        builder.setMtu(1500);
        builder.setSession(getString(R.string.app_name));
        // Use the serial field to receive the IP address to use :)
        builder.addAddress(accessory.getSerial(), 30);
        builder.addRoute("0.0.0.0", 0);
        builder.addDnsServer("8.8.8.8");

        final ParcelFileDescriptor accessoryFd = ((UsbManager) getSystemService(Context.USB_SERVICE)).openAccessory(accessory);
        if (accessoryFd == null) {
            showErrorDialog(getString(R.string.accessory_error));
            stopSelf();
            return START_NOT_STICKY;
        }

        final ParcelFileDescriptor tunFd = builder.establish();
        if (tunFd == null) {
            showErrorDialog(getString(R.string.tun_error));
            stopSelf();
            return START_NOT_STICKY;
        }

        Toast.makeText(this, "SimpleRT Connected! (" + accessory.getSerial() + ")", Toast.LENGTH_SHORT).show();
        Native.start(tunFd.detachFd(), accessoryFd.detachFd());

        return START_NOT_STICKY;
    }

    private void showErrorDialog(String err) {
        Intent activityIntent = new Intent(getApplicationContext(), InfoActivity.class);
        activityIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        activityIntent.putExtra("text", err);
        startActivity(activityIntent);
    }
}
