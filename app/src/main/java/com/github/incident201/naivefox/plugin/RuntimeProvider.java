/*
 * Copyright (C) 2026 NaiveFox Android Plugin contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

package com.github.incident201.naivefox.plugin;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Multi-file native plugin provider implementing Exclave's slow-path contract.
 *
 * <p>The provider deliberately does not advertise an executable fast path. Exclave first asks
 * for {@code sagernet:getExecutable}, receives {@code null}, then queries and copies every path
 * below into its own no-backup plugin directory.</p>
 */
public final class RuntimeProvider extends ContentProvider {
    private static final String MIME_TYPE = "application/x-elf";
    private static final String METHOD_GET_EXECUTABLE = "sagernet:getExecutable";
    private static final String COLUMN_PATH = "path";
    private static final String COLUMN_MODE = "mode";
    private static final String ENTRY_PATH = "naive-plugin";
    private static final String LAUNCHER_LIBRARY = "libnaivefox_launcher.so";
    private static final String MANIFEST_ASSET = "plugin/runtime/manifest.json";
    private static final Pattern OCTAL_MODE = Pattern.compile("^[0-7]{4}$");
    private static final String[] DEFAULT_PROJECTION = {COLUMN_PATH, COLUMN_MODE};

    private Map<String, PluginFile> pluginFiles = Collections.emptyMap();
    private boolean arm64Supported;

    private static final class PluginFile {
        final String providerPath;
        final String assetPath;
        final int mode;
        final boolean launcher;

        PluginFile(String providerPath, String assetPath, int mode, boolean launcher) {
            this.providerPath = providerPath;
            this.assetPath = assetPath;
            this.mode = mode;
            this.launcher = launcher;
        }
    }

    @Override
    public boolean onCreate() {
        arm64Supported = false;
        for (String abi : Build.SUPPORTED_64_BIT_ABIS) {
            if ("arm64-v8a".equals(abi)) {
                arm64Supported = true;
                break;
            }
        }
        pluginFiles = Collections.unmodifiableMap(loadPluginFiles());
        return true;
    }

