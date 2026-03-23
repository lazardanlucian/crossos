package io.crossos.hello;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public final class CrossOSNativeActivity extends NativeActivity {
	private static final int REQUEST_PICK_FILES = 42173;
	private static final int REQUEST_MANAGE_ALL_FILES = 42174;
	private static final int REQUEST_RUNTIME_STORAGE = 42175;
	private static final int MAX_PICK_RESULTS = 64;
	private static final int COPY_BUFFER_SIZE = 64 * 1024;
	private final Object pickedFilesLock = new Object();
	private String[] pendingPickedFiles = null;

	private static native void nativeSetFilterText(String text);

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		requestStorageAccessIfNeeded();
		CrossOSUsbBurner.requestAttachedDevicePermissions(this);
	}

	@Override
	protected void onResume() {
		super.onResume();
		CrossOSUsbBurner.requestAttachedDevicePermissions(this);
	}

	private void requestStorageAccessIfNeeded() {
		if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
			Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
					Uri.parse("package:" + getPackageName()));
			startActivityForResult(intent, REQUEST_MANAGE_ALL_FILES);
			return;
		}

		if (Build.VERSION.SDK_INT < 33) {
			if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
				requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE}, REQUEST_RUNTIME_STORAGE);
			}
		}
	}

	public void showFilterInput(final String initialText) {
		runOnUiThread(() -> {
			final EditText input = new EditText(this);
			input.setInputType(InputType.TYPE_CLASS_TEXT);
			input.setSingleLine(true);
			input.setText(initialText != null ? initialText : "");
			input.setSelection(input.getText().length());

			AlertDialog dialog = new AlertDialog.Builder(this)
					.setTitle("Filter files")
					.setView(input)
					.setPositiveButton("Apply", (d, which) -> nativeSetFilterText(input.getText().toString()))
					.setNegativeButton("Cancel", (d, which) -> nativeSetFilterText(null))
					.setOnCancelListener(d -> nativeSetFilterText(null))
					.create();

			dialog.setOnShowListener(d -> {
				input.requestFocus();
				InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
				if (imm != null) {
					imm.showSoftInput(input, InputMethodManager.SHOW_IMPLICIT);
				}
			});

			dialog.show();
		});
	}

	public void showFilePicker() {
		runOnUiThread(() -> {
			Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
			intent.addCategory(Intent.CATEGORY_OPENABLE);
			intent.setType("*/*");
			intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
			intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
			intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
			startActivityForResult(intent, REQUEST_PICK_FILES);
		});
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);

		if (requestCode == REQUEST_MANAGE_ALL_FILES) {
			if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
				requestStorageAccessIfNeeded();
			}
			return;
		}

		if (requestCode != REQUEST_PICK_FILES) {
			return;
		}

		if (resultCode != RESULT_OK || data == null) {
			synchronized (pickedFilesLock) {
				pendingPickedFiles = new String[0];
			}
			return;
		}

		final Uri single = data.getData();
		final android.content.ClipData clip = data.getClipData();

		new Thread(() -> {
			try {
				String[] picked = processPickedUris(single, clip);
				synchronized (pickedFilesLock) {
					pendingPickedFiles = picked;
				}
			} catch (Throwable ignored) {
				synchronized (pickedFilesLock) {
					pendingPickedFiles = new String[0];
				}
			}
		}, "crossos-picker-copy").start();
	}

	public String[] consumePickedFiles() {
		synchronized (pickedFilesLock) {
			String[] result = pendingPickedFiles;
			pendingPickedFiles = null;
			return result;
		}
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
		super.onRequestPermissionsResult(requestCode, permissions, grantResults);
		if (requestCode == REQUEST_RUNTIME_STORAGE) {
			for (int result : grantResults) {
				if (result != PackageManager.PERMISSION_GRANTED) {
					return;
				}
			}
		}
	}

	private String[] processPickedUris(Uri single, android.content.ClipData clipData) {
		final Set<String> uniqueUris = new LinkedHashSet<>();
		final List<String> pickedPaths = new ArrayList<>();

		if (single != null) {
			uniqueUris.add(single.toString());
		}

		if (clipData != null) {
			int count = Math.min(clipData.getItemCount(), MAX_PICK_RESULTS);
			for (int i = 0; i < count; i++) {
				Uri uri = clipData.getItemAt(i).getUri();
				if (uri != null) {
					uniqueUris.add(uri.toString());
				}
			}
		}

		for (String uriText : uniqueUris) {
			String copied = copyUriToAppCache(Uri.parse(uriText), pickedPaths.size());
			if (copied != null) {
				pickedPaths.add(copied);
			}
		}

		return pickedPaths.toArray(new String[0]);
	}

	private String copyUriToAppCache(Uri uri, int index) {
		if (uri == null) {
			return null;
		}

		try {
			int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION;
			getContentResolver().takePersistableUriPermission(uri, flags);
		} catch (SecurityException ignored) {
		}

		String name = queryDisplayName(uri);
		if (name == null || name.isEmpty()) {
			name = "picked_" + index;
		}
		name = sanitizeName(name);

		File dir = new File(getCacheDir(), "picked");
		if (!dir.exists() && !dir.mkdirs()) {
			return null;
		}

		File outFile = new File(dir, System.currentTimeMillis() + "_" + index + "_" + name);
		try (InputStream in = getContentResolver().openInputStream(uri);
			 FileOutputStream out = new FileOutputStream(outFile, false)) {
			if (in == null) {
				return null;
			}

			byte[] buffer = new byte[COPY_BUFFER_SIZE];
			int n;
			while ((n = in.read(buffer)) > 0) {
				out.write(buffer, 0, n);
			}
			out.flush();
			return outFile.getAbsolutePath();
		} catch (IOException e) {
			return null;
		}
	}

	private String queryDisplayName(Uri uri) {
		Cursor cursor = null;
		try {
			cursor = getContentResolver().query(uri, null, null, null, null);
			if (cursor != null && cursor.moveToFirst()) {
				int col = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
				if (col >= 0) {
					String value = cursor.getString(col);
					if (value != null && !value.isEmpty()) {
						return value;
					}
				}
			}
		} catch (Exception ignored) {
		} finally {
			if (cursor != null) {
				cursor.close();
			}
		}

		String segment = uri.getLastPathSegment();
		if (segment == null || segment.isEmpty()) {
			return null;
		}
		int slash = segment.lastIndexOf('/');
		if (slash >= 0 && slash + 1 < segment.length()) {
			return segment.substring(slash + 1);
		}
		return segment;
	}

	private static String sanitizeName(String name) {
		String clean = name.replaceAll("[^A-Za-z0-9._-]", "_");
		if (clean.isEmpty()) {
			return "picked_file";
		}
		return clean;
	}
}
