package com.botwrecompandroid;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.inspector.WindowInspector;
import android.widget.Button;
import android.widget.TextView;

import java.util.concurrent.atomic.AtomicReference;

// No JUnit/runner dependency: this exercises the deployed APK's real JNI fatal
// path, Android clipboard and dialog callbacks in one short instrumentation run.
public final class RuntimeErrorDialogCheck extends Instrumentation {
    private static final String MARKER = "Error-dialog verification";
    private static native void raiseRuntimeError();

    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }

    private static TextView findText(View view, String text, boolean button) {
        if (view.isShown() && view instanceof TextView && (!button || view instanceof Button)
                && ((TextView)view).getText().toString().contains(text)) return (TextView)view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)view;
            for (int i=0;i<group.getChildCount();i++) {
                TextView found = findText(group.getChildAt(i), text, button);
                if (found != null) return found;
            }
        }
        return null;
    }

    private View dialogRoot() {
        for (View root : WindowInspector.getGlobalWindowViews())
            if (findText(root, MARKER, false) != null) return root;
        return null;
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            Intent launch = new Intent(Intent.ACTION_MAIN)
                    .setClassName(getTargetContext().getPackageName(), BOTWRecompAndroidActivity.class.getName())
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    .putExtra("env", "LARECOMP_NO_LAUNCHER=1,LARECOMP_GAME_DYNRES=1");
            Activity activity = startActivitySync(launch);
            System.loadLibrary("error_dialog_test");
            int pid = Process.myPid();
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread worker = new Thread(() -> {
                try { raiseRuntimeError(); failure.set(new AssertionError("A failed native call resumed")); }
                catch (Throwable error) { failure.set(error); }
            }, "RuntimeErrorDialogCheck");
            worker.setDaemon(true);
            worker.start();
            AtomicReference<View> root = new AtomicReference<>();
            long deadline = SystemClock.uptimeMillis()+10000;
            do {
                runOnMainSync(() -> root.set(dialogRoot()));
                if (root.get() != null || failure.get() != null) break;
                SystemClock.sleep(50);
            } while (SystemClock.uptimeMillis()<deadline);
            require(failure.get()==null, "Native test failed: "+failure.get());
            require(root.get()!=null, "Runtime error dialog did not appear");
            runOnMainSync(() -> {
                TextView copy = findText(root.get(), activity.getString(R.string.error_copy), true);
                require(copy!=null, "Copy button missing");
                copy.performClick();
                require(dialogRoot()!=null, "Copy dismissed the dialog");
                ClipboardManager clipboard = activity.getSystemService(ClipboardManager.class);
                ClipData clip = clipboard.getPrimaryClip();
                require(clip!=null && clip.getItemCount()>0 && clip.getItemAt(0).coerceToText(activity).toString().contains(MARKER),
                        "Clipboard does not contain the error details");
                View ok = root.get().findViewById(android.R.id.button1);
                require(ok!=null, "OK button missing");
                ok.performClick();
            });
            waitForIdleSync();
            SystemClock.sleep(300);
            runOnMainSync(() -> {
                require(dialogRoot()==null, "OK did not dismiss the dialog");
                require(!activity.isFinishing() && !activity.isDestroyed(), "OK closed the activity");
            });
            require(pid==Process.myPid() && worker.isAlive() && failure.get()==null,
                    "Dismissal terminated the process or resumed the failed native call");
            result.putString("stream", "PASS: Copy preserved the dialog and copied details; OK dismissed it; process and failed native worker retained.\n");
            finish(Activity.RESULT_OK,result);
        } catch (Throwable error) {
            result.putString("stream", "FAIL: "+error+"\n");
            finish(Activity.RESULT_CANCELED,result);
        }
    }
}
