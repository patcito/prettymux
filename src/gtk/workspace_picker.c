#include "workspace_picker.h"

#include "app_state.h"
#include "ghostty_terminal.h"
#include "ui_language.h"
#include "workspace.h"

static const char *
current_workspace_cwd(void)
{
    Workspace *ws = workspace_get_current();
    GtkNotebook *notebook;
    GtkWidget *page;
    GtkWidget *terminal;
    int page_num;

    if (!ws)
        return NULL;

    notebook = workspace_get_focused_pane(ws);
    if (!GTK_IS_NOTEBOOK(notebook))
        return ws->cwd[0] ? ws->cwd : NULL;

    page_num = gtk_notebook_get_current_page(notebook);
    page = page_num >= 0
        ? gtk_notebook_get_nth_page(notebook, page_num)
        : NULL;
    terminal = page
        ? g_object_get_data(G_OBJECT(page), "linked-terminal")
        : NULL;

    if (GHOSTTY_IS_TERMINAL(terminal)) {
        const char *cwd =
            ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));
        if (cwd && cwd[0])
            return cwd;
    }

    return ws->cwd[0] ? ws->cwd : NULL;
}

static void
workspace_picker_response(GtkNativeDialog *dialog,
                          int response,
                          gpointer user_data)
{
    (void)user_data;

    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *folder =
            gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));

        if (folder) {
            g_autofree char *path = g_file_get_path(folder);

            if (path && path[0]) {
                workspace_add_with_cwd(ui.terminal_stack,
                                       ui.workspace_list,
                                       g_ghostty_app,
                                       path);
            }
            g_object_unref(folder);
        }
    }

    gtk_native_dialog_destroy(dialog);
    g_object_unref(dialog);
}

void
workspace_picker_show(GtkWindow *parent)
{
    GtkFileChooserNative *dialog;
    const char *cwd;

    dialog = gtk_file_chooser_native_new(
        ui_text(UI_TEXT_CHOOSE_PROJECT_FOLDER),
        parent,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        ui_text(UI_TEXT_CREATE_WORKSPACE),
        ui_text(UI_TEXT_CANCEL));

    cwd = current_workspace_cwd();
    if (cwd && cwd[0]) {
        g_autoptr(GFile) initial_folder = g_file_new_for_path(cwd);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                            initial_folder, NULL);
    }

    g_signal_connect(dialog, "response",
                     G_CALLBACK(workspace_picker_response), NULL);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}
