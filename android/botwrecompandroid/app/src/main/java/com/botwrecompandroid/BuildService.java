package com.botwrecompandroid;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

// Adapted from BOTWRecompAndroid 97afeda (MIT). The existing builder stays in the app's process; this service
// keeps it eligible to run in the background and releases its wake lock when the build ends or Android times it out.
public class BuildService extends Service {
    private static final String CHANNEL = "game_build";
    private static final int ID = 1;
    private static volatile boolean requested;
    private static volatile int shown = -1;
    private static volatile BuildService active;
    private PowerManager.WakeLock wake;

    static synchronized void start(Context context) {
        requested = true;
        shown = -1;
        try {
            context.startForegroundService(new Intent(context, BuildService.class));
        } catch (RuntimeException error) {
            requested = false;
            Log.w("TOTKRecompAndroid", "Build service could not start", error);
        }
    }

    static synchronized void stop(Context context) {
        requested = false;
        shown = -1;
        context.stopService(new Intent(context, BuildService.class));
    }

    static void progress(int percent) {
        percent = Math.max(-1, Math.min(percent, 100));
        if (!requested || percent == shown) return;
        shown = percent;
        BuildService service = active;
        if (service != null) {
            NotificationManager manager = service.getSystemService(NotificationManager.class);
            if (manager != null) manager.notify(ID, notification(service, percent));
        }
    }

    private static Notification notification(Context context, int percent) {
        Intent open = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        PendingIntent pending = open == null ? null : PendingIntent.getActivity(context, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(context, CHANNEL)
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle(context.getString(R.string.build_title))
                .setContentText(context.getString(R.string.build_text))
                .setProgress(100, Math.max(percent, 0), percent < 0)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(pending)
                .build();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                getString(R.string.build_channel), NotificationManager.IMPORTANCE_LOW));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!requested) {
            stopSelf();
            return START_NOT_STICKY;
        }
        try {
            if (Build.VERSION.SDK_INT >= 29)
                startForeground(ID, notification(this, shown), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            else startForeground(ID, notification(this, shown));
        } catch (RuntimeException error) {
            Log.w("TOTKRecompAndroid", "Build service was refused", error);
            stopSelf();
            return START_NOT_STICKY;
        }
        active = this;
        if (wake == null) {
            PowerManager manager = getSystemService(PowerManager.class);
            if (manager != null) wake = manager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "totkrecomp:build");
        }
        if (wake != null && !wake.isHeld()) wake.acquire(4 * 60 * 60 * 1000L);
        return START_NOT_STICKY;
    }

    @Override
    public void onTimeout(int startId, int serviceType) {
        requested = false;
        stopSelf();
    }

    @Override
    public void onDestroy() {
        if (active == this) active = null;
        if (wake != null && wake.isHeld()) wake.release();
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.cancel(ID);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