    private Map<String, PluginFile> loadPluginFiles() {
        LinkedHashMap<String, PluginFile> result = new LinkedHashMap<>();
        putUnique(result, new PluginFile(ENTRY_PATH, null, 0755, true));
        putUnique(result, new PluginFile("manifest.json", MANIFEST_ASSET, 0644, false));

        try (InputStream stream = providerContext().getAssets().open(MANIFEST_ASSET);
             ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16 * 1024];
            int count;
            while ((count = stream.read(buffer)) != -1) {
                bytes.write(buffer, 0, count);
            }
            String json = bytes.toString(java.nio.charset.StandardCharsets.UTF_8.name());
            JSONObject manifest = new JSONObject(json);
            requireManifestValue(manifest, "format_version", 1);
            requireManifestValue(manifest, "product", "naivefox-android-embedded");
            requireManifestValue(manifest, "target", "android-aarch64");
            requireManifestValue(manifest, "abi", "arm64-v8a");
            if (manifest.getInt("min_android_api") > 26) {
                throw new IllegalStateException("NaiveFox runtime requires Android API above 26");
            }

            JSONArray symbols = manifest.getJSONArray("exported_symbols");
            requireArrayString(symbols, "NaiveFoxRunEmbedded");
            requireArrayString(symbols, "NaiveFoxRequestStop");
            requireArrayString(symbols, "NaiveFoxVersion");

            String runtimePath = requireSafeRelativePath(manifest.getString("runtime_path"));
            String expectedLibxul = runtimePath + "/libxul.so";
            boolean hasLibxul = false;
            JSONArray files = manifest.getJSONArray("files");
            for (int index = 0; index < files.length(); index++) {
                JSONObject item = files.getJSONObject(index);
                String relativePath = requireSafeRelativePath(item.getString("path"));
                String modeText = item.getString("mode");
                if (!OCTAL_MODE.matcher(modeText).matches()) {
                    throw new IllegalStateException("Invalid runtime mode for " + relativePath);
                }
                int mode = Integer.parseInt(modeText, 8);
                if ((mode & ~0777) != 0) {
                    throw new IllegalStateException("Unsafe runtime mode for " + relativePath);
                }
                String providerPath = flatFileName(relativePath);
                putUnique(result, new PluginFile(
                        providerPath,
                        "plugin/runtime/" + relativePath,
                        mode,
                        false));
                hasLibxul |= expectedLibxul.equals(relativePath);
            }
            if (!hasLibxul) {
                throw new IllegalStateException("Runtime manifest does not contain libxul.so");
            }
            return result;
        } catch (IOException | JSONException exception) {
            throw new IllegalStateException("Cannot load packaged NaiveFox manifest", exception);
        }
    }

    private static void requireManifestValue(JSONObject object, String name, Object expected)
            throws JSONException {
        Object actual = object.get(name);
        if (!expected.equals(actual)) {
            throw new IllegalStateException(
                    "Unexpected NaiveFox manifest " + name + ": " + actual);
        }
    }

    private static void requireArrayString(JSONArray array, String expected) throws JSONException {
        for (int index = 0; index < array.length(); index++) {
            if (expected.equals(array.getString(index))) {
                return;
            }
        }
        throw new IllegalStateException("NaiveFox manifest is missing symbol " + expected);
    }

    private static String requireSafeRelativePath(String path) {
        if (path.isEmpty() || path.startsWith("/") || path.endsWith("/")
                || path.contains("\\") || path.contains("//")) {
            throw new IllegalStateException("Unsafe runtime path: " + path);
        }
        for (String part : path.split("/")) {
            if (part.isEmpty() || ".".equals(part) || "..".equals(part)) {
                throw new IllegalStateException("Unsafe runtime path: " + path);
            }
        }
        return path;
    }

    private static void putUnique(Map<String, PluginFile> files, PluginFile file) {
        if (files.put(file.providerPath, file) != null) {
            throw new IllegalStateException("Duplicate plugin path: " + file.providerPath);
        }
    }

    private static String flatFileName(String path) {
        int separator = path.lastIndexOf('/');
        return separator == -1 ? path : path.substring(separator + 1);
    }

    private android.content.Context providerContext() {
        android.content.Context context = getContext();
        if (context == null) {
            throw new IllegalStateException("ContentProvider is not attached");
        }
        return context;
    }

    private void assertReady() {
        if (!arm64Supported) {
            throw new IllegalStateException("NaiveFox plugin supports arm64-v8a only");
        }
        if (pluginFiles.isEmpty()) {
            throw new IllegalStateException("NaiveFox plugin file index is unavailable");
        }
    }

    @Override
    public String getType(Uri uri) {
        return MIME_TYPE;
    }

    @Override
    public Cursor query(
            Uri uri,
            String[] projection,
            String selection,
            String[] selectionArgs,
            String sortOrder) {
        if (selection != null || selectionArgs != null || sortOrder != null) {
            throw new IllegalArgumentException("Selection and sorting are not supported");
        }
        assertReady();
        String[] columns = projection == null ? DEFAULT_PROJECTION : projection;
        MatrixCursor cursor = new MatrixCursor(columns);
        String basePath = normalizedUriPath(uri);
        for (PluginFile file : pluginFiles.values()) {
            if (file.providerPath.startsWith(basePath)) {
                cursor.newRow()
                        .add(COLUMN_PATH, file.providerPath)
                        .add(COLUMN_MODE, file.mode);
            }
        }
        return cursor;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new IllegalArgumentException("NaiveFox plugin files are read-only");
        }
        return openPluginFile(uri);
    }

    @Override
    public ParcelFileDescriptor openFile(
            Uri uri,
            String mode,
            CancellationSignal signal) throws FileNotFoundException {
        return openFile(uri, mode);
    }

    private ParcelFileDescriptor openPluginFile(Uri uri) throws FileNotFoundException {
        assertReady();
        String path = normalizedUriPath(uri);
        PluginFile file = pluginFiles.get(path);
        if (file == null) {
            throw new FileNotFoundException(path);
        }
        if (file.launcher) {
            File launcher = new File(providerContext().getApplicationInfo().nativeLibraryDir,
                    LAUNCHER_LIBRARY);
            if (!launcher.isFile()) {
                throw new FileNotFoundException(launcher.toString());
            }
            return ParcelFileDescriptor.open(launcher, ParcelFileDescriptor.MODE_READ_ONLY);
        }

        return openPipeHelper(uri, "application/octet-stream", null, file.assetPath,
                (output, ignoredUri, ignoredMimeType, ignoredOptions, assetPath) -> {
                    try (InputStream input = providerContext().getAssets().open(
                            assetPath, AssetManager.ACCESS_STREAMING);
                         FileOutputStream destination =
                                 new ParcelFileDescriptor.AutoCloseOutputStream(output)) {
                        byte[] buffer = new byte[64 * 1024];
                        int count;
                        while ((count = input.read(buffer)) != -1) {
                            destination.write(buffer, 0, count);
                        }
                    } catch (IOException exception) {
                        throw new IllegalStateException("Cannot stream plugin asset " + assetPath,
                                exception);
                    }
                });
    }

    private static String normalizedUriPath(Uri uri) {
        String path = uri.getPath();
        if (path == null || path.isEmpty() || "/".equals(path)) {
            return "";
        }
        String normalized = path.startsWith("/") ? path.substring(1) : path;
        return requireSafeRelativePath(normalized);
    }

    /** Force Exclave to continue from its executable fast path to the multi-file slow path. */
    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        if (METHOD_GET_EXECUTABLE.equals(method)) {
            return null;
        }
        return super.call(method, arg, extras);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
