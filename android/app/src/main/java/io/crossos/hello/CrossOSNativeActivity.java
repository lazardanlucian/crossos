package io.crossos.hello;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

public final class CrossOSNativeActivity extends NativeActivity {
	private static final int REQUEST_PICK_FILES = 42173;
	private static final int MAX_PICK_RESULTS = 64;
	private static final int COPY_BUFFER_SIZE = 64 * 1024;

	private static native void nativeSetFilterText(String text);
	private static native void nativeSetPickedFiles(String[] paths);

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
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

		if (requestCode != REQUEST_PICK_FILES) {
			return;
		}

		if (resultCode != RESULT_OK || data == null) {
			nativeSetPickedFiles(null);
			return;
		}

		final List<String> pickedPaths = new ArrayList<>();

		if (data.getData() != null) {
			String copied = copyUriToAppCache(data.getData(), pickedPaths.size());
			if (copied != null) {
				pickedPaths.add(copied);
			}
		}

		if (data.getClipData() != null) {
			int count = Math.min(data.getClipData().getItemCount(), MAX_PICK_RESULTS);
			for (int i = 0; i < count; i++) {
				Uri uri = data.getClipData().getItemAt(i).getUri();
				String copied = copyUriToAppCache(uri, pickedPaths.size());
				if (copied != null) {
					pickedPaths.add(copied);
				}
			}
		}

		nativeSetPickedFiles(pickedPaths.toArray(new String[0]));
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

		File outFile = new File(dir, index + "_" + name);
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
