#include <crossos/dialog.h>

#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <stdio.h>
extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);
#endif

extern void crossos__set_error(const char *fmt, ...);

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>

static char *crossos__wchar_to_utf8(const wchar_t *ws)
{
    if (!ws)
    {
        return NULL;
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (len <= 0)
    {
        return NULL;
    }

    char *out = (char *)malloc((size_t)len);
    if (!out)
    {
        return NULL;
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, len, NULL, NULL))
    {
        free(out);
        return NULL;
    }

    return out;
}

static int crossos__append_item(crossos_dialog_file_list_t *list, const wchar_t *ws)
{
    char *utf8 = crossos__wchar_to_utf8(ws);
    if (!utf8)
    {
        return 0;
    }

    char **new_items = (char **)realloc(list->items, (list->count + 1) * sizeof(char *));
    if (!new_items)
    {
        free(utf8);
        return 0;
    }

    list->items = new_items;
    list->items[list->count] = utf8;
    list->count++;
    return 1;
}

static int crossos__copy_utf8_path(char *out_path, size_t out_path_size, const wchar_t *ws)
{
    char *utf8 = crossos__wchar_to_utf8(ws);
    if (!utf8)
    {
        return 0;
    }

    size_t len = strlen(utf8);
    if (len + 1 > out_path_size)
    {
        free(utf8);
        return 0;
    }

    memcpy(out_path, utf8, len + 1);
    free(utf8);
    return 1;
}

crossos_result_t crossos_dialog_pick_files(const char *title,
                                           int allow_multiple,
                                           crossos_dialog_file_list_t *out_files)
{
    if (!out_files)
    {
        crossos__set_error("dialog_pick_files: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    memset(out_files, 0, sizeof(*out_files));

    wchar_t title_w[256];
    if (title && title[0] != '\0')
    {
        MultiByteToWideChar(CP_UTF8, 0, title, -1, title_w, (int)(sizeof(title_w) / sizeof(title_w[0])));
    }
    else
    {
        title_w[0] = L'\0';
    }

    wchar_t buffer[8192];
    memset(buffer, 0, sizeof(buffer));

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));

    static const wchar_t raw_filter[] =
        L"RAW/JPEG Files (*.raf;*.raw;*.jpg;*.jpeg;*.ppm;*.pgm)\0"
        L"*.raf;*.RAF;*.raw;*.RAW;*.jpg;*.JPG;*.jpeg;*.JPEG;*.ppm;*.PPM;*.pgm;*.PGM\0"
        L"All Files (*.*)\0*.*\0\0";

    int raw_mode = (title && strstr(title, "RAW") != NULL);

    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrTitle = (title_w[0] != L'\0') ? title_w : NULL;
    if (raw_mode)
    {
        ofn.lpstrFilter = raw_filter;
    }
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (allow_multiple)
    {
        ofn.Flags |= OFN_ALLOWMULTISELECT;
    }

    if (!GetOpenFileNameW(&ofn))
    {
        DWORD err = CommDlgExtendedError();
        if (err == 0)
        {
            return CROSSOS_OK;
        }
        crossos__set_error("dialog_pick_files: GetOpenFileName failed (%lu)", (unsigned long)err);
        return CROSSOS_ERR_WINDOW;
    }

    const wchar_t *first = buffer;
    const wchar_t *second = first + wcslen(first) + 1;

    if (*second == L'\0')
    {
        if (!crossos__append_item(out_files, first))
        {
            crossos_dialog_file_list_free(out_files);
            crossos__set_error("dialog_pick_files: out of memory");
            return CROSSOS_ERR_OOM;
        }
        return CROSSOS_OK;
    }

    wchar_t full[8192];
    while (*second != L'\0')
    {
        swprintf(full, sizeof(full) / sizeof(full[0]), L"%ls\\%ls", first, second);
        full[(sizeof(full) / sizeof(full[0])) - 1] = L'\0';

        if (!crossos__append_item(out_files, full))
        {
            crossos_dialog_file_list_free(out_files);
            crossos__set_error("dialog_pick_files: out of memory");
            return CROSSOS_ERR_OOM;
        }

        second += wcslen(second) + 1;
    }

    return CROSSOS_OK;
}

