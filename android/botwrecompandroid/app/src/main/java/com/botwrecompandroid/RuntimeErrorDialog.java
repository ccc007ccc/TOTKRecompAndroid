package com.botwrecompandroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Build;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.concurrent.CountDownLatch;

final class RuntimeErrorDialog {
    private static final String TAG = "TOTKRecompAndroid";

    // Shared with the driver dialog, whose three standard buttons already have
    // actions. Copying never dismisses the message or changes application state.
    static View messageView(Context context, String report) {
        int padding = Math.round(24 * context.getResources().getDisplayMetrics().density);
        LinearLayout content = new LinearLayout(context);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, padding / 3, padding, 0);
        TextView message = new TextView(context);
        message.setTextAppearance(android.R.style.TextAppearance_Material_Body1);
        message.setText(report);
        message.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(context);
        scroll.addView(message);
        int height = Math.min(padding * 10, context.getResources().getDisplayMetrics().heightPixels / 2);
        content.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, height));
        Button copy = new Button(context);
        copy.setText(R.string.error_copy);
        copy.setOnClickListener(v -> {
            try {
                ClipboardManager clipboard = context.getSystemService(ClipboardManager.class);
                if (clipboard == null) throw new IllegalStateException("Clipboard unavailable");
                clipboard.setPrimaryClip(ClipData.newPlainText(TAG, report));
                Toast.makeText(context, R.string.error_copied, Toast.LENGTH_SHORT).show();
            } catch (RuntimeException e) {
                Log.w(TAG, "copy error details", e);
                Toast.makeText(context, R.string.error_copy_failed, Toast.LENGTH_SHORT).show();
            }
        });
        content.addView(copy);
        return content;
    }

    static boolean show(Activity activity, String details, boolean nativeCrash) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return false;
        String version = "";
        try { version = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName; }
        catch (Exception e) { Log.w(TAG, "error report version", e); }
        String report = activity.getString(nativeCrash ? R.string.native_crash_intro : R.string.runtime_error_intro)
                + "\n\n" + details + "\n\n" + TAG + " " + version + "\n" + Build.MODEL + " / Android " + Build.VERSION.RELEASE;
        CountDownLatch dismissed = new CountDownLatch(1);
        activity.runOnUiThread(() -> {
            if (activity.isFinishing() || activity.isDestroyed()) { dismissed.countDown(); return; }
            try {
                AlertDialog.Builder builder = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle(nativeCrash ? R.string.native_crash_title : R.string.runtime_error_title)
                        .setPositiveButton(android.R.string.ok, null);
                builder.setView(messageView(builder.getContext(), report));
                AlertDialog dialog = builder.create();
                dialog.setOnDismissListener(d -> dismissed.countDown());
                dialog.show();
            } catch (RuntimeException e) {
                Log.e(TAG, "show error details", e);
                dismissed.countDown();
            }
        });
        // Native errors normally originate on a worker. Never wait on Android's
        // UI thread, which must dispatch the Copy and OK buttons.
        if (Looper.myLooper() != Looper.getMainLooper()) {
            try { dismissed.await(); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }
        return true;
    }
}
