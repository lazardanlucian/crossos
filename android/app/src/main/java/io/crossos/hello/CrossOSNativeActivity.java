package io.crossos.hello;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Context;
import android.os.Bundle;
import android.text.InputType;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

public final class CrossOSNativeActivity extends NativeActivity {
	private static native void nativeSetFilterText(String text);

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
}
