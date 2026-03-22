#include <crossos/dialog.h>

#include <stdlib.h>
#include <string.h>

extern void crossos__set_error(const char *fmt, ...);

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>

static char *crossos__wchar_to_utf8(const wchar_t *ws)
{
    if (!ws) {
        return NULL;
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (len <= 0) {
        return NULL;
    }

    char *out = (char *)malloc((size_t)len);
    if (!out) {
        return NULL;
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, len, NULL, NULL)) {
        free(out);
        return NULL;
    }

    return out;
}

static int crossos__append_item(crossos_dialog_file_list_t *list, const wchar_t *ws)
{
    char *utf8 = crossos__wchar_to_utf8(ws);
    if (!utf8) {
        return 0;
    }

    char **new_items = (char **)realloc(list->items, (list->count + 1) * sizeof(char *));
    if (!new_items) {
        free(utf8);
        return 0;
    }

    list->items = new_items;
    list->items[list->count] = utf8;
    list->count++;
    return 1;
}

crossos_result_t crossos_dialog_pick_files(const char *title,
                                           int allow_multiple,
                                           crossos_dialog_file_list_t *out_files)
{
    if (!out_files) {
        crossos__set_error("dialog_pick_files: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    memset(out_files, 0, sizeof(*out_files));

    wchar_t title_w[256];
    if (title && title[0] != '\0') {
        MultiByteToWideChar(CP_UTF8, 0, title, -1, title_w, (int)(sizeof(title_w) / sizeof(title_w[0])));
    } else {
        title_w[0] = L'\0';
    }

    wchar_t buffer[8192];
    memset(buffer, 0, sizeof(buffer));

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrTitle = (title_w[0] != L'\0') ? title_w : NULL;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (allow_multiple) {
        ofn.Flags |= OFN_ALLOWMULTISELECT;
    }

    if (!GetOpenFileNameW(&ofn)) {
        DWORD err = CommDlgExtendedError();
        if (err == 0) {
            return CROSSOS_OK;
        }
        crossos__set_error("dialog_pick_files: GetOpenFileName failed (%lu)", (unsigned long)err);
        return CROSSOS_ERR_WINDOW;
    }

    const wchar_t *first = buffer;
    const wchar_t *second = first + wcslen(first) + 1;

    if (*second == L'\0') {
        if (!crossos__append_item(out_files, first)) {
            crossos_dialog_file_list_free(out_files);
            crossos__set_error("dialog_pick_files: out of memory");
            return CROSSOS_ERR_OOM;
        }
        return CROSSOS_OK;
    }

    wchar_t full[8192];
    while (*second != L'\0') {
        swprintf(full, sizeof(full) / sizeof(full[0]), L"%ls\\%ls", first, second);
        full[(sizeof(full) / sizeof(full[0])) - 1] = L'\0';

        if (!crossos__append_item(out_files, full)) {
            crossos_dialog_file_list_free(out_files);
            crossos__set_error("dialog_pick_files: out of memory");
            return CROSSOS_ERR_OOM;
        }

        second += wcslen(second) + 1;
    }

    return CROSSOS_OK;
}

#else

crossos_result_t crossos_dialog_pick_files(const char *title,
                                           int allow_multiple,
                                           crossos_dialog_file_list_t *out_files)
{
    (void)title;
    (void)allow_multiple;
    if (!out_files) {
        crossos__set_error("dialog_pick_files: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }
    memset(out_files, 0, sizeof(*out_files));
    crossos__set_error("dialog_pick_files: not implemented on this platform yet");
    return CROSSOS_ERR_UNSUPPORT;
}

#endif

void crossos_dialog_file_list_free(crossos_dialog_file_list_t *files)
{
    if (!files) {
        return;
    }

    for (size_t i = 0; i < files->count; i++) {
        free(files->items[i]);
    }
    free(files->items);
    files->items = NULL;
    files->count = 0;
}