crossos_result_t crossos_dialog_save_file(const char *title,
                                          const char *default_name,
                                          char *out_path,
                                          size_t out_path_size)
{
    if (!out_path || out_path_size == 0)
    {
        crossos__set_error("dialog_save_file: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    out_path[0] = '\0';

    wchar_t title_w[256];
    wchar_t default_w[512];
    if (title && title[0] != '\0')
        MultiByteToWideChar(CP_UTF8, 0, title, -1, title_w, (int)(sizeof(title_w) / sizeof(title_w[0])));
    else
        title_w[0] = L'\0';

    if (default_name && default_name[0] != '\0')
        MultiByteToWideChar(CP_UTF8, 0, default_name, -1, default_w, (int)(sizeof(default_w) / sizeof(default_w[0])));
    else
        default_w[0] = L'\0';

    wchar_t buffer[8192];
    memset(buffer, 0, sizeof(buffer));
    if (default_w[0] != L'\0')
    {
        wcsncpy(buffer, default_w, (sizeof(buffer) / sizeof(buffer[0])) - 1);
        buffer[(sizeof(buffer) / sizeof(buffer[0])) - 1] = L'\0';
    }

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));

    static const wchar_t save_filter[] =
        L"Image Files (*.png;*.jpg;*.jpeg;*.ppm)\0"
        L"*.png;*.PNG;*.jpg;*.JPG;*.jpeg;*.JPEG;*.ppm;*.PPM\0"
        L"All Files (*.*)\0*.*\0\0";

    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrTitle = (title_w[0] != L'\0') ? title_w : NULL;
    ofn.lpstrFilter = save_filter;
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"png";

    if (!GetSaveFileNameW(&ofn))
    {
        DWORD err = CommDlgExtendedError();
        if (err == 0)
            return CROSSOS_OK;
        crossos__set_error("dialog_save_file: GetSaveFileName failed (%lu)", (unsigned long)err);
        return CROSSOS_ERR_WINDOW;
    }

    if (!crossos__copy_utf8_path(out_path, out_path_size, buffer))
    {
        crossos__set_error("dialog_save_file: output path too long");
        return CROSSOS_ERR_OOM;
    }

    return CROSSOS_OK;
}

#else

#if defined(__linux__) && !defined(__ANDROID__)

