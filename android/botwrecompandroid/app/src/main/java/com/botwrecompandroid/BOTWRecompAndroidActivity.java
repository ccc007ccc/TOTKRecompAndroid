// BOTWRecompAndroid on Android: SDL's activity runs the runtime (libmain.so, runtime/main.c). Before the native code starts
// this sets up what the desktop finds next to its executable (runtime/platform/android.c, runtime/paths.c):
// - the shipped resources (assets/res: the start screen's font, the icon) copied to <internal files>/res;
// - the Vulkan driver: the first driver in <external files>/drivers (a .so, or an adrenotools .zip: Turnip), copied to
//   internal storage (dlopen refuses external storage) and named in LARECOMP_VK_DRIVER; none there = the system's;
// - LARECOMP_NATIVE_LIB_DIR: where adrenotools finds its hook libraries;
// - the game code (totkgame.so + totkgame.so.id, built from the player's own game files: android/build_gamelib.sh) from
//   <external files>/extracted to <internal files>/res, where runtime/gamelib.c looks for it next to "the executable".
// - the game code's builder (docs/las/ANDROID-PORT.md, Fase 6): assets/gen (the generator, its Python's standard library,
//   zig's headers) unpacked to <internal files>/gen once per app version, named in LARECOMP_GEN_DIR; its programs
//   (libpymain.so, libzig.so) are in nativeLibraryDir;
// - the system's pickers for the start screen (runtime/ui/launcher.mm): the game files folder (exefs/ and romfs/ are
//   found in it; exefs/ is copied to its version's extracted/ - <external files>/extracted for 1.0.0, v160/ and v182/
//   inside it for the others, runtime/paths.h - and romfs.bin built there straight from romfs/) and the Vulkan driver
//   (a .zip copied to the drivers folder).
// The data root (game files, saves, settings) is <external files>: /sdcard/Android/data/com.totkrecomp.android/files.
// While in front it reports the battery and thermal state once a second for the performance overlay
// (runtime/ui/perf_overlay.mm): apps can't read those in /sys.
package com.botwrecompandroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.content.IntentFilter;
import android.content.res.AssetManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class BOTWRecompAndroidActivity extends SDLActivity {
    private static final String TAG = "TOTKRecompAndroid";

    private final Handler stats = new Handler(Looper.getMainLooper());
    private final Runnable report = new Runnable() {
        @Override
        public void run() {
            reportDeviceStats();
            stats.postDelayed(this, 1000);
        }
    };

    private static native void nativeDeviceStats(float battTempC, float battPct, float watts, int thermal, float headroom);
    // the start screen (runtime/ui/launcher.mm)
    private static native int nativeGameVersion(byte[] head);
    private static native void nativeImportLooking();  // (the picked folder is being searched: busy, and said so)
    private static native String nativeImportBegin(String name, int version);
    private static native void nativeImportProgress(long done, long total);
    private static native void nativeImportDone(int result);
    private static native void nativeImportDetail(String what);  // (a failure's cause, shown under it)
    private native String nativeBuildRomfs(String[] paths, long[] sizes);
    private static native boolean nativeCheckRomfs(int version, String[] paths, String versionTxt);
    private static native void nativeDriverPicked(String name);
    private static native void nativeModImportDone(String message);
    private static native void nativeLogFolderPicked(String name);

    private static final int REQ_GAME = 0x4c41, REQ_DRIVER = 0x4c42, REQ_MOD = 0x4c46, REQ_LOG = 0x4c47,
            REQ_DRIVER_RECOVER = 0x4c48;
    private static BOTWRecompAndroidActivity self;
    private WatchMapServer watchMap;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "shaderc_shared", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        try {
            File res = new File(getFilesDir(), "res");
            copyAssets("res", res);
            File games = new File(getExternalFilesDir(null), "extracted");
            copyIfChanged(new File(games, "totkgame.so"), new File(res, "totkgame.so"));
            copyIfChanged(new File(games, "totkgame.so.id"), new File(res, "totkgame.so.id"));
            setenv("LARECOMP_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir);
            setenv("LARECOMP_LAST_EXIT", lastExit());
            File gen = new File(getFilesDir(), "gen");
            unpackGen(gen);
            setenv("LARECOMP_GEN_DIR", gen.getAbsolutePath());
            if (getIntent() != null && SYSTEM_DRIVER.equals(getIntent().getAction())) dropCustomDriver();
            File driver = customDriver();
            setenv("LARECOMP_VK_DRIVER", driver != null ? driver.getAbsolutePath() : "");
            // testing: the runtime's LARECOMP_* switches from the launch intent, separated by commas (adb shell drops
            // the quotes around spaces), e.g. LARECOMP_VK_DRIVER= is the system driver:
            // adb shell am start -n com.totkrecomp.android/com.botwrecompandroid.BOTWRecompAndroidActivity --es env LARECOMP_NO_LAUNCHER=1,LARECOMP_VK_DRIVER=
            String env = getIntent() != null ? getIntent().getStringExtra("env") : null;
            // Opened again from the recent apps, Android hands back that test launch's intent: keep only which game
            // version it ran (a scripted input left in it ignored the touch screen - the menu looked frozen)
            boolean recents = getIntent() != null && (getIntent().getFlags() & Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0;
            if (env != null) {  // (a piece without '=' continues the previous value: LARECOMP_GPUPROF_AT=1500,1750)
                String name = null, value = null;
                for (String kv : env.trim().split("[,\\s]+")) {
                    int eq = kv.indexOf('=');
                    if (eq > 0) {
                        if (name != null && (!recents || KEEP_FROM_RECENTS.contains(name))) setenv(name, value);
                        name = kv.substring(0, eq);
                        value = kv.substring(eq + 1);
                    } else if (name != null) value += "," + kv;
                }
                if (name != null && (!recents || KEEP_FROM_RECENTS.contains(name))) setenv(name, value);
                // (debugging) the game code built on the phone, out where adb can read it: a crash's host addresses
                if (env.contains("LARECOMP_EXPORT_GAMELIB")) {
                    File lib = new File(res, "totkgame.so");
                    if (lib.exists()) try (InputStream in = new FileInputStream(lib)) {
                        copy(in, new File(getExternalFilesDir(null), "totkgame-export.so"));
                    }
                }
            }
        } catch (IOException | ErrnoException e) {
            Log.e(TAG, "setup: " + e);
        }
        self = this;
        setShowWhenLocked(true);
        setTurnScreenOn(true);
        super.onCreate(state);
        // SDL's own immersive full screen (no status / navigation bar, the camera cutout used): SDL's onCreate asks
        // for the windowed style, and keeps the bars hidden once its full screen mode is on
        setWindowStyle(true);
        watchMap = new WatchMapServer(this);
        if (getIntent() != null && WatchMapServer.ACTION.equals(getIntent().getAction()))
            stats.post(() -> watchMap.showSettings());
    }

    @Override
    protected void onResume() {
        super.onResume();
        stats.post(report);
        if (watchMap != null) watchMap.resume();
    }

    @Override
    protected void onPause() {
        stats.removeCallbacks(report);
        if (watchMap != null) watchMap.pause();
        super.onPause();
    }

    // Battery temperature, charge and power draw, and Android's thermal state (runtime/platform/android.c).
    private void reportDeviceStats() {
        try {
            Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            float temp = -1, pct = -1, watts = 0;
            if (b != null) {
                temp = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -10) / 10f;
                int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
                if (level >= 0 && scale > 0) pct = level * 100f / scale;
                int mv = b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0);
                BatteryManager bm = getSystemService(BatteryManager.class);
                long now = bm != null ? Math.abs(bm.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)) : 0;
                if (now > 0 && now < 20000) now *= 1000;  // (some devices report mA instead of uA)
                watts = now * (float) mv / 1e9f;
            }
            PowerManager pm = getSystemService(PowerManager.class);
            int thermal = pm != null ? pm.getCurrentThermalStatus() : -1;
            float headroom = pm != null ? pm.getThermalHeadroom(10) : -1;
            nativeDeviceStats(temp, pct, watts, thermal, Float.isNaN(headroom) ? -1 : headroom);
        } catch (Throwable t) {  // (also before libmain is loaded: UnsatisfiedLinkError)
            Log.w(TAG, "device stats: " + t);
        }
    }

    // ---- the game code's builder: assets/gen, unpacked only when its bundled content changes (runtime/gamebuild.c)
    private void unpackGen(File dir) throws IOException {
        String[] top = getAssets().list("gen");
        if (top == null || top.length == 0) return;  // (an APK without it: android/deps/build_gen_deps.sh not run)
        String stamp;
        try (InputStream in = getAssets().open("gen.id")) {
            stamp = "sha256:" + new String(readAll(in), java.nio.charset.StandardCharsets.US_ASCII).trim();
        } catch (IOException missingFingerprint) {
            // Compatibility with APKs built before gen.id was introduced.
            try {
                stamp = "install:" + getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            } catch (Exception e) {
                throw new IOException("Could not identify the bundled game builder", e);
            }
        }
        File st = new File(dir, ".stamp");
        if (st.exists() && stamp.equals(readSmall(st))) return;
        deleteTree(dir);
        copyAssetTree("gen", dir);
        try (OutputStream o = new FileOutputStream(st)) {
            o.write(stamp.getBytes());
        }
        Log.i(TAG, "unpacked the game code builder to " + dir);
    }

    private void copyAssetTree(String from, File to) throws IOException {
        AssetManager am = getAssets();
        String[] names = am.list(from);
        if (names == null || names.length == 0) {  // a file (or an empty folder)
            try (InputStream in = am.open(from)) {
                to.getParentFile().mkdirs();
                copy(in, to);
            } catch (IOException e) {
                to.mkdirs();
            }
            return;
        }
        to.mkdirs();
        for (String n : names) copyAssetTree(from + "/" + n, new File(to, n));
    }

    // ---- pickers (called from the start screen, on SDL's thread)
    public static void pickGameFolder() {
        self.runOnUiThread(() -> {
            try {
                self.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_GAME);
            } catch (Exception e) {
                Log.e(TAG, "folder picker: " + e);
                nativeImportDone(1);
            }
        });
    }

    public static void pickDriver() {
        self.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            try {
                self.startActivityForResult(i, REQ_DRIVER);
            } catch (Exception e) {
                Log.e(TAG, "file picker: " + e);
            }
        });
    }

    // Back to the system's Vulkan driver: the chosen package and its unpacked copy go (from the next start on)
    public static void useSystemDriver() {
        new Thread(() -> {
            self.dropCustomDriver();
            nativeDriverPicked("-");
        }, "driver").start();
    }

    private void dropCustomDriver() {
        deleteTree(new File(getExternalFilesDir(null), "drivers"));
        deleteTree(new File(getFilesDir(), "drivers"));
    }

    // The app icon's long-press shortcut (res/xml/shortcuts.xml): the system driver, settings out of reach or not - a
    // custom driver that draws garbage (Turnip_v26.3.0-R6 on an Adreno 830: stripes, the menus unreadable) or crashes.
    // Before the driver loads when it starts the app; a running one (a single instance) drops it and starts again.
    static final String SYSTEM_DRIVER = "com.botwrecompandroid.action.SYSTEM_DRIVER";
    // (android/run_ver.sh: the game version a test launch ran, and its own start without the launcher)
    static final List<String> KEEP_FROM_RECENTS = Arrays.asList("LARECOMP_DATA_ROOT", "LARECOMP_GAME_LIB",
            "LARECOMP_SAVE_SEED", "LARECOMP_NO_LAUNCHER");

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent != null && WatchMapServer.ACTION.equals(intent.getAction()) && watchMap != null)
            watchMap.showSettings();
        if (intent != null && SYSTEM_DRIVER.equals(intent.getAction())) {
            Log.i(TAG, "shortcut: back to the system driver");
            dropCustomDriver();
            restartApp();
        }
    }

    // a new process: the driver is loaded once, at start
    public static void restartApp() {
        self.runOnUiThread(() -> {
            Intent i = self.getPackageManager().getLaunchIntentForPackage(self.getPackageName());
            if (i != null) {
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                self.startActivity(i);
            }
            System.exit(0);
        });
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        if (req == REQ_GAME) {
            Uri tree = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            if (tree == null) nativeImportDone(1);
            else new Thread(() -> importGame(tree), "import").start();
            return;
        }
        if (req == REQ_MOD) {
            Uri tree = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            if (tree == null) nativeModImportDone("2|");
            else new Thread(() -> importMod(tree), "mod-import").start();
            return;
        }
        if (req == REQ_DRIVER) {
            Uri f = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            if (f != null) new Thread(() -> importDriver(f), "driver").start();
            return;
        }
        if (req == REQ_DRIVER_RECOVER) {
            Uri f = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            if (f == null) showDriverError(null);
            else new Thread(() -> {
                if (!installDriver(this, f).isEmpty()) restartApp();
                else runOnUiThread(() -> showDriverError(getString(R.string.driver_not_a_package)));
            }, "driver").start();
            return;
        }
        if (req == REQ_LOG) {
            Uri tree = result == Activity.RESULT_OK && data != null ? data.getData() : null;
            if (tree != null) {
                try {
                    getContentResolver().takePersistableUriPermission(tree,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                    getSharedPreferences("botwrecomp", MODE_PRIVATE).edit().putString("logTree", tree.toString()).apply();
                } catch (Exception e) {
                    Log.e(TAG, "log folder permission", e);
                }
            }
            nativeLogFolderPicked(logFolderName());  // ("": cancelled, or no write access)
            return;
        }
        super.onActivityResult(req, result, data);
    }

    // ---- the log file (Settings > Prelaunch > Driver compatibility, runtime/platform/android.c; LASRecompAndroid's)
    // goes to a folder the player picks (Android keeps Android/data/ out of reach of file managers): botwrecomp-log.txt,
    // the run before's kept as botwrecomp-log-previous.txt, both openable on the phone and sendable with a bug report
    private static final String LOG_NAME = "botwrecomp-log.txt", LOG_PREVIOUS = "botwrecomp-log-previous.txt";

    public static void pickLogFolder() {
        self.runOnUiThread(() -> {
            try {
                self.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_LOG);
            } catch (Exception e) {
                Log.e(TAG, "log folder picker", e);
                nativeLogFolderPicked("");
            }
        });
    }

    private static Uri logTree() {  // the picked folder while the app may still write there (and it's still there)
        String s = self.getSharedPreferences("botwrecomp", MODE_PRIVATE).getString("logTree", null);
        if (s == null) return null;
        Uri tree = Uri.parse(s);
        boolean allowed = false;
        for (android.content.UriPermission p : self.getContentResolver().getPersistedUriPermissions())
            allowed |= p.getUri().equals(tree) && p.isWritePermission();
        if (!allowed) return null;
        Uri dir = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor c = self.getContentResolver().query(dir, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID},
                                                        null, null, null)) {
            return c != null && c.moveToFirst() ? tree : null;
        } catch (Exception e) {  // (the folder was deleted)
            return null;
        }
    }

    // The folder's name for the settings page ("Download/BOTWRecomp"); "" with none
    public static String logFolderName() {
        Uri tree = logTree();
        if (tree == null) return "";
        String id = DocumentsContract.getTreeDocumentId(tree);  // e.g. "primary:Download/BOTWRecomp"
        int colon = id.indexOf(':');
        String path = colon >= 0 ? id.substring(colon + 1) : id;
        return path.isEmpty() ? "/" : path;
    }

    // A file descriptor to write the log to (native code owns it), -1 without a usable folder. first: the run's first
    // open - the last run's log becomes the previous one and a new file starts; otherwise this run's is appended to.
    public static int openLog(boolean first) {
        Uri tree = logTree();
        if (tree == null) return -1;
        try {
            ContentResolver cr = self.getContentResolver();
            String treeId = DocumentsContract.getTreeDocumentId(tree);
            List<Doc> kids = self.children(tree, treeId);
            Doc cur = named(kids, LOG_NAME), prev = named(kids, LOG_PREVIOUS);
            if (first && cur != null) {
                if (prev != null) DocumentsContract.deleteDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, prev.id));
                DocumentsContract.renameDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, cur.id), LOG_PREVIOUS);
                cur = null;
            }
            Uri file = cur != null ? DocumentsContract.buildDocumentUriUsingTree(tree, cur.id)
                : DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, treeId), "text/plain", LOG_NAME);
            if (file == null) return -1;
            android.os.ParcelFileDescriptor pfd = cr.openFileDescriptor(file, first ? "wt" : "wa");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Log.w(TAG, "log file in the picked folder", e);
            return -1;
        }
    }

    // A document in the picked tree: id, name, folder or not, size
    private static final class Doc {
        String id, name;
        boolean dir;
        long size;
    }

    private List<Doc> children(Uri tree, String id) {
        List<Doc> out = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(u, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                Doc d = new Doc();
                d.id = c.getString(0);
                d.name = c.getString(1);
                d.dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                d.size = c.isNull(3) ? -1 : c.getLong(3);  // (-1: the provider doesn't say - listFiles refuses it)
                out.add(d);
            }
        }
        return out;
    }

    private static Doc named(List<Doc> l, String name) {
        for (Doc d : l) if (d.name != null && d.name.equals(name)) return d;
        return null;
    }

    // exefs: the folder with main and main.npdm; romfs: the folder with Pack - in the picked folder or a few
    // levels below it (as runtime/ui/launcher.mm find_game)
    private Doc exefs, romfs;
    private boolean packages;
    // A game code built already (totkgame.so with its .id, and .game: the game version it's for - a package made on a
    // PC from the same files), used instead of building it on the phone when it's for this version and this app
    private Doc preLib, preId, preGame;

    private void find(Uri tree, Doc dir, int depth) {
        if (depth > 4 || (exefs != null && romfs != null)) return;
        List<Doc> kids = children(tree, dir.id);
        if (preLib == null && named(kids, "totkgame.so") != null && named(kids, "totkgame.so.id") != null &&
            named(kids, "totkgame.so.game") != null) {
            preLib = named(kids, "totkgame.so");
            preId = named(kids, "totkgame.so.id");
            preGame = named(kids, "totkgame.so.game");
        }
        if (exefs == null && named(kids, "main") != null && named(kids, "main.npdm") != null) exefs = dir;
        Doc rc = named(kids, "Pack");
        if (romfs == null && rc != null && rc.dir) romfs = dir;
        // (romfs/'s own folders hold no exefs: through the file provider, listing their ~40000 files took long enough
        // for the setup to look stuck - issue #17)
        if (dir == romfs || dir == exefs) return;
        for (Doc k : kids) {
            String n = k.name == null ? "" : k.name.toLowerCase();
            if (n.endsWith(".nsp") || n.endsWith(".xci") || n.endsWith(".nca") || n.endsWith(".nsz") || n.endsWith(".xcz")) packages = true;
            if (k.dir && !"Pack".equals(k.name)) find(tree, k, depth + 1);
        }
    }

    private long treeBytes(Uri tree, Doc dir) {
        long n = 0;
        for (Doc k : children(tree, dir.id)) n += k.dir ? treeBytes(tree, k) : Math.max(k.size, 0);
        return n;
    }

    // romfs/'s files (relative paths): nativeBuildRomfs builds romfs.bin straight from them, no copy of the folder
    // (LASRecompAndroid's)
    private Uri romfsTree;
    private final List<Doc> romfsFiles = new ArrayList<>();
    private final List<String> romfsPaths = new ArrayList<>();

    private void listFiles(Uri tree, Doc dir, String prefix) throws IOException {
        for (Doc k : children(tree, dir.id)) {
            String rel = prefix + k.name;
            if (k.dir) {
                listFiles(tree, k, rel + "/");
                continue;
            }
            if (k.size < 0) throw new IOException("The file provider did not report a size for " + k.name +
                    ". Copy the extracted game files to a local folder and select it again.");
            romfsFiles.add(k);
            romfsPaths.add(rel);
        }
    }

    // (called by nativeBuildRomfs) file i's data as a descriptor the native side reads and closes; -1 when it can't be
    // opened, romfsOpenError saying why. Three tries: a setup stopped once in ~41000 opens with "can't read" (#17).
    // Through one client of the file provider for the whole image (ContentResolver's openFileDescriptor took and dropped
    // the provider for each file, and its descriptors held it until the garbage collector came by); unstable: the
    // provider dying fails an open (a new client, tried again), not this app.
    private String romfsOpenError;
    private android.content.ContentProviderClient romfsClient;

    @SuppressWarnings("unused")
    private int openRomfsFile(int i) {
        Uri u = DocumentsContract.buildDocumentUriUsingTree(romfsTree, romfsFiles.get(i).id);
        for (int attempt = 1; ; attempt++) {
            try {
                if (romfsClient == null) romfsClient = getContentResolver().acquireUnstableContentProviderClient(u);
                if (romfsClient == null) throw new IOException("no file provider for " + u.getAuthority());
                android.os.ParcelFileDescriptor pfd = romfsClient.openFile(u, "r");
                if (pfd == null) throw new IOException("the file provider gave no file");
                return pfd.detachFd();
            } catch (Exception e) {
                String why = e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
                Log.w(TAG, "opening " + romfsPaths.get(i) + ", try " + attempt + ": " + why);
                if (e instanceof android.os.RemoteException) closeRomfsClient();  // (the provider died: a new one)
                if (attempt == 3) {
                    romfsOpenError = why;
                    return -1;
                }
                try {
                    Thread.sleep(attempt * 1000L);
                } catch (InterruptedException ie) {
                    return -1;
                }
            }
        }
    }

    private void closeRomfsClient() {
        if (romfsClient != null) romfsClient.close();
        romfsClient = null;
    }

    private long copied, total, reported;

    private void copyTree(Uri tree, Doc dir, File to) throws IOException {
        to.mkdirs();
        ContentResolver cr = getContentResolver();
        for (Doc k : children(tree, dir.id)) {
            File dst = new File(to, k.name);
            if (k.dir) {
                copyTree(tree, k, dst);
                continue;
            }
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, k.id));
                 OutputStream o = new FileOutputStream(dst)) {
                byte[] buf = new byte[1 << 20];
                for (int r; (r = in.read(buf)) > 0; ) {
                    o.write(buf, 0, r);
                    copied += r;
                    if (copied - reported > (16 << 20)) {
                        reported = copied;
                        nativeImportProgress(copied, total);
                    }
                }
            }
        }
    }

    private String readText(Uri tree, Doc d) {
        try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id))) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8) : "";
        } catch (Exception e) {
            return "";
        }
    }

    private void importGame(Uri tree) {
        int result;
        nativeImportLooking();
        try {
            exefs = romfs = null;
            preLib = preId = preGame = null;
            packages = false;
            Doc root = new Doc();
            root.id = DocumentsContract.getTreeDocumentId(tree);
            root.dir = true;
            find(tree, root, 0);
            if (exefs == null || romfs == null) {
                nativeImportDone(packages ? 3 : 2);
                return;
            }
            Doc main = named(children(tree, exefs.id), "main");
            byte[] head = new byte[0x48];
            try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, main.id))) {
                int got = 0;
                for (int r; got < head.length && (r = in.read(head, got, head.length - got)) > 0; ) got += r;
            }
            int version = nativeGameVersion(head);  // (1.0.0, 1.6.0, 1.8.2: each set up in its own folder)
            if (version == 0) {
                nativeImportDone(4);
                return;
            }
            // (listed before nativeImportBegin removes an earlier setup: a folder that can't be used leaves it alone)
            romfsTree = tree;
            romfsFiles.clear();
            romfsPaths.clear();
            listFiles(tree, romfs, "");
            int vt = romfsPaths.indexOf("System/Version.txt");  // (its version against the exefs/'s, and its file count)
            if (!nativeCheckRomfs(version, romfsPaths.toArray(new String[0]), vt >= 0 ? readText(tree, romfsFiles.get(vt)) : "")) {
                romfsFiles.clear();
                romfsPaths.clear();
                nativeImportDone(6);
                return;
            }
            long romfsBytes = 0;
            for (Doc d : romfsFiles) romfsBytes = Math.addExact(romfsBytes, d.size);
            String name = DocumentsContract.getTreeDocumentId(tree);
            File games = new File(nativeImportBegin(name.substring(name.lastIndexOf(':') + 1), version));
            copied = reported = 0;
            total = Math.addExact(romfsBytes, treeBytes(tree, exefs));
            copyTree(tree, exefs, new File(games, "exefs"));
            if (preLib != null && Integer.toString(version).equals(readText(tree, preGame).trim())) {
                // (the native side checks its id against this app's and builds it on the phone when they differ)
                copyDoc(tree, preId, new File(games, "totkgame.so.id"));
                copyDoc(tree, preLib, new File(games, "totkgame.so"));
                Log.i(TAG, "game code from the picked folder: " + preLib.size + " bytes");
            }
            nativeImportProgress(copied, total);
            long[] sizes = new long[romfsFiles.size()];
            for (int i = 0; i < sizes.length; i++) sizes[i] = romfsFiles.get(i).size;
            romfsOpenError = null;
            String failed;
            try {
                failed = nativeBuildRomfs(romfsPaths.toArray(new String[0]), sizes);
            } finally {
                closeRomfsClient();
            }
            romfsFiles.clear();
            romfsPaths.clear();
            if (failed != null) {
                if (romfsOpenError != null) nativeImportDetail(failed + " - " + romfsOpenError);  // (in place of the native side's)
                Log.e(TAG, "game data image: " + failed);
                nativeImportDone(5);
                return;
            }
            result = 0;
        } catch (Exception e) {
            Log.e(TAG, "game files: " + e);
            nativeImportDetail(e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : ""));
            result = 5;
        }
        nativeImportDone(result);
    }

    private void importDriver(Uri f) {
        nativeDriverPicked(installDriver(this, f));
    }

    // A driver package (.zip or .adpkg - a zip too -, or a bare .so) into <external files>/drivers, in place of any
    // there (customDriver loads it at the next start). Its name, or "" (not a driver package, or the copy failed).
    // Also DriverPickActivity's.
    static String installDriver(Context ctx, Uri f) {
        String name = "driver.zip";
        try (Cursor c = ctx.getContentResolver().query(f, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) name = c.getString(0);
        } catch (Exception ignored) {
        }
        if (!DRIVER_FILE.matcher(name).find()) return "";
        try {
            File dir = new File(ctx.getExternalFilesDir(null), "drivers");
            deleteTree(dir);
            dir.mkdirs();
            File dst = new File(dir, name);
            try (InputStream in = ctx.getContentResolver().openInputStream(f)) {
                copy(in, dst);
            }
            if (driverKind(dst) == 0) {  // (named like one, but neither a zip nor a library)
                deleteTree(dir);
                return "";
            }
            return name;
        } catch (Exception e) {
            Log.e(TAG, "driver: " + e);
            return "";
        }
    }

    public static boolean runtimeErrorDialog(String text, boolean nativeCrash) {
        return RuntimeErrorDialog.show(self, text, nativeCrash);
    }

    // ---- a graphics driver error that stops startup (runtime/platform/android.c plat_fatal_notice): the driver is
    // loaded before the start screen, so the Settings rows that choose it can't be reached when it fails. This dialog
    // installs a driver package, or goes back to the system's driver, and starts the app again; Close dismisses it. Blocks
    // the calling (native) thread until the player closes it. LAS Recomp for Android 1c05238, from
    // LASRecomp-Android-Plus (MIT); the app icon's long-press "Choose driver" (DriverPickActivity) does the same.
    private static final Object dlgLock = new Object();
    private static boolean dlgDone;
    private static String dlgText;

    public static boolean driverErrorDialog(String text) {
        BOTWRecompAndroidActivity a = self;
        if (a == null) return false;
        synchronized (dlgLock) { dlgDone = false; dlgText = text; }
        a.runOnUiThread(() -> a.showDriverError(null));
        synchronized (dlgLock) {
            while (!dlgDone) {
                try { dlgLock.wait(); } catch (InterruptedException e) { break; }
            }
        }
        return true;
    }

    private void showDriverError(String extra) {
        // (the native message is in English: what to do comes first, in the phone's language - issue #13)
        String msg = getString(R.string.driver_error_intro) + "\n" + dlgText + (extra != null ? "\n\n" + extra : "");
        AlertDialog.Builder b = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.driver_error_title).setCancelable(false)
            .setPositiveButton(R.string.driver_install, (d, w) -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                try {
                    startActivityForResult(i, REQ_DRIVER_RECOVER);
                } catch (Exception e) {
                    Log.e(TAG, "file picker: " + e);
                    showDriverError(getString(R.string.driver_not_a_package));
                }
            })
            .setNegativeButton(R.string.driver_close, (d, w) -> {
                synchronized (dlgLock) { dlgDone = true; dlgLock.notifyAll(); }
            });
        File[] custom = new File(getExternalFilesDir(null), "drivers").listFiles((dir, n) -> DRIVER_FILE.matcher(n).find());
        if (custom != null && custom.length > 0)
            b.setNeutralButton(R.string.driver_system, (d, w) -> { dropCustomDriver(); restartApp(); });
        b.setView(RuntimeErrorDialog.messageView(b.getContext(), msg));
        b.show();
    }

    // driver packages by name, and by content: 1 a zip (adrenotools' .zip / .adpkg), 2 a library (ELF), 0 neither
    private static final java.util.regex.Pattern DRIVER_FILE =
            java.util.regex.Pattern.compile("\\.(so|zip|adpkg)$", java.util.regex.Pattern.CASE_INSENSITIVE);
    private static int driverKind(File f) throws IOException {
        byte[] m = new byte[4];
        try (InputStream in = new FileInputStream(f)) {
            if (in.read(m) != 4) return 0;
        }
        if (m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4) return 1;
        if (m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') return 2;
        return 0;
    }

    // (by size and date: the game code is ~100 MB)
    private static void copyIfChanged(File src, File dst) throws IOException {
        if (!src.exists()) return;
        if (dst.exists() && dst.length() == src.length() && dst.lastModified() >= src.lastModified()) return;
        try (InputStream in = new FileInputStream(src)) {
            copy(in, dst);
        }
        Log.i(TAG, "copied " + src + " -> " + dst);
    }

    // How the previous run ended (ApplicationExitInfo), for the log: a setup that vanished with nothing in the log was
    // the system closing the app - for memory or something else (issue #18). "" when unknown.
    private static final String[] EXIT_REASONS = {"unknown", "exit self", "signaled", "low memory", "crash", "native crash",
            "not responding", "initialization failure", "permission change", "excessive resource usage", "user requested",
            "user stopped", "dependency died", "other", "freezer", "package state change", "package updated"};

    private String lastExit() {
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
            List<android.app.ApplicationExitInfo> l = am.getHistoricalProcessExitReasons(getPackageName(), 0, 1);
            if (l.isEmpty()) return "";
            android.app.ApplicationExitInfo e = l.get(0);
            int r = e.getReason();
            return String.format(java.util.Locale.ROOT, "%s (reason %d, status %d) at %tF %<tT, pss %d MB, rss %d MB: %s",
                    r >= 0 && r < EXIT_REASONS.length ? EXIT_REASONS[r] : "?", r, e.getStatus(), e.getTimestamp(),
                    e.getPss() / 1024, e.getRss() / 1024, e.getDescription());
        } catch (Exception e) {
            return "";
        }
    }

    private static void setenv(String name, String value) throws ErrnoException {
        Os.setenv(name, value, true);
    }

    private void copyAssets(String from, File to) throws IOException {
        AssetManager am = getAssets();
        String[] names = am.list(from);
        if (names == null) return;
        to.mkdirs();
        for (String n : names) {
            try (InputStream in = am.open(from + "/" + n)) {
                copy(in, new File(to, n));
            }
        }
    }

    // The first driver in <external files>/drivers, in internal storage (copied again only when it changed). A zip's
    // library is the one its meta.json names (adrenotools' libraryName: packages with support libraries beside it, as
    // RADV-Xclipse's), else one named like a Vulkan driver, else its only / last .so.
    private File customDriver() throws IOException {
        File src = new File(getExternalFilesDir(null), "drivers");
        src.mkdirs();
        File[] files = src.listFiles((d, n) -> DRIVER_FILE.matcher(n).find());
        if (files == null || files.length == 0) return null;
        Arrays.sort(files);
        File f = files[0];
        File dir = new File(getFilesDir(), "drivers/" + DRIVER_FILE.matcher(f.getName()).replaceAll(""));
        File stamp = new File(dir, ".source");  // (the package's size:date, then the library chosen in it)
        String id = f.length() + ":" + f.lastModified();
        String[] was = stamp.exists() ? readSmall(stamp).split("\n") : new String[0];
        File lib = was.length == 2 ? new File(dir, was[1]) : null;
        if (lib != null && id.equals(was[0]) && lib.exists()) return lib;
        deleteTree(dir);
        dir.mkdirs();
        int kind = driverKind(f);
        if (kind == 2) {
            try (InputStream in = new FileInputStream(f)) {
                lib = copy(in, new File(dir, f.getName().toLowerCase().endsWith(".so") ? f.getName() : "libvulkan_custom.so"));
            }
        } else if (kind == 1) {
            lib = null;
            File named = null, vulkan = null;
            String libraryName = null;
            try (ZipInputStream z = new ZipInputStream(new FileInputStream(f))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    String n = new File(e.getName()).getName();
                    if (e.isDirectory()) continue;
                    if (n.equals("meta.json")) {
                        try {
                            libraryName = new org.json.JSONObject(new String(readAll(z))).optString("libraryName", null);
                        } catch (org.json.JSONException bad) {
                            Log.w(TAG, "driver meta.json: " + bad);
                        }
                    } else if (n.endsWith(".so")) {
                        lib = copyNoClose(z, new File(dir, n));
                        if (n.contains("vulkan")) vulkan = lib;
                    }
                }
            }
            if (libraryName != null) named = new File(dir, new File(libraryName).getName());
            if (named != null && named.exists()) lib = named;
            else if (vulkan != null) lib = vulkan;
        } else {
            Log.w(TAG, "Vulkan driver: " + f.getName() + " is neither a zip nor a library");
            lib = null;
        }
        if (lib != null) {
            try (OutputStream o = new FileOutputStream(stamp)) {
                o.write((id + "\n" + lib.getName()).getBytes());
            }
        }
        Log.i(TAG, "Vulkan driver: " + f.getName() + " -> " + lib);
        return lib;
    }

    private static String readSmall(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            return new String(readAll(in));
        }
    }

    // (InputStream.readAllBytes is Android 13+: on 11 and 12 the app died at launch, issue #3)
    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
        return o.toByteArray();
    }

    private static File copy(InputStream in, File dst) throws IOException {
        return copyNoClose(in, dst);
    }

    private static File copyNoClose(InputStream in, File dst) throws IOException {
        try (OutputStream o = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
        }
        return dst;
    }

    // ---- game mods (the menu's Mods page, runtime/ui/mods_tab.mm; runtime/mods.c; LASRecompAndroid's): a mod's folder
    // - the one with romfs/ in it, or a few levels above it - copied into <dir>/<its name>/ (dir: the running game
    // version's mods/): romfs/ and the notes (.txt) beside it. Answers nativeModImportDone "1|<name>" imported,
    // "0|<reason>" failed, "2|" cancelled.
    private static String modsDir;

    public static void importMod(String dir) {
        modsDir = dir;
        self.runOnUiThread(() -> {
            try {
                self.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_MOD);
            } catch (Exception e) {
                nativeModImportDone("0|" + e.getMessage());
            }
        });
    }

    // the folder holding romfs/: dir itself or one below it (3 levels at most)
    private Doc modFolder(Uri tree, Doc dir, int depth) {
        List<Doc> kids = children(tree, dir.id);
        Doc r = named(kids, "romfs");
        if (r != null && r.dir) return dir;
        if (depth >= 3) return null;
        for (Doc k : kids) {
            if (!k.dir) continue;
            Doc f = modFolder(tree, k, depth + 1);
            if (f != null) return f;
        }
        return null;
    }

    private void copyDocs(Uri tree, Doc dir, File to) throws IOException {
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("Could not create " + to.getName());
        for (Doc k : children(tree, dir.id)) {
            if (k.name == null || k.name.contains("/")) continue;
            if (k.dir) copyDocs(tree, k, new File(to, k.name));
            else copyDoc(tree, k, new File(to, k.name));
        }
    }

    private void copyDoc(Uri tree, Doc d, File to) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id))) {
            if (in == null) throw new IOException("Could not open " + d.name);
            copy(in, to);
        }
    }

    private void importMod(Uri tree) {
        File stage = null;
        try {
            Doc root = new Doc();
            root.id = DocumentsContract.getTreeDocumentId(tree);
            root.dir = true;
            try (Cursor c = getContentResolver().query(DocumentsContract.buildDocumentUriUsingTree(tree, root.id),
                    new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst()) root.name = c.getString(0);
            }
            Doc mod = modFolder(tree, root, 0);
            if (mod == null) {
                nativeModImportDone("0|No romfs folder in the selected folder.");
                return;
            }
            String name = mod.name == null ? "mod" : mod.name.replaceAll("[^A-Za-z0-9 ._()\\-]", "_").trim();
            if (name.isEmpty() || name.startsWith(".")) name = "mod";
            if (name.length() > 80) name = name.substring(0, 80);
            File mods = new File(modsDir != null ? modsDir : new File(getExternalFilesDir(null), "mods").getPath());
            if (!mods.isDirectory() && !mods.mkdirs()) throw new IOException("Could not create the mods folder.");
            stage = new File(mods, ".import-" + name);
            deleteTree(stage);
            Doc romfsDoc = named(children(tree, mod.id), "romfs");
            copyDocs(tree, romfsDoc, new File(stage, "romfs"));
            for (Doc k : children(tree, mod.id))  // (its notes: the menu shows one as its description)
                if (!k.dir && k.name != null && k.name.toLowerCase().endsWith(".txt")) copyDoc(tree, k, new File(stage, k.name));
            File dst = new File(mods, name);
            deleteTree(dst);
            if (!stage.renameTo(dst)) throw new IOException("Could not move the mod into place.");
            stage = null;
            nativeModImportDone("1|" + name);
        } catch (Exception e) {
            Log.e(TAG, "mod import", e);
            nativeModImportDone("0|" + e.getMessage());
        } finally {
            if (stage != null) deleteTree(stage);
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
