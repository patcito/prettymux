#include "ui_language.h"

#include "app_settings.h"

static const char *const language_codes[UI_LANGUAGE_COUNT] = {
    "zh", "en", "ko", "ja", "es",
};

static const char *const language_names[UI_LANGUAGE_COUNT] = {
    "中文", "English", "한국어", "日本語", "Español",
};

static const char *const text[UI_LANGUAGE_COUNT][UI_TEXT_COUNT] = {
    [UI_LANGUAGE_ZH] = {
        [UI_TEXT_LANGUAGE] = "语言",
        [UI_TEXT_NEW_PROJECT] = "新建项目…",
        [UI_TEXT_NEW_TAB] = "新建终端",
        [UI_TEXT_RIGHT_TERMINAL] = "右侧终端",
        [UI_TEXT_BELOW_TERMINAL] = "下方终端",
        [UI_TEXT_CLOSE_PANE] = "关闭窗格",
        [UI_TEXT_ZOOM_RESTORE] = "放大 / 还原",
        [UI_TEXT_ALL_ACTIONS] = "全部操作",
        [UI_TEXT_CHOOSE_PROJECT_FOLDER] = "选择项目文件夹",
        [UI_TEXT_CREATE_WORKSPACE] = "创建 Workspace",
        [UI_TEXT_CANCEL] = "取消",
        [UI_TEXT_NEW_WORKSPACE] = "+ 新建 Workspace",
        [UI_TEXT_SEARCH_WORKSPACES] = "搜索 Workspace",
        [UI_TEXT_CHAT_HISTORY] = "Claude / Codex 历史会话",
        [UI_TEXT_REFRESH_CHATS] = "刷新 Claude/Codex 历史",
        [UI_TEXT_RENAME_CHAT] = "重命名会话（仅在 PrettyMux 中显示）",
        [UI_TEXT_EMPTY_CHATS] = "暂无可恢复的 Claude/Codex 会话",
        [UI_TEXT_UNTITLED_CLAUDE] = "未命名 Claude 会话",
        [UI_TEXT_UNTITLED_CODEX] = "未命名 Codex 会话",
        [UI_TEXT_NOW] = "刚刚",
        [UI_TEXT_MINUTES_AGO] = "%" G_GINT64_FORMAT " 分钟前",
        [UI_TEXT_HOURS_AGO] = "%" G_GINT64_FORMAT " 小时前",
        [UI_TEXT_DAYS_AGO] = "%" G_GINT64_FORMAT " 天前",
        [UI_TEXT_CLOSE_TAB_HEADING] = "关闭这个标签？",
        [UI_TEXT_CLOSE_TAB_BODY] = "当前终端标签将被关闭。",
        [UI_TEXT_CLOSE_TAB_CONFIRM] = "关闭标签",
        [UI_TEXT_CLOSE_PANE_HEADING] = "关闭这个窗格？",
        [UI_TEXT_CLOSE_PANE_BODY] = "当前窗格及其终端标签将被关闭。",
        [UI_TEXT_CLOSE_PANE_CONFIRM] = "关闭窗格",
        [UI_TEXT_CLOSE_WORKSPACE_HEADING] = "关闭这个 Workspace？",
        [UI_TEXT_CLOSE_WORKSPACE_BODY] = "当前 Workspace 及其窗格将被关闭。",
        [UI_TEXT_CLOSE_WORKSPACE_CONFIRM] = "关闭 Workspace",
        [UI_TEXT_QUIT_HEADING] = "退出 PrettyMux？",
        [UI_TEXT_QUIT_BODY] = "PrettyMux 将关闭此窗口中的所有窗格和标签。",
        [UI_TEXT_QUIT_CONFIRM] = "退出",
        [UI_TEXT_CONFIRM_CLOSE] = "确认关闭",
        [UI_TEXT_CONFIRM] = "确认",
        [UI_TEXT_DONT_ASK_AGAIN] = "不再询问并记住此选择",
    },
    [UI_LANGUAGE_EN] = {
        [UI_TEXT_LANGUAGE] = "Language",
        [UI_TEXT_NEW_PROJECT] = "New Project…",
        [UI_TEXT_NEW_TAB] = "New Tab",
        [UI_TEXT_RIGHT_TERMINAL] = "Right Terminal",
        [UI_TEXT_BELOW_TERMINAL] = "Below Terminal",
        [UI_TEXT_CLOSE_PANE] = "Close Pane",
        [UI_TEXT_ZOOM_RESTORE] = "Zoom / Restore",
        [UI_TEXT_ALL_ACTIONS] = "All Actions",
        [UI_TEXT_CHOOSE_PROJECT_FOLDER] = "Choose a project folder",
        [UI_TEXT_CREATE_WORKSPACE] = "Create Workspace",
        [UI_TEXT_CANCEL] = "Cancel",
        [UI_TEXT_NEW_WORKSPACE] = "+ New Workspace",
        [UI_TEXT_SEARCH_WORKSPACES] = "Search workspaces",
        [UI_TEXT_CHAT_HISTORY] = "Claude / Codex Chats",
        [UI_TEXT_REFRESH_CHATS] = "Refresh Claude/Codex chats",
        [UI_TEXT_RENAME_CHAT] = "Rename chat (PrettyMux display only)",
        [UI_TEXT_EMPTY_CHATS] = "No resumable Claude/Codex chats",
        [UI_TEXT_UNTITLED_CLAUDE] = "Untitled Claude chat",
        [UI_TEXT_UNTITLED_CODEX] = "Untitled Codex chat",
        [UI_TEXT_NOW] = "now",
        [UI_TEXT_MINUTES_AGO] = "%" G_GINT64_FORMAT "m ago",
        [UI_TEXT_HOURS_AGO] = "%" G_GINT64_FORMAT "h ago",
        [UI_TEXT_DAYS_AGO] = "%" G_GINT64_FORMAT "d ago",
        [UI_TEXT_CLOSE_TAB_HEADING] = "Close this tab?",
        [UI_TEXT_CLOSE_TAB_BODY] = "The current tab will be closed.",
        [UI_TEXT_CLOSE_TAB_CONFIRM] = "Close Tab",
        [UI_TEXT_CLOSE_PANE_HEADING] = "Close this pane?",
        [UI_TEXT_CLOSE_PANE_BODY] = "The current pane and its tabs will be closed.",
        [UI_TEXT_CLOSE_PANE_CONFIRM] = "Close Pane",
        [UI_TEXT_CLOSE_WORKSPACE_HEADING] = "Close this workspace?",
        [UI_TEXT_CLOSE_WORKSPACE_BODY] = "The current workspace and its panes will be closed.",
        [UI_TEXT_CLOSE_WORKSPACE_CONFIRM] = "Close Workspace",
        [UI_TEXT_QUIT_HEADING] = "Quit PrettyMux?",
        [UI_TEXT_QUIT_BODY] = "PrettyMux will close all panes and tabs in this window.",
        [UI_TEXT_QUIT_CONFIRM] = "Quit",
        [UI_TEXT_CONFIRM_CLOSE] = "Confirm close",
        [UI_TEXT_CONFIRM] = "Confirm",
        [UI_TEXT_DONT_ASK_AGAIN] = "Don't ask again and remember this",
    },
    [UI_LANGUAGE_KO] = {
        [UI_TEXT_LANGUAGE] = "언어",
        [UI_TEXT_NEW_PROJECT] = "새 프로젝트…",
        [UI_TEXT_NEW_TAB] = "새 터미널",
        [UI_TEXT_RIGHT_TERMINAL] = "오른쪽 터미널",
        [UI_TEXT_BELOW_TERMINAL] = "아래쪽 터미널",
        [UI_TEXT_CLOSE_PANE] = "창 닫기",
        [UI_TEXT_ZOOM_RESTORE] = "확대 / 복원",
        [UI_TEXT_ALL_ACTIONS] = "모든 작업",
        [UI_TEXT_CHOOSE_PROJECT_FOLDER] = "프로젝트 폴더 선택",
        [UI_TEXT_CREATE_WORKSPACE] = "Workspace 만들기",
        [UI_TEXT_CANCEL] = "취소",
        [UI_TEXT_NEW_WORKSPACE] = "+ 새 Workspace",
        [UI_TEXT_SEARCH_WORKSPACES] = "Workspace 검색",
        [UI_TEXT_CHAT_HISTORY] = "Claude / Codex 채팅 기록",
        [UI_TEXT_REFRESH_CHATS] = "Claude/Codex 기록 새로 고침",
        [UI_TEXT_RENAME_CHAT] = "채팅 이름 바꾸기(PrettyMux에만 표시)",
        [UI_TEXT_EMPTY_CHATS] = "재개할 Claude/Codex 채팅이 없습니다",
        [UI_TEXT_UNTITLED_CLAUDE] = "이름 없는 Claude 채팅",
        [UI_TEXT_UNTITLED_CODEX] = "이름 없는 Codex 채팅",
        [UI_TEXT_NOW] = "방금",
        [UI_TEXT_MINUTES_AGO] = "%" G_GINT64_FORMAT "분 전",
        [UI_TEXT_HOURS_AGO] = "%" G_GINT64_FORMAT "시간 전",
        [UI_TEXT_DAYS_AGO] = "%" G_GINT64_FORMAT "일 전",
        [UI_TEXT_CLOSE_TAB_HEADING] = "이 탭을 닫을까요?",
        [UI_TEXT_CLOSE_TAB_BODY] = "현재 터미널 탭이 닫힙니다.",
        [UI_TEXT_CLOSE_TAB_CONFIRM] = "탭 닫기",
        [UI_TEXT_CLOSE_PANE_HEADING] = "이 창을 닫을까요?",
        [UI_TEXT_CLOSE_PANE_BODY] = "현재 창과 터미널 탭이 닫힙니다.",
        [UI_TEXT_CLOSE_PANE_CONFIRM] = "창 닫기",
        [UI_TEXT_CLOSE_WORKSPACE_HEADING] = "이 Workspace를 닫을까요?",
        [UI_TEXT_CLOSE_WORKSPACE_BODY] = "현재 Workspace와 창이 닫힙니다.",
        [UI_TEXT_CLOSE_WORKSPACE_CONFIRM] = "Workspace 닫기",
        [UI_TEXT_QUIT_HEADING] = "PrettyMux를 종료할까요?",
        [UI_TEXT_QUIT_BODY] = "이 창의 모든 창과 탭이 닫힙니다.",
        [UI_TEXT_QUIT_CONFIRM] = "종료",
        [UI_TEXT_CONFIRM_CLOSE] = "닫기 확인",
        [UI_TEXT_CONFIRM] = "확인",
        [UI_TEXT_DONT_ASK_AGAIN] = "다시 묻지 않고 이 선택 기억",
    },
    [UI_LANGUAGE_JA] = {
        [UI_TEXT_LANGUAGE] = "言語",
        [UI_TEXT_NEW_PROJECT] = "新規プロジェクト…",
        [UI_TEXT_NEW_TAB] = "新規ターミナル",
        [UI_TEXT_RIGHT_TERMINAL] = "右にターミナル",
        [UI_TEXT_BELOW_TERMINAL] = "下にターミナル",
        [UI_TEXT_CLOSE_PANE] = "ペインを閉じる",
        [UI_TEXT_ZOOM_RESTORE] = "拡大 / 元に戻す",
        [UI_TEXT_ALL_ACTIONS] = "すべての操作",
        [UI_TEXT_CHOOSE_PROJECT_FOLDER] = "プロジェクトフォルダーを選択",
        [UI_TEXT_CREATE_WORKSPACE] = "Workspace を作成",
        [UI_TEXT_CANCEL] = "キャンセル",
        [UI_TEXT_NEW_WORKSPACE] = "+ 新規 Workspace",
        [UI_TEXT_SEARCH_WORKSPACES] = "Workspace を検索",
        [UI_TEXT_CHAT_HISTORY] = "Claude / Codex チャット履歴",
        [UI_TEXT_REFRESH_CHATS] = "Claude/Codex 履歴を更新",
        [UI_TEXT_RENAME_CHAT] = "チャット名を変更（PrettyMux 内のみ）",
        [UI_TEXT_EMPTY_CHATS] = "再開できる Claude/Codex チャットはありません",
        [UI_TEXT_UNTITLED_CLAUDE] = "無題の Claude チャット",
        [UI_TEXT_UNTITLED_CODEX] = "無題の Codex チャット",
        [UI_TEXT_NOW] = "たった今",
        [UI_TEXT_MINUTES_AGO] = "%" G_GINT64_FORMAT "分前",
        [UI_TEXT_HOURS_AGO] = "%" G_GINT64_FORMAT "時間前",
        [UI_TEXT_DAYS_AGO] = "%" G_GINT64_FORMAT "日前",
        [UI_TEXT_CLOSE_TAB_HEADING] = "このタブを閉じますか？",
        [UI_TEXT_CLOSE_TAB_BODY] = "現在のターミナルタブを閉じます。",
        [UI_TEXT_CLOSE_TAB_CONFIRM] = "タブを閉じる",
        [UI_TEXT_CLOSE_PANE_HEADING] = "このペインを閉じますか？",
        [UI_TEXT_CLOSE_PANE_BODY] = "現在のペインとタブを閉じます。",
        [UI_TEXT_CLOSE_PANE_CONFIRM] = "ペインを閉じる",
        [UI_TEXT_CLOSE_WORKSPACE_HEADING] = "この Workspace を閉じますか？",
        [UI_TEXT_CLOSE_WORKSPACE_BODY] = "現在の Workspace とペインを閉じます。",
        [UI_TEXT_CLOSE_WORKSPACE_CONFIRM] = "Workspace を閉じる",
        [UI_TEXT_QUIT_HEADING] = "PrettyMux を終了しますか？",
        [UI_TEXT_QUIT_BODY] = "このウィンドウのすべてのペインとタブを閉じます。",
        [UI_TEXT_QUIT_CONFIRM] = "終了",
        [UI_TEXT_CONFIRM_CLOSE] = "終了の確認",
        [UI_TEXT_CONFIRM] = "確認",
        [UI_TEXT_DONT_ASK_AGAIN] = "今後確認せず、この選択を保存",
    },
    [UI_LANGUAGE_ES] = {
        [UI_TEXT_LANGUAGE] = "Idioma",
        [UI_TEXT_NEW_PROJECT] = "Nuevo proyecto…",
        [UI_TEXT_NEW_TAB] = "Nueva terminal",
        [UI_TEXT_RIGHT_TERMINAL] = "Terminal a la derecha",
        [UI_TEXT_BELOW_TERMINAL] = "Terminal abajo",
        [UI_TEXT_CLOSE_PANE] = "Cerrar panel",
        [UI_TEXT_ZOOM_RESTORE] = "Ampliar / Restaurar",
        [UI_TEXT_ALL_ACTIONS] = "Todas las acciones",
        [UI_TEXT_CHOOSE_PROJECT_FOLDER] = "Elegir carpeta del proyecto",
        [UI_TEXT_CREATE_WORKSPACE] = "Crear espacio",
        [UI_TEXT_CANCEL] = "Cancelar",
        [UI_TEXT_NEW_WORKSPACE] = "+ Nuevo espacio",
        [UI_TEXT_SEARCH_WORKSPACES] = "Buscar espacios de trabajo",
        [UI_TEXT_CHAT_HISTORY] = "Historial de Claude / Codex",
        [UI_TEXT_REFRESH_CHATS] = "Actualizar historial de Claude/Codex",
        [UI_TEXT_RENAME_CHAT] = "Renombrar chat (solo en PrettyMux)",
        [UI_TEXT_EMPTY_CHATS] = "No hay chats de Claude/Codex para reanudar",
        [UI_TEXT_UNTITLED_CLAUDE] = "Chat de Claude sin título",
        [UI_TEXT_UNTITLED_CODEX] = "Chat de Codex sin título",
        [UI_TEXT_NOW] = "ahora",
        [UI_TEXT_MINUTES_AGO] = "hace %" G_GINT64_FORMAT " min",
        [UI_TEXT_HOURS_AGO] = "hace %" G_GINT64_FORMAT " h",
        [UI_TEXT_DAYS_AGO] = "hace %" G_GINT64_FORMAT " d",
        [UI_TEXT_CLOSE_TAB_HEADING] = "¿Cerrar esta pestaña?",
        [UI_TEXT_CLOSE_TAB_BODY] = "Se cerrará la pestaña de terminal actual.",
        [UI_TEXT_CLOSE_TAB_CONFIRM] = "Cerrar pestaña",
        [UI_TEXT_CLOSE_PANE_HEADING] = "¿Cerrar este panel?",
        [UI_TEXT_CLOSE_PANE_BODY] = "Se cerrarán el panel actual y sus pestañas.",
        [UI_TEXT_CLOSE_PANE_CONFIRM] = "Cerrar panel",
        [UI_TEXT_CLOSE_WORKSPACE_HEADING] = "¿Cerrar este espacio de trabajo?",
        [UI_TEXT_CLOSE_WORKSPACE_BODY] = "Se cerrarán el espacio actual y sus paneles.",
        [UI_TEXT_CLOSE_WORKSPACE_CONFIRM] = "Cerrar espacio",
        [UI_TEXT_QUIT_HEADING] = "¿Salir de PrettyMux?",
        [UI_TEXT_QUIT_BODY] = "Se cerrarán todos los paneles y pestañas de esta ventana.",
        [UI_TEXT_QUIT_CONFIRM] = "Salir",
        [UI_TEXT_CONFIRM_CLOSE] = "Confirmar cierre",
        [UI_TEXT_CONFIRM] = "Confirmar",
        [UI_TEXT_DONT_ASK_AGAIN] = "No volver a preguntar y recordar esta opción",
    },
};