static char *crossos__dup_trimmed_line(const char *line)
{
    if (!line)
    {
        return NULL;
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
    {
        len--;
    }

    char *out = (char *)malloc(len + 1);
    if (!out)
    {
        return NULL;
    }
    memcpy(out, line, len);
    out[len] = '\0';
    return out;
}

static int crossos__append_utf8_item(crossos_dialog_file_list_t *list, const char *text)
{
    char **new_items = (char **)realloc(list->items, (list->count + 1) * sizeof(char *));
    if (!new_items)
    {
        return 0;
    }
    list->items = new_items;
    list->items[list->count] = crossos__dup_trimmed_line(text);
    if (!list->items[list->count])
    {
        return 0;
    }
    list->count++;
    return 1;
}

static int crossos__command_exists(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    int rc = system(cmd);
    return rc == 0;
}

static int crossos__append_shell_quoted(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(dst);
    if (len + 2 >= dst_size)
        return 0;

    dst[len++] = '\'';
    dst[len] = '\0';
    while (src && *src)
    {
        if (*src == '\'')
        {
            static const char repl[] = "'\\''";
            size_t rlen = sizeof(repl) - 1;
            if (len + rlen >= dst_size)
                return 0;
            memcpy(dst + len, repl, rlen);
            len += rlen;
        }
        else
        {
            if (len + 1 >= dst_size)
                return 0;
            dst[len++] = *src;
        }
        src++;
    }

    if (len + 2 > dst_size)
        return 0;
    dst[len++] = '\'';
    dst[len] = '\0';
    return 1;
}

static crossos_result_t crossos__parse_pipe_output(char *line,
                                                   crossos_dialog_file_list_t *out_files)
{
    if (!line || line[0] == '\0')
    {
        return CROSSOS_OK;
    }

    char *cur = line;
    while (cur && *cur)
    {
        char *sep = strchr(cur, '|');
        if (sep)
        {
            *sep = '\0';
        }

        if (cur[0] != '\0')
        {
            if (!crossos__append_utf8_item(out_files, cur))
            {
                return CROSSOS_ERR_OOM;
            }
        }

        cur = sep ? (sep + 1) : NULL;
    }

    return CROSSOS_OK;
}

crossos_result_t crossos_dialog_pick_files(const char *title,
                                           int allow_multiple,
                                           crossos_dialog_file_list_t *out_files)
{
    if (!out_files)
    {
        crossos__set_error("dialog_pick_files: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    memset(out_files, 0, sizeof(*out_files));

    char cmd[2048];
    int use_zenity = crossos__command_exists("zenity");
    int use_kdialog = !use_zenity && crossos__command_exists("kdialog");
    int raw_mode = (title && strstr(title, "RAW") != NULL);

    if (!use_zenity && !use_kdialog)
    {
        crossos__set_error("dialog_pick_files: neither zenity nor kdialog is available");
        return CROSSOS_ERR_UNSUPPORT;
    }

    if (use_zenity)
    {
        if (raw_mode)
        {
            snprintf(cmd, sizeof(cmd),
                     "zenity --file-selection %s --separator='|' "
                     "--file-filter='RAW/JPEG | *.raf *.RAF *.raw *.RAW *.jpg *.JPG *.jpeg *.JPEG *.ppm *.PPM *.pgm *.PGM' "
                     "--file-filter='All files | *' %s 2>/dev/null",
                     allow_multiple ? "--multiple" : "",
                     (title && title[0]) ? "--title=\"CrossOS File Picker\"" : "");
        }
        else
        {
            snprintf(cmd, sizeof(cmd),
                     "zenity --file-selection %s --separator='|' %s 2>/dev/null",
                     allow_multiple ? "--multiple" : "",
                     (title && title[0]) ? "--title=\"CrossOS File Picker\"" : "");
        }
    }
    else
    {
        if (raw_mode)
        {
            snprintf(cmd, sizeof(cmd),
                     "kdialog --getopenfilename %s \"*.raf *.RAF *.raw *.RAW *.jpg *.JPG *.jpeg *.JPEG *.ppm *.PPM *.pgm *.PGM|RAW/JPEG files\" 2>/dev/null",
                     allow_multiple ? "--multiple --separate-output" : "");
        }
        else
        {
            snprintf(cmd, sizeof(cmd),
                     "kdialog --getopenfilename %s 2>/dev/null",
                     allow_multiple ? "--multiple --separate-output" : "");
        }
    }

    FILE *pipe = popen(cmd, "r");
    if (!pipe)
    {
        crossos__set_error("dialog_pick_files: failed to launch picker command");
        return CROSSOS_ERR_WINDOW;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), pipe))
    {
        pclose(pipe);
        return CROSSOS_OK;
    }

    crossos_result_t rc;
    if (use_zenity)
    {
        rc = crossos__parse_pipe_output(line, out_files);
    }
    else
    {
        rc = crossos__append_utf8_item(out_files, line) ? CROSSOS_OK : CROSSOS_ERR_OOM;
        if (allow_multiple && rc == CROSSOS_OK)
        {
            while (fgets(line, sizeof(line), pipe))
            {
                if (!crossos__append_utf8_item(out_files, line))
                {
                    rc = CROSSOS_ERR_OOM;
                    break;
                }
            }
        }
    }

    pclose(pipe);

    if (rc != CROSSOS_OK)
    {
        crossos_dialog_file_list_free(out_files);
        crossos__set_error("dialog_pick_files: out of memory");
        return rc;
    }

    return CROSSOS_OK;
}

crossos_result_t crossos_dialog_save_file(const char *title,
                                          const char *default_name,
                                          char *out_path,
                                          size_t out_path_size)
{
    if (!out_path || out_path_size == 0)
    {
        crossos__set_error("dialog_save_file: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    out_path[0] = '\0';

    char cmd[4096];
    int use_zenity = crossos__command_exists("zenity");
    int use_kdialog = !use_zenity && crossos__command_exists("kdialog");

    if (!use_zenity && !use_kdialog)
    {
        crossos__set_error("dialog_save_file: neither zenity nor kdialog is available");
        return CROSSOS_ERR_UNSUPPORT;
    }

    if (use_zenity)
    {
        snprintf(cmd, sizeof(cmd), "zenity --file-selection --save --confirm-overwrite");
        if (title && title[0])
        {
            strncat(cmd, " --title=", sizeof(cmd) - strlen(cmd) - 1);
            if (!crossos__append_shell_quoted(cmd, sizeof(cmd), title))
            {
                crossos__set_error("dialog_save_file: command too long");
                return CROSSOS_ERR_OOM;
            }
        }
        if (default_name && default_name[0])
        {
            strncat(cmd, " --filename=", sizeof(cmd) - strlen(cmd) - 1);
            if (!crossos__append_shell_quoted(cmd, sizeof(cmd), default_name))
            {
                crossos__set_error("dialog_save_file: command too long");
                return CROSSOS_ERR_OOM;
            }
        }
        strncat(cmd, " 2>/dev/null", sizeof(cmd) - strlen(cmd) - 1);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "kdialog --getsavefilename");
        if (default_name && default_name[0])
        {
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            if (!crossos__append_shell_quoted(cmd, sizeof(cmd), default_name))
            {
                crossos__set_error("dialog_save_file: command too long");
                return CROSSOS_ERR_OOM;
            }
        }
        strncat(cmd, " '*.png *.jpg *.jpeg *.ppm|Image files' 2>/dev/null", sizeof(cmd) - strlen(cmd) - 1);
    }

    FILE *pipe = popen(cmd, "r");
    if (!pipe)
    {
        crossos__set_error("dialog_save_file: failed to launch picker command");
        return CROSSOS_ERR_WINDOW;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), pipe))
    {
        pclose(pipe);
        return CROSSOS_OK;
    }

    pclose(pipe);

    char *trimmed = crossos__dup_trimmed_line(line);
    if (!trimmed)
    {
        crossos__set_error("dialog_save_file: out of memory");
        return CROSSOS_ERR_OOM;
    }

    size_t len = strlen(trimmed);
    if (len + 1 > out_path_size)
    {
        free(trimmed);
        crossos__set_error("dialog_save_file: output path too long");
        return CROSSOS_ERR_OOM;
    }

    memcpy(out_path, trimmed, len + 1);
    free(trimmed);
    return CROSSOS_OK;
}

#else

crossos_result_t crossos_dialog_pick_files(const char *title,
                                           int allow_multiple,
                                           crossos_dialog_file_list_t *out_files)
{
    (void)title;
    (void)allow_multiple;
    if (!out_files)
    {
        crossos__set_error("dialog_pick_files: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }
    memset(out_files, 0, sizeof(*out_files));
    crossos__set_error("dialog_pick_files: not implemented on this platform yet");
    return CROSSOS_ERR_UNSUPPORT;
}

crossos_result_t crossos_dialog_save_file(const char *title,
                                          const char *default_name,
                                          char *out_path,
                                          size_t out_path_size)
{
    (void)title;
    (void)default_name;
    if (!out_path || out_path_size == 0)
    {
        crossos__set_error("dialog_save_file: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }
    out_path[0] = '\0';
    crossos__set_error("dialog_save_file: not implemented on this platform yet");
    return CROSSOS_ERR_UNSUPPORT;
}

#endif

#endif

void crossos_dialog_file_list_free(crossos_dialog_file_list_t *files)
{
    if (!files)
    {
        return;
    }

    for (size_t i = 0; i < files->count; i++)
    {
        free(files->items[i]);
    }
    free(files->items);
    files->items = NULL;
    files->count = 0;
}
