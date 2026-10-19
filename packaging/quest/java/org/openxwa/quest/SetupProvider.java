package org.openxwa.quest;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Lets the PC launcher put the game files on the headset without starting
 * the game, so the headset can stay on the desk. Everything is written by
 * the app itself: folders and files made over USB belong to the "shell"
 * user and the app cannot read them.
 *
 *   adb shell content call --uri content://org.openxwa.quest.setup --method mkdirs --arg 'XWA:pilot-import'
 *       makes folders (':' separated, relative to the app's files folder),
 *       and the app's obb folder.
 *   adb push files.zip /sdcard/Android/obb/org.openxwa.quest/openxwa-import.zip
 *       (the obb folder is the one place where the app reads what adb copied;
 *       or, slower and only a fallback:
 *   adb exec-in content write --uri content://org.openxwa.quest.setup/import < files.zip
 *       streams it into files/.openxwa-import.zip, written by the app)
 *   adb shell content call --uri content://org.openxwa.quest.setup --method unzip --arg XWA
 *       unpacks the zip (obb first) into files/XWA (CRC checked by
 *       ZipInputStream), then deletes it.
 *
 * Paths outside the files folder are skipped. Only adb (shell) or root may
 * call it. Results are Bundles: base, made/asked (mkdirs), files/skipped
 * (unzip), or error=<exception>.
 *
 * The local repack build adds this class as a hand-assembled classes2.dex
 * (packaging/quest/tools/dexgen_setup_provider.py); keep the two in step.
 */
public class SetupProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    private static boolean allowed() {
        int uid = Binder.getCallingUid();
        return uid == 2000 || uid == 0;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!allowed()) {
            return null;
        }
        File zip = new File(getContext().getExternalFilesDir(null), ".openxwa-import.zip");
        zip.delete(); // a fresh file even if "w" does not truncate
        return ParcelFileDescriptor.open(zip, ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        if (!allowed()) {
            return null;
        }
        try {
            return run(method, arg);
        } catch (Throwable t) {
            Bundle out = new Bundle();
            out.putString("error", t.toString());
            return out;
        }
    }

    private Bundle run(String method, String arg) throws IOException {
        File base = getContext().getExternalFilesDir(null);
        if (base == null) {
            return null;
        }
        String prefix = base.getCanonicalPath().concat("/");
        getContext().getObbDir(); // made by the system, so it belongs to the app
        if ("unzip".equals(method)) {
            return unzip(base, prefix, arg);
        }
        return mkdirs(base, prefix, arg);
    }

    private Bundle mkdirs(File base, String prefix, String arg) throws IOException {
        if (arg == null) {
            arg = "XWA";
        }
        String[] parts = arg.split(":");
        int made = 0;
        for (int i = 0; i < parts.length; i++) {
            File dir = new File(base, parts[i]);
            if (dir.getCanonicalPath().startsWith(prefix)) {
                dir.mkdirs();
                if (dir.isDirectory()) {
                    made++;
                }
            }
        }
        Bundle out = new Bundle();
        out.putString("base", prefix);
        out.putInt("made", made);
        out.putInt("asked", parts.length);
        return out;
    }

    private Bundle unzip(File base, String prefix, String arg) throws IOException {
        if (arg == null) {
            arg = "XWA";
        }
        File dest = new File(base, arg);
        File zip = new File(getContext().getObbDir(), "openxwa-import.zip");
        if (!zip.exists()) {
            zip = new File(base, ".openxwa-import.zip");
        }
        ZipInputStream in = new ZipInputStream(new BufferedInputStream(new FileInputStream(zip), 65536));
        byte[] buf = new byte[65536];
        int files = 0;
        int skipped = 0;
        ZipEntry e;
        while ((e = in.getNextEntry()) != null) {
            File out = new File(dest, e.getName());
            if (!out.getCanonicalPath().startsWith(prefix)) {
                skipped++;
                continue;
            }
            if (e.isDirectory()) {
                out.mkdirs();
                continue;
            }
            out.getParentFile().mkdirs();
            out.delete();
            FileOutputStream o = new FileOutputStream(out);
            int n;
            while ((n = in.read(buf, 0, 65536)) > 0) {
                o.write(buf, 0, n);
            }
            o.close();
            files++;
        }
        in.close();
        zip.delete();
        Bundle out = new Bundle();
        out.putString("base", prefix);
        out.putString("zip", zip.getPath());
        out.putInt("files", files);
        out.putInt("skipped", skipped);
        return out;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