const char *
ui_language_code(UiLanguage language)
{
    return language >= 0 && language < UI_LANGUAGE_COUNT
        ? language_codes[language] : "en";
}

const char *
ui_language_display_name(UiLanguage language)
{
    return language >= 0 && language < UI_LANGUAGE_COUNT
        ? language_names[language] : language_names[UI_LANGUAGE_EN];
}

UiLanguage
ui_language_current(void)
{
    const char *code = app_settings_get_ui_language();

    for (int i = 0; i < UI_LANGUAGE_COUNT; i++) {
        if (g_strcmp0(code, language_codes[i]) == 0)
            return (UiLanguage)i;
    }
    return UI_LANGUAGE_EN;
}

void
ui_language_set(UiLanguage language)
{
    if (language < 0 || language >= UI_LANGUAGE_COUNT)
        language = UI_LANGUAGE_EN;
    app_settings_set_ui_language(language_codes[language]);
    app_settings_save();
}

const char *
ui_text(UiTextKey key)
{
    UiLanguage language = ui_language_current();

    if (key < 0 || key >= UI_TEXT_COUNT)
        return "";
    if (text[language][key])
        return text[language][key];
    return text[UI_LANGUAGE_EN][key] ? text[UI_LANGUAGE_EN][key] : "";
}
