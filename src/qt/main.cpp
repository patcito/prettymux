// PrettyMux — Qt6 + Chromium (QWebEngineView) + ghostty (OpenGL)
// Single window, native Wayland, GPU-accelerated terminal

#include <QApplication>
#include <QMainWindow>
#include <QSplitter>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QInputMethodEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QToolBar>
#include <QStackedWidget>
#include <QAbstractItemDelegate>
#include <QClipboard>
#include <QMimeData>
#include <QProcess>
#include <QInputDialog>
#include <QTabBar>
#include <QDir>
#include <QShortcut>
#include <QMenu>
#include <QWidgetAction>
#include <QPainter>
#include <QFrame>
#include <QCloseEvent>
#include <QJsonArray>
#include <QSystemTrayIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextEdit>
#include <QMap>
#include <QCompleter>
#include <QStringListModel>
#include <QDrag>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QScrollArea>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

extern "C" {
#include "ghostty.h"
}

// ── Ghostty callbacks ──

static ghostty_app_t g_app = nullptr;

static void wakeup_cb(void*) {}
// Defined after PrettyMuxWindow
static bool action_cb(ghostty_app_t, ghostty_target_s, ghostty_action_s);
static void close_surface_cb(void*, bool) {}
static bool read_clipboard_cb(void*, ghostty_clipboard_e, void*) { return false; }
static void confirm_read_clipboard_cb(void*, const char*, void*, ghostty_clipboard_request_e) {}
static void write_clipboard_cb(void*, ghostty_clipboard_e, const ghostty_clipboard_content_s*, size_t, bool) {}
// Forward declaration
class PrettyMuxWindow;
static PrettyMuxWindow* g_window = nullptr;

class GhosttyWidget; // forward declaration
class PaneWidget; // forward declaration
class PrettyMuxWindow; // forward declaration

// Forward declarations for broadcast (defined after PrettyMuxWindow)
static bool isBroadcastEnabled(GhosttyWidget* source);
static void doBroadcastKey(GhosttyWidget* source, ghostty_input_key_s ke);
static void doBroadcastText(GhosttyWidget* source, const char* text, size_t len);

// ── ShortcutManager — configurable keyboard shortcuts ──

class ShortcutManager {
public:
    struct Shortcut {
        QString action;
        QKeySequence sequence;
        QString label;
    };

    ShortcutManager() {
        defaults = {
            {"workspace.new", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), "New workspace"},
            {"workspace.close", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W), "Close workspace"},
            {"workspace.next", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight), "Next workspace"},
            {"workspace.prev", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft), "Previous workspace"},
            {"pane.tab.new", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), "New terminal tab"},
            {"browser.toggle", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B), "Toggle browser"},
            {"browser.new", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), "New browser tab"},
            {"devtools.docked", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), "Inspector docked"},
            {"devtools.window", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J), "Inspector window"},
            {"shortcuts.show", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), "Shortcuts overlay"},
            {"search.show", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), "Search palette"},
            {"pane.zoom", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), "Zoom pane"},
            {"terminal.search", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), "Terminal search"},
            {"broadcast.toggle", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return), "Broadcast mode"},
            {"notes.toggle", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q), "Quick notes"},
            {"theme.cycle", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Comma), "Cycle theme"},
            {"history.show", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H), "Command history"},
            {"pip.toggle", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M), "Picture in picture"},
        };
        for (auto& d : defaults)
            bindings[d.action] = d.sequence;
        loadFromFile();
    }

    QString configPath() {
        QString dir = QDir::homePath() + "/.config/prettymux";
        QDir().mkpath(dir);
        return dir + "/keybindings.json";
    }

    void loadFromFile() {
        QFile f(configPath());
        if (!f.open(QIODevice::ReadOnly)) return;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject()) return;
        QJsonObject obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            QKeySequence seq(it.value().toString());
            if (!seq.isEmpty())
                bindings[it.key()] = seq;
        }
    }

    void saveToFile() {
        QJsonObject obj;
        for (auto it = bindings.begin(); it != bindings.end(); ++it) {
            QKeySequence defaultSeq;
            for (auto& d : defaults) {
                if (d.action == it.key()) { defaultSeq = d.sequence; break; }
            }
            if (it.value() != defaultSeq)
                obj[it.key()] = it.value().toString();
        }
        QFile f(configPath());
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(obj).toJson());
            f.close();
        }
    }

    void resetToDefaults() {
        bindings.clear();
        for (auto& d : defaults)
            bindings[d.action] = d.sequence;
        QFile::remove(configPath());
    }

    QString matchShortcut(QKeyEvent* event) {
        QKeyCombination combo(event->modifiers(), static_cast<Qt::Key>(event->key()));
        QKeySequence pressed(combo);
        for (auto it = bindings.begin(); it != bindings.end(); ++it) {
            if (pressed.matches(it.value()) == QKeySequence::ExactMatch)
                return it.key();
        }
        return "";
    }

    bool shouldIntercept(QKeyEvent* event) {
        return !matchShortcut(event).isEmpty();
    }

    QKeySequence getBinding(const QString& action) {
        auto it = bindings.find(action);
        return it != bindings.end() ? it.value() : QKeySequence();
    }

    void setBinding(const QString& action, const QKeySequence& seq) {
        bindings[action] = seq;
    }

    const std::vector<Shortcut>& getDefaults() const { return defaults; }
    const QMap<QString, QKeySequence>& getBindings() const { return bindings; }

private:
    std::vector<Shortcut> defaults;
    QMap<QString, QKeySequence> bindings;
};

static ShortcutManager* g_shortcuts = nullptr;

// ── Theme System ──

struct Theme {
    QString name;
    QString bg, fg, surface, overlay, subtext, accent, green, red, yellow, blue, peach, muted, highlight;
};

static const Theme kThemes[] = {
    {"Dark",    "#1e1e2e", "#cdd6f4", "#181825", "#313244", "#6c7086", "#cba6f7",
                "#a6e3a1", "#f38ba8", "#f9e2af", "#89b4fa", "#fab387", "#45475a", "#585b70"},
    {"Light",   "#eff1f5", "#4c4f69", "#e6e9ef", "#ccd0da", "#8c8fa1", "#8839ef",
                "#40a02b", "#d20f39", "#df8e1d", "#1e66f5", "#fe640b", "#9ca0b0", "#acb0be"},
    {"Monokai", "#272822", "#f8f8f2", "#1e1f1c", "#3e3d32", "#75715e", "#ae81ff",
                "#a6e22e", "#f92672", "#e6db74", "#66d9ef", "#fd971f", "#49483e", "#75715e"},
};
static const int kThemeCount = 3;
static int g_currentTheme = 0;

static const Theme& currentTheme() { return kThemes[g_currentTheme]; }

// ── GhosttyWidget — QOpenGLWidget hosting a ghostty surface ──

class GhosttyWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

    ghostty_surface_t surface = nullptr;
    QTimer* tickTimer = nullptr;
    QByteArray m_textBuf;
    bool m_exitEmitted = false;
    QByteArray m_startCwd;

public:
    QString lastCwd;
    bool hasNewOutput = false;
    int progressPercent = -1;
    int progressState = -1;  // ghostty_action_progress_report_state_e or -1

    GhosttyWidget(const QString& startDir = "", QWidget* parent = nullptr) : QOpenGLWidget(parent) {
        if (!startDir.isEmpty())
            m_startCwd = startDir.toUtf8();
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_InputMethodEnabled, true);
        setMouseTracking(true);
    }

    bool event(QEvent* event) override {
        // Intercept Tab/Shift+Tab before Qt uses them for focus navigation
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            QKeyEvent* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
                if (event->type() == QEvent::KeyPress)
                    keyPressEvent(ke);
                else
                    keyReleaseEvent(ke);
                return true;
            }
        }
        return QOpenGLWidget::event(event);
    }

    ~GhosttyWidget() {
        if (surface) ghostty_surface_free(surface);
    }

    bool hasExited() const {
        return surface && ghostty_surface_process_exited(surface);
    }

    void* getSurface() const { return (void*)surface; }

signals:
    void processExited();

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        if (!g_app) return;

        ghostty_surface_config_s config = ghostty_surface_config_new();
        config.platform_tag = GHOSTTY_PLATFORM_LINUX;
        config.platform.gtk.gtk_widget = (void*)this;
        config.scale_factor = devicePixelRatioF();
        config.working_directory = m_startCwd.isEmpty() ? getenv("HOME") : m_startCwd.constData();

        // Pass env vars so child shells get prettymux integration silently
        static ghostty_env_var_s env_vars[3];
        static QByteArray socket_val = qgetenv("PRETTYMUX_SOCKET");
        static QByteArray bash_env_val = qgetenv("BASH_ENV");
        env_vars[0] = { "PRETTYMUX_SOCKET", socket_val.constData() };
        env_vars[1] = { "PRETTYMUX", "1" };
        env_vars[2] = { "BASH_ENV", bash_env_val.constData() };
        config.env_vars = env_vars;
        config.env_var_count = 3;

        surface = ghostty_surface_new(g_app, &config);
        if (surface) {
            ghostty_surface_init_opengl(surface);
            ghostty_surface_set_focus(surface, hasFocus());
        }

        tickTimer = new QTimer(this);
        connect(tickTimer, &QTimer::timeout, this, [this]() {
            if (g_app) ghostty_app_tick(g_app);
            update();
            if (hasExited() && !m_exitEmitted) {
                m_exitEmitted = true;
                emit processExited();
            }
        });
        tickTimer->start(16);
    }

    void resizeGL(int w, int h) override {
        if (!surface) return;
        float scale = devicePixelRatioF();
        ghostty_surface_set_content_scale(surface, scale, scale);
        ghostty_surface_set_size(surface, w * scale, h * scale);
    }

    void paintGL() override {
        if (!surface) return;
        ghostty_surface_draw_frame(surface);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!surface) return;

        // Intercept Alt+Arrow keys for pane focus navigation
        if (event->modifiers() == Qt::AltModifier) {
            switch (event->key()) {
                case Qt::Key_Left: case Qt::Key_Right:
                case Qt::Key_Up: case Qt::Key_Down:
                    QApplication::sendEvent(window(), event);
                    return;
                default: break;
            }
        }

        // Intercept shortcuts bound in ShortcutManager before ghostty consumes them
        if (g_shortcuts && g_shortcuts->shouldIntercept(event)) {
            QApplication::sendEvent(window(), event);
            return;
        }

        // Ctrl+Tab / Ctrl+Shift+Tab: pass to parent to cycle pane tabs
        if (event->key() == Qt::Key_Tab && (event->modifiers() & Qt::ControlModifier)) {
            event->ignore();
            return;
        }

        // Ctrl+Shift+C: copy selection
        if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_C) {
            ghostty_text_s text = {};
            if (ghostty_surface_has_selection(surface) &&
                ghostty_surface_read_selection(surface, &text)) {
                if (text.text && text.text_len > 0) {
                    QApplication::clipboard()->setText(
                        QString::fromUtf8(text.text, text.text_len));
                }
                ghostty_surface_free_text(surface, &text);
            }
            return;
        }

        // Ctrl+Shift+V or Ctrl+V: paste
        if ((event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_V) ||
            (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_V)) {
            QString text = QApplication::clipboard()->text();
            if (!text.isEmpty()) {
                QByteArray utf8 = text.toUtf8();
                ghostty_surface_text(surface, utf8.constData(), utf8.size());
                if (isBroadcastEnabled(this)) doBroadcastText(this, utf8.constData(), utf8.size());
            }
            return;
        }

        // Qt nativeScanCode() returns XKB keycodes on Linux
        // which is what ghostty expects
        uint32_t keycode = event->nativeScanCode();

        ghostty_input_key_s ke = {};
        ke.action = GHOSTTY_ACTION_PRESS;
        ke.keycode = keycode;
        ke.mods = translateMods(event->modifiers());
        ke.composing = false;

        // For plain keys, send text. For Ctrl combos, skip text but
        // always set unshifted_codepoint so ghostty can match bindings
        // like Ctrl+= -> increase_font_size.
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            QByteArray utf8 = event->text().toUtf8();
            if (!utf8.isEmpty() && utf8[0] >= 0x20) {
                m_textBuf = utf8;
                m_textBuf.append('\0');
                ke.text = m_textBuf.constData();
            }
        }

        // Always set unshifted_codepoint from the key
        uint32_t key = event->key();
        if (key >= 'A' && key <= 'Z') key = key - 'A' + 'a';
        ke.unshifted_codepoint = key < 0x110000 ? key : 0;

        ghostty_surface_key(surface, ke);
        if (isBroadcastEnabled(this)) doBroadcastKey(this, ke);
        update();
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (!surface) return;
        ghostty_input_key_s ke = {};
        ke.action = GHOSTTY_ACTION_RELEASE;
        ke.keycode = event->nativeScanCode();
        ke.mods = translateMods(event->modifiers());
        ghostty_surface_key(surface, ke);
        if (isBroadcastEnabled(this)) doBroadcastKey(this, ke);
    }

    void focusInEvent(QFocusEvent*) override {
        if (surface) ghostty_surface_set_focus(surface, true);
    }
    void focusOutEvent(QFocusEvent*) override {
        if (surface) ghostty_surface_set_focus(surface, false);
    }

    void inputMethodEvent(QInputMethodEvent* event) override {
        if (!surface) return;
        QByteArray utf8 = event->commitString().toUtf8();
        if (!utf8.isEmpty()) {
            ghostty_surface_text(surface, utf8.constData(), utf8.size());
            if (isBroadcastEnabled(this)) doBroadcastText(this, utf8.constData(), utf8.size());
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!surface) return;
        setFocus();
        ghostty_surface_mouse_pos(surface,
            event->position().x(),
            event->position().y(),
            translateMods(event->modifiers()));
        ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_PRESS,
            translateButton(event->button()), translateMods(event->modifiers()));
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!surface) return;
        ghostty_surface_mouse_pos(surface,
            event->position().x(),
            event->position().y(),
            translateMods(event->modifiers()));
        ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_RELEASE,
            translateButton(event->button()), translateMods(event->modifiers()));
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!surface) return;
        ghostty_surface_mouse_pos(surface,
            event->position().x(),
            event->position().y(),
            translateMods(event->modifiers()));
        update();
    }

    void wheelEvent(QWheelEvent* event) override {
        if (!surface) return;
        double dx = event->angleDelta().x() / 120.0;
        double dy = event->angleDelta().y() / 120.0;
        ghostty_surface_mouse_scroll(surface, dx, dy,
            translateMods(event->modifiers()));
        update();
    }

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override {
        if (query == Qt::ImEnabled) return true;
        return QOpenGLWidget::inputMethodQuery(query);
    }

private:
    static ghostty_input_mods_e translateMods(Qt::KeyboardModifiers mods) {
        int r = 0;
        if (mods & Qt::ShiftModifier)   r |= GHOSTTY_MODS_SHIFT;
        if (mods & Qt::ControlModifier) r |= GHOSTTY_MODS_CTRL;
        if (mods & Qt::AltModifier)     r |= GHOSTTY_MODS_ALT;
        if (mods & Qt::MetaModifier)    r |= GHOSTTY_MODS_SUPER;
        return (ghostty_input_mods_e)r;
    }

    static ghostty_input_mouse_button_e translateButton(Qt::MouseButton btn) {
        switch (btn) {
            case Qt::LeftButton:   return GHOSTTY_MOUSE_LEFT;
            case Qt::RightButton:  return GHOSTTY_MOUSE_RIGHT;
            case Qt::MiddleButton: return GHOSTTY_MOUSE_MIDDLE;
            default:               return GHOSTTY_MOUSE_LEFT;
        }
    }
};

// ── WebPage that opens target=_blank in new tabs ──

class PrettyMuxPage : public QWebEnginePage {
    Q_OBJECT
    std::function<void(const QUrl&)> m_openTab;
public:
    PrettyMuxPage(std::function<void(const QUrl&)> openTab, QObject* parent = nullptr)
        : QWebEnginePage(parent), m_openTab(openTab) {}

    bool acceptNavigationRequest(const QUrl& url, NavigationType type, bool isMainFrame) override {
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }

    QWebEnginePage* createWindow(WebWindowType) override {
        // Create a temporary page that captures the URL then opens it as a tab
        auto* tempPage = new QWebEnginePage(this);
        connect(tempPage, &QWebEnginePage::urlChanged, this, [this, tempPage](const QUrl& url) {
            if (!url.isEmpty() && url.toString() != "about:blank") {
                m_openTab(url);
                tempPage->deleteLater();
            }
        });
        return tempPage;
    }
};

// ── TabDragFilter — detects cross-pane tab drags ──

class PaneWidget; // forward
class TabDragFilter : public QObject {
public:
    PaneWidget* paneWidget;
    int dragTabIndex = -1;
    QPoint dragStartPos;

    TabDragFilter(PaneWidget* pw);
    bool eventFilter(QObject* obj, QEvent* event) override;
    void startCrossPaneDrag(QTabBar* tabBar);
};

// ── PaneWidget: a single pane with tab bar, each tab is one ghostty terminal ──
// Split buttons emit splitRequested so the parent can replace this pane
// with a splitter containing this pane + a new pane.

class PaneWidget : public QWidget {
    Q_OBJECT
public:
    QTabWidget* tabs;
    QFrame* progressBar = nullptr;
    std::vector<GhosttyWidget*> terminals; // one per tab

    PaneWidget(QWidget* parent = nullptr) : QWidget(parent) {
        const auto& t = currentTheme();
        setStyleSheet(QString("border: 2px solid %1; border-radius: 0px;").arg(t.overlay));
        setMinimumSize(100, 100);
        setAcceptDrops(true);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        tabs = new QTabWidget();
        tabs->setTabsClosable(true);
        tabs->setMovable(true);
        applyTabStyle();

        // Corner buttons
        QWidget* corner = new QWidget();
        auto* cl = new QHBoxLayout(corner);
        cl->setContentsMargins(2, 2, 2, 2);
        cl->setSpacing(2);

        auto btn = [&t](const QString& text) {
            auto* b = new QPushButton(text);
            b->setFixedHeight(20);
            b->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 3px; padding: 0 6px;").arg(t.overlay, t.subtext));
            return b;
        };

        auto* addBtn = btn("+");
        auto* splitH = btn("⬌");
        auto* splitV = btn("⬍");

        connect(addBtn, &QPushButton::clicked, this, [this]() { addNewTab(); });
        connect(splitH, &QPushButton::clicked, this, [this]() { emit splitRequested(this, Qt::Horizontal); });
        connect(splitV, &QPushButton::clicked, this, [this]() { emit splitRequested(this, Qt::Vertical); });

        cl->addWidget(addBtn);
        cl->addWidget(splitH);
        cl->addWidget(splitV);
        tabs->setCornerWidget(corner);

        // Close tab
        connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int idx) {
            if (terminals.size() <= 1) return;
            auto* term = terminals[idx];
            tabs->removeTab(idx);
            terminals.erase(terminals.begin() + idx);
            term->deleteLater();
        });

        // Double-click rename
        connect(tabs->tabBar(), &QTabBar::tabBarDoubleClicked, this, [this](int idx) {
            if (idx < 0 || idx >= (int)terminals.size()) return;
            bool ok;
            QString name = QInputDialog::getText(this, "Rename Tab", "Tab name:",
                QLineEdit::Normal, tabs->tabText(idx), &ok);
            if (ok && !name.isEmpty()) tabs->setTabText(idx, name);
        });

        layout->addWidget(tabs);

        // Thin progress bar at bottom of pane
        progressBar = new QFrame();
        progressBar->setFixedHeight(3);
        progressBar->setStyleSheet("background: transparent;");
        progressBar->hide();
        layout->addWidget(progressBar);

        // Clear activity indicator when switching tabs; update progress bar
        connect(tabs, &QTabWidget::currentChanged, this, [this](int idx) {
            if (idx >= 0 && idx < (int)terminals.size()) {
                terminals[idx]->hasNewOutput = false;
                tabs->tabBar()->setTabTextColor(idx, QColor());
                // Remove activity dot prefix
                QString text = tabs->tabText(idx);
                if (text.startsWith(QChar(0x25CF)))
                    tabs->setTabText(idx, text.mid(2));
                // Update progress bar for newly active tab
                updateTabProgress(idx, terminals[idx]->progressState, terminals[idx]->progressPercent);
            }
        });

        addNewTab();

        // Install shortcut for Ctrl+Tab / Ctrl+Shift+Tab
        auto* nextTab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), this);
        connect(nextTab, &QShortcut::activated, this, [this]() {
            if (tabs->count() > 1)
                tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
        });
        auto* prevTab = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this);
        connect(prevTab, &QShortcut::activated, this, [this]() {
            if (tabs->count() > 1)
                tabs->setCurrentIndex((tabs->currentIndex() - 1 + tabs->count()) % tabs->count());
        });

        // Install drag filter for cross-pane tab DnD
        tabs->tabBar()->installEventFilter(new TabDragFilter(this));

        // Sync terminals vector when tabs are reordered within pane
        connect(tabs->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
            if (from >= 0 && from < (int)terminals.size() && to >= 0 && to < (int)terminals.size()) {
                auto* term = terminals[from];
                terminals.erase(terminals.begin() + from);
                terminals.insert(terminals.begin() + to, term);
            }
        });
    }

    void applyTabStyle() {
        const auto& t = currentTheme();
        tabs->setStyleSheet(QString(
            "QTabWidget::pane { border: none; background: %1; }"
            "QTabBar::tab { background: %2; color: %3; padding: 4px 10px; border: none; border-bottom: 2px solid transparent; }"
            "QTabBar::tab:selected { color: %4; border-bottom: 2px solid %5; background: %1; }"
            "QTabBar::tab:hover { color: %4; background: %6; }"
        ).arg(t.bg, t.surface, t.subtext, t.fg, t.green, t.overlay));
        setStyleSheet(QString("border: 2px solid %1; border-radius: 0px;").arg(t.overlay));
    }

    void addNewTab(const QString& startDir = "") {
        auto* term = new GhosttyWidget(startDir);
        terminals.push_back(term);
        QString name = QString("shell %1").arg(terminals.size());
        tabs->addTab(term, name);
        tabs->setCurrentIndex(terminals.size() - 1);

        connect(term, &GhosttyWidget::processExited, this, [this, term]() {
            int idx = tabs->indexOf(term);
            if (idx < 0) return;
            auto it = std::find(terminals.begin(), terminals.end(), term);
            if (it != terminals.end()) terminals.erase(it);
            tabs->removeTab(idx);
            term->deleteLater();
            if (terminals.empty()) addNewTab();
        });

        term->setFocus();
    }

    std::vector<GhosttyWidget*> allTerminals() const { return terminals; }

    void markTabActivity(int tabIdx) {
        if (tabIdx < 0 || tabIdx >= tabs->count()) return;
        if (tabIdx == tabs->currentIndex()) return;
        tabs->tabBar()->setTabTextColor(tabIdx, QColor(currentTheme().green));
        QString text = tabs->tabText(tabIdx);
        if (!text.startsWith(QChar(0x25CF)))
            tabs->setTabText(tabIdx, QString("%1 %2").arg(QChar(0x25CF)).arg(text));
    }

    void updateTabProgress(int tabIdx, int state, int percent) {
        if (tabIdx < 0 || tabIdx >= tabs->count()) return;

        // Determine progress color for visual markers
        QString color;
        switch (state) {
            case GHOSTTY_PROGRESS_STATE_SET: color = "#a6e3a1"; break;
            case GHOSTTY_PROGRESS_STATE_ERROR: color = "#f38ba8"; break;
            case GHOSTTY_PROGRESS_STATE_PAUSE: color = "#f9e2af"; break;
            case GHOSTTY_PROGRESS_STATE_INDETERMINATE: color = "#89b4fa"; break;
            default: color = ""; break;
        }

        // Tooltip for any tab
        QString tooltip;
        if (state == GHOSTTY_PROGRESS_STATE_SET)
            tooltip = percent >= 0 ? QString("Progress: %1%").arg(percent) : "Progress";
        else if (state == GHOSTTY_PROGRESS_STATE_ERROR)
            tooltip = "Progress: Error";
        else if (state == GHOSTTY_PROGRESS_STATE_PAUSE)
            tooltip = "Progress: Paused";
        else if (state == GHOSTTY_PROGRESS_STATE_INDETERMINATE)
            tooltip = "Progress: Running...";

        if (!tooltip.isEmpty())
            tabs->setTabToolTip(tabIdx, tooltip);

        // Always-visible progress marker on tab label for ALL tabs
        // Strip any existing progress suffix (▰▱ bar or state markers)
        QString text = tabs->tabText(tabIdx);
        int markerPos = text.indexOf(QChar(0x2590)); // ▐ separator
        if (markerPos >= 0)
            text = text.left(markerPos).trimmed();

        if (state == GHOSTTY_PROGRESS_STATE_REMOVE || state < 0) {
            // Remove visual marker, reset tab color
            tabs->setTabText(tabIdx, text);
            tabs->tabBar()->setTabTextColor(tabIdx, QColor());
            tabs->setTabToolTip(tabIdx, "");
        } else {
            // Build a visible progress suffix on the tab label
            QString suffix;
            if (state == GHOSTTY_PROGRESS_STATE_SET && percent >= 0 && percent <= 100) {
                // Show mini progress bar: ▐▰▰▰▱▱▱ 42%
                int filled = percent / 20;  // 0-5 blocks
                int empty = 5 - filled;
                suffix = QString(" %1%2%3 %4%")
                    .arg(QChar(0x2590))  // ▐ separator
                    .arg(QString(filled, QChar(0x2588)))   // █ filled
                    .arg(QString(empty, QChar(0x2591)))     // ░ empty
                    .arg(percent);
            } else if (state == GHOSTTY_PROGRESS_STATE_ERROR) {
                suffix = QString(" %1 ✗").arg(QChar(0x2590));
            } else if (state == GHOSTTY_PROGRESS_STATE_PAUSE) {
                suffix = QString(" %1 ⏸").arg(QChar(0x2590));
            } else {
                // Indeterminate
                suffix = QString(" %1 ◌").arg(QChar(0x2590));
            }
            tabs->setTabText(tabIdx, text + suffix);
            tabs->tabBar()->setTabTextColor(tabIdx, QColor(color));
        }

        // Show thin progress bar at bottom of pane for the current tab only
        if (tabIdx != tabs->currentIndex()) return;

        if (state == GHOSTTY_PROGRESS_STATE_REMOVE || state < 0) {
            progressBar->hide();
            return;
        }
        progressBar->show();
        if (state == GHOSTTY_PROGRESS_STATE_SET && percent >= 0 && percent <= 100) {
            double frac = percent / 100.0;
            progressBar->setStyleSheet(QString(
                "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
                "stop:0 %1, stop:%2 %1, stop:%3 transparent, stop:1 transparent);"
            ).arg(color).arg(frac, 0, 'f', 3).arg(frac + 0.001, 0, 'f', 3));
        } else {
            progressBar->setStyleSheet(QString("background: %1;").arg(color));
        }
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat("application/x-prettymux-tab"))
            event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasFormat("application/x-prettymux-tab"))
            event->acceptProposedAction();
    }

    void dropEvent(QDropEvent* event) override {
        if (!event->mimeData()->hasFormat("application/x-prettymux-tab")) return;

        QByteArray data = event->mimeData()->data("application/x-prettymux-tab");
        QDataStream stream(&data, QIODevice::ReadOnly);
        quint64 sourcePtr;
        int tabIdx;
        stream >> sourcePtr >> tabIdx;

        PaneWidget* source = reinterpret_cast<PaneWidget*>(sourcePtr);
        if (!source || source == this) return;
        if (tabIdx < 0 || tabIdx >= (int)source->terminals.size()) return;
        if (source->terminals.size() <= 1) return; // don't remove last tab

        GhosttyWidget* term = source->terminals[tabIdx];
        QString tabName = source->tabs->tabText(tabIdx);

        source->tabs->removeTab(tabIdx);
        source->terminals.erase(source->terminals.begin() + tabIdx);

        terminals.push_back(term);
        tabs->addTab(term, tabName);
        tabs->setCurrentIndex(terminals.size() - 1);
        term->setFocus();

        event->acceptProposedAction();
    }

signals:
    void splitRequested(PaneWidget* source, Qt::Orientation orientation);
};

// ── TabDragFilter implementation ──

TabDragFilter::TabDragFilter(PaneWidget* pw) : QObject(pw), paneWidget(pw) {}

bool TabDragFilter::eventFilter(QObject* obj, QEvent* event) {
    auto* tabBar = qobject_cast<QTabBar*>(obj);
    if (!tabBar) return false;

    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            dragTabIndex = tabBar->tabAt(me->position().toPoint());
            dragStartPos = me->position().toPoint();
        }
    } else if (event->type() == QEvent::MouseMove) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (dragTabIndex >= 0 && (me->buttons() & Qt::LeftButton)) {
            if ((me->position().toPoint() - dragStartPos).manhattanLength() > QApplication::startDragDistance()) {
                int y = me->position().toPoint().y();
                if (y < -20 || y > tabBar->height() + 20) {
                    startCrossPaneDrag(tabBar);
                    return true;
                }
            }
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        dragTabIndex = -1;
    }
    return false;
}

void TabDragFilter::startCrossPaneDrag(QTabBar* tabBar) {
    if (dragTabIndex < 0 || dragTabIndex >= (int)paneWidget->terminals.size()) return;

    QDrag* drag = new QDrag(tabBar);
    QMimeData* mime = new QMimeData();
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream << (quint64)paneWidget << dragTabIndex;
    mime->setData("application/x-prettymux-tab", data);
    drag->setMimeData(mime);

    QPixmap pixmap(120, 30);
    pixmap.fill(QColor(currentTheme().overlay));
    QPainter p(&pixmap);
    p.setPen(QColor(currentTheme().fg));
    p.drawText(pixmap.rect(), Qt::AlignCenter, tabBar->tabText(dragTabIndex));
    p.end();
    drag->setPixmap(pixmap);

    drag->exec(Qt::MoveAction);
    dragTabIndex = -1;
}

// ── Workspace: vertical sidebar tab, contains panes arranged in splitters ──

struct Workspace {
    QString name;
    bool userRenamed = false;      // true if user manually renamed this workspace
    QWidget* container;            // top-level widget (either PaneWidget or QSplitter)
    std::vector<PaneWidget*> panes; // all panes (flat list for lookup)
    QString cwd;
    QString title;
    QString gitBranch;
    int gitDirtyCount = 0;
    int gitAhead = 0;
    int gitBehind = 0;
    QStringList listeningPorts;
    QString notes;
    QString notification;
    int newPort = 0;
    bool broadcast = false;
    bool zoomed = false;
    PaneWidget* zoomedPane = nullptr;
    std::vector<QList<int>> savedSplitterSizes;
};

// ── PiPWindow — floating picture-in-picture browser window ──

class PiPWindow : public QWidget {
    Q_OBJECT
public:
    QWebEngineView* view = nullptr;
    int originalTabIndex = -1;
    QTabWidget* originalTabs = nullptr;
    QWidget* originalContainer = nullptr;
    QPoint dragPos;
    bool dragging = false;
    int resizeEdge = 0; // 0=none, 1=left, 2=right, 4=top, 8=bottom (combinable)

    PiPWindow(QWidget* parent = nullptr) : QWidget(parent,
        Qt::Window | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setMinimumSize(320, 200);
        resize(560, 380);
        setMouseTracking(true);

        const auto& t = currentTheme();
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(4, 4, 4, 4);
        mainLayout->setSpacing(0);

        auto* frame = new QWidget();
        frame->setObjectName("pipFrame");
        frame->setStyleSheet(QString(
            "background: %1; border: 1px solid %2; border-radius: 8px;"
        ).arg(t.bg, t.muted));
        auto* frameLayout = new QVBoxLayout(frame);
        frameLayout->setContentsMargins(0, 0, 0, 0);
        frameLayout->setSpacing(0);

        // Title bar
        auto* titleBar = new QWidget();
        titleBar->setObjectName("pipTitleBar");
        titleBar->setFixedHeight(28);
        titleBar->setStyleSheet(QString(
            "background: %1; border-radius: 8px 8px 0 0; border: none;"
        ).arg(t.surface));
        auto* titleLayout = new QHBoxLayout(titleBar);
        titleLayout->setContentsMargins(10, 2, 4, 2);

        auto* titleLabel = new QLabel("Picture in Picture");
        titleLabel->setObjectName("pipTitle");
        titleLabel->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent; border: none;").arg(t.fg));
        titleLayout->addWidget(titleLabel);
        titleLayout->addStretch();

        auto* closeBtn = new QPushButton("x");
        closeBtn->setObjectName("pipCloseBtn");
        closeBtn->setFixedSize(20, 20);
        closeBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 3px;").arg(t.muted, t.fg));
        connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
        titleLayout->addWidget(closeBtn);

        frameLayout->addWidget(titleBar);
        frameLayout->addStretch(); // placeholder for view
        mainLayout->addWidget(frame);
    }

    void setView(QWebEngineView* v, int tabIdx, QTabWidget* tabs, QWidget* container) {
        view = v;
        originalTabIndex = tabIdx;
        originalTabs = tabs;
        originalContainer = container;

        auto* frame = findChild<QWidget*>()->layout();
        if (!frame) return;
        // Remove the stretch placeholder
        auto* item = frame->itemAt(1);
        if (item) {
            frame->removeItem(item);
            delete item;
        }
        frame->addWidget(v);

        auto* titleLabel = findChild<QLabel*>("pipTitle");
        if (titleLabel) {
            QString t = view->title().isEmpty() ? "Picture in Picture" : view->title().left(40);
            titleLabel->setText(t);
            connect(view, &QWebEngineView::titleChanged, titleLabel, [titleLabel](const QString& title) {
                titleLabel->setText(title.left(40));
            });
        }
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            resizeEdge = hitTestEdge(event->position().toPoint());
            if (resizeEdge) {
                dragPos = event->globalPosition().toPoint();
            } else if (event->position().y() < 32) {
                dragging = true;
                dragPos = event->globalPosition().toPoint() - frameGeometry().topLeft();
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging) {
            move(event->globalPosition().toPoint() - dragPos);
        } else if (resizeEdge) {
            QPoint delta = event->globalPosition().toPoint() - dragPos;
            dragPos = event->globalPosition().toPoint();
            QRect geo = geometry();
            if (resizeEdge & 2) geo.setRight(geo.right() + delta.x());
            if (resizeEdge & 1) geo.setLeft(geo.left() + delta.x());
            if (resizeEdge & 8) geo.setBottom(geo.bottom() + delta.y());
            if (resizeEdge & 4) geo.setTop(geo.top() + delta.y());
            if (geo.width() >= minimumWidth() && geo.height() >= minimumHeight())
                setGeometry(geo);
        } else {
            int edge = hitTestEdge(event->position().toPoint());
            if (edge == 1 || edge == 2) setCursor(Qt::SizeHorCursor);
            else if (edge == 4 || edge == 8) setCursor(Qt::SizeVerCursor);
            else if (edge == 5 || edge == 10) setCursor(Qt::SizeFDiagCursor);
            else if (edge == 6 || edge == 9) setCursor(Qt::SizeBDiagCursor);
            else setCursor(Qt::ArrowCursor);
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        dragging = false;
        resizeEdge = 0;
    }

    void closeEvent(QCloseEvent* event) override {
        emit restoreRequested();
        event->accept();
    }

signals:
    void restoreRequested();

private:
    int hitTestEdge(QPoint pos) {
        int margin = 6;
        int edge = 0;
        if (pos.x() < margin) edge |= 1;
        if (pos.x() > width() - margin) edge |= 2;
        if (pos.y() < margin) edge |= 4;
        if (pos.y() > height() - margin) edge |= 8;
        return edge;
    }
};

// ── MainWindow ──

struct SurfaceLocation { int workspaceIdx; PaneWidget* pane; int tabIdx; GhosttyWidget* term; };

class PrettyMuxWindow : public QMainWindow {
    Q_OBJECT

public:
    QString socketPath;
private:
    QListWidget* tabList;
    QStackedWidget* terminalStack;
    QTabWidget* browserTabs;
    QSplitter* mainSplitter;
    QWidget* sidebar = nullptr;
    QSplitter* outerSplit = nullptr;
    QSystemTrayIcon* trayIcon = nullptr;
    QLocalServer* socketServer = nullptr;
    struct NotificationEntry {
        QString title;
        QString body;
        int workspaceIdx;
        PaneWidget* pane;
        int tabIdx;
        bool read;
    };
    std::vector<NotificationEntry> notifications;
    QPushButton* bellBtn = nullptr;
    std::vector<Workspace> workspaces;
    int activeWorkspace = -1;
    QTextEdit* notesPanel = nullptr;
    QSplitter* terminalArea = nullptr;
    QTimer* portScanTimer = nullptr;
    QStringList lastKnownPorts;
    bool portScanEnabled = true;
    QStringList urlHistory;
    PiPWindow* pipWindow = nullptr;

public:
    PrettyMuxWindow() {
        setWindowTitle("prettymux");
        resize(1400, 900);

        // Central widget
        const auto& t = currentTheme();
        QWidget* central = new QWidget(this);
        outerSplit = new QSplitter(Qt::Horizontal, central);
        outerSplit->setStyleSheet(QString("QSplitter::handle { background: %1; width: 3px; }").arg(t.overlay));
        QHBoxLayout* mainLayout = new QHBoxLayout(central); mainLayout->setContentsMargins(0,0,0,0); mainLayout->setSpacing(0); mainLayout->addWidget(outerSplit);

        // ── Sidebar ──
        sidebar = new QWidget();
        sidebar->setMinimumWidth(120); sidebar->setMaximumWidth(400);
        sidebar->setStyleSheet(QString("background: %1; border-right: 1px solid %2;").arg(t.surface, t.overlay));

        QVBoxLayout* sidebarLayout = new QVBoxLayout(sidebar);
        sidebarLayout->setContentsMargins(0, 0, 0, 0);
        sidebarLayout->setSpacing(0);

        // Header
        QWidget* header = new QWidget();
        header->setObjectName("sidebarHeader");
        header->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2; padding: 8px;").arg(t.surface, t.overlay));
        QHBoxLayout* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(12, 8, 12, 8);

        QLabel* title = new QLabel("prettymux");
        title->setStyleSheet(QString("color: %1; font-weight: bold;").arg(t.accent));
        headerLayout->addWidget(title);
        headerLayout->addStretch();

        // Bell button with notification count badge
        bellBtn = new QPushButton();
        bellBtn->setFixedSize(28, 28);
        bellBtn->setToolTip("Notifications");
        bellBtn->setIcon(QIcon::fromTheme("notification-symbolic", QIcon::fromTheme("preferences-system-notifications")));
        bellBtn->setStyleSheet(QString("background: %1; border: none; border-radius: 4px;").arg(t.overlay));
        connect(bellBtn, &QPushButton::clicked, this, &PrettyMuxWindow::showNotificationDropdown);
        headerLayout->addWidget(bellBtn);

        QPushButton* addBtn = new QPushButton("+");
        addBtn->setFixedSize(24, 24);
        addBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 4px;").arg(t.overlay, t.fg));
        connect(addBtn, &QPushButton::clicked, this, &PrettyMuxWindow::addWorkspace);
        headerLayout->addWidget(addBtn);

        sidebarLayout->addWidget(header);

        // Search
        QLineEdit* searchBox = new QLineEdit();
        searchBox->setPlaceholderText("Search workspaces...");
        searchBox->setStyleSheet(QString(
            "background: %1; color: %2; border: none; border-radius: 4px;"
            "padding: 6px 10px; margin: 6px 8px; selection-background-color: %3;"
        ).arg(t.overlay, t.fg, t.highlight));
        connect(searchBox, &QLineEdit::textChanged, this, [this](const QString& text) {
            for (int i = 0; i < tabList->count(); i++) {
                bool match = text.isEmpty() || tabList->item(i)->text().contains(text, Qt::CaseInsensitive);
                tabList->item(i)->setHidden(!match);
            }
        });
        sidebarLayout->addWidget(searchBox);

        // Tab list
        tabList = new QListWidget();
        tabList->setWordWrap(true);
        tabList->setStyleSheet(QString(
            "QListWidget { background: %1; border: none; outline: none; }"
            "QListWidget::item { color: #a6adc8; padding: 10px 12px; border-left: 3px solid transparent; }"
            "QListWidget::item:selected { background: rgba(137,180,250,0.15); border-left: 3px solid %2; color: %3; }"
            "QListWidget::item:hover { background: %4; }"
        ).arg(t.surface, t.blue, t.fg, t.bg));
        tabList->setEditTriggers(QAbstractItemView::DoubleClicked);
        tabList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            auto* item = tabList->itemAt(pos);
            if (!item) return;
            int idx = tabList->row(item);
            const auto& tm = currentTheme();
            QMenu menu;
            menu.setStyleSheet(QString(
                "QMenu { background: %1; border: 1px solid %2; }"
                "QMenu::item { color: %3; padding: 6px 16px; }"
                "QMenu::item:selected { background: %2; }"
            ).arg(tm.bg, tm.overlay, tm.fg));
            menu.addAction("Rename", [this, idx]() {
                tabList->editItem(tabList->item(idx));
            });
            menu.addAction("Delete", [this, idx]() {
                if ((int)workspaces.size() <= 1) return; // keep at least one
                Workspace& ws = workspaces[idx];
                terminalStack->removeWidget(ws.container);
                for (auto* p : ws.panes) p->deleteLater();
                ws.container->deleteLater();
                workspaces.erase(workspaces.begin() + idx);
                delete tabList->takeItem(idx);
                if (activeWorkspace >= (int)workspaces.size())
                    activeWorkspace = workspaces.size() - 1;
                tabList->setCurrentRow(activeWorkspace);
            });
            menu.exec(tabList->mapToGlobal(pos));
        });
        connect(tabList, &QListWidget::currentRowChanged, this, &PrettyMuxWindow::switchWorkspace);
        connect(tabList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            int idx = tabList->row(item);
            if (idx >= 0 && idx < (int)workspaces.size() && workspaces[idx].newPort > 0) {
                // Check if click was on the notification area (bottom part of item)
                QRect itemRect = tabList->visualItemRect(item);
                QPoint clickPos = tabList->mapFromGlobal(QCursor::pos());
                // If click is in bottom third of item, open the port
                if (clickPos.y() > itemRect.top() + itemRect.height() * 2 / 3) {
                    addBrowserTab(QString("http://localhost:%1").arg(workspaces[idx].newPort));
                    browserTabs->setVisible(true);
                    workspaces[idx].newPort = 0;
                    workspaces[idx].notification.clear();
                    refreshSidebarItem(idx);
                }
            }
        });
        connect(tabList->itemDelegate(), &QAbstractItemDelegate::commitData, this, [this](QWidget*) {
            int row = tabList->currentRow();
            if (row >= 0 && row < (int)workspaces.size()) {
                workspaces[row].name = tabList->item(row)->text();
                workspaces[row].userRenamed = true;
            }
        });
        sidebarLayout->addWidget(tabList);

        // ── Toolbar ──
        QWidget* toolbar = new QWidget();
        toolbar->setObjectName("sidebarToolbar");
        toolbar->setStyleSheet(QString("background: %1; border-top: 1px solid %2;").arg(t.surface, t.overlay));
        QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbar);
        toolbarLayout->setContentsMargins(8, 4, 8, 4);

        // Split buttons removed from sidebar, they are now per-pane

        QPushButton* browserBtn = new QPushButton("Browser");
        browserBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 3px; padding: 4px 8px;").arg(t.overlay, t.fg));
        browserBtn->setCheckable(true);
        browserBtn->setChecked(true);
        connect(browserBtn, &QPushButton::toggled, this, &PrettyMuxWindow::toggleBrowser);
        toolbarLayout->addWidget(browserBtn);

        toolbarLayout->addStretch();
        sidebarLayout->addWidget(toolbar);

        outerSplit->addWidget(sidebar);

        // ── Main area: terminal stack + browser ──
        mainSplitter = new QSplitter(Qt::Horizontal);
        mainSplitter->setStyleSheet(QString("QSplitter::handle { background: %1; width: 4px; }").arg(t.overlay));

        terminalStack = new QStackedWidget();
        terminalStack->setStyleSheet(QString("background: %1;").arg(t.bg));

        // Terminal area: terminal stack + notes panel (vertical splitter)
        terminalArea = new QSplitter(Qt::Vertical);
        terminalArea->setStyleSheet(QString("QSplitter::handle { background: %1; height: 3px; }").arg(t.highlight));
        terminalArea->addWidget(terminalStack);

        notesPanel = new QTextEdit();
        notesPanel->setPlaceholderText("Quick notes for this workspace... (Ctrl+Shift+Q to toggle)");
        notesPanel->setStyleSheet(QString(
            "QTextEdit { background: %1; color: %2; border-top: 1px solid %3;"
            "padding: 12px; font-family: monospace; font-size: 13px; selection-background-color: %4; }"
        ).arg(t.bg, t.fg, t.overlay, t.highlight));
        notesPanel->setMinimumHeight(80);
        notesPanel->setMaximumHeight(300);
        notesPanel->hide();
        terminalArea->addWidget(notesPanel);

        // Sync notes to workspace on text change
        connect(notesPanel, &QTextEdit::textChanged, this, [this]() {
            if (activeWorkspace >= 0 && activeWorkspace < (int)workspaces.size())
                workspaces[activeWorkspace].notes = notesPanel->toPlainText();
        });

        mainSplitter->addWidget(terminalArea);

        // Browser tabs
        browserTabs = new QTabWidget();
        browserTabs->setTabsClosable(true);
        browserTabs->setMovable(true);
        browserTabs->setMinimumWidth(300);
        browserTabs->setStyleSheet(QString(
            "QTabWidget::pane { border: none; background: %1; }"
            "QTabBar::tab { background: %2; color: %3; padding: 6px 12px; border: none; border-bottom: 2px solid transparent; }"
            "QTabBar::tab:selected { color: %4; border-bottom: 2px solid %5; background: %1; }"
            "QTabBar::tab:hover { color: %4; background: %6; }"
            "QTabBar::close-button { image: none; }"
        ).arg(t.bg, t.surface, t.subtext, t.fg, t.accent, t.overlay));

        // New tab button
        QPushButton* newTabBtn = new QPushButton("+");
        newTabBtn->setFixedSize(24, 24);
        newTabBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 4px;").arg(t.overlay, t.fg));
        connect(newTabBtn, &QPushButton::clicked, this, [this]() { addBrowserTab("http://localhost:5173"); });
        browserTabs->setCornerWidget(newTabBtn);

        connect(browserTabs, &QTabWidget::tabCloseRequested, this, [this](int idx) {
            if (browserTabs->count() > 1) {
                QWidget* w = browserTabs->widget(idx);
                browserTabs->removeTab(idx);
                w->deleteLater();
            }
        });

        // Welcome tab
        addBrowserTab("file:///home/pe/newnewrepos/w/yo/prettymux/src/qt/welcome.html");

        mainSplitter->addWidget(browserTabs);

        mainSplitter->setSizes({700, 700});
        outerSplit->addWidget(mainSplitter); outerSplit->setSizes({200, 1200});

        setCentralWidget(central);

        // Theme stylesheet
        setStyleSheet(QString("QMainWindow { background: %1; } QToolTip { color: %2; background: %3; border: 1px solid %4; padding: 4px; }").arg(t.bg, t.fg, t.overlay, t.muted));

        // Connect resize overlay to main splitters
        connectSplitterOverlay(mainSplitter);
        connectSplitterOverlay(terminalArea);

        // Socket server for prettymux-open
        socketPath = QString("/tmp/prettymux-%1.sock").arg(QCoreApplication::applicationPid());
        QLocalServer::removeServer(socketPath);
        socketServer = new QLocalServer(this);
        socketServer->listen(socketPath);
        connect(socketServer, &QLocalServer::newConnection, this, [this]() {
            while (auto client = socketServer->nextPendingConnection()) {
                connect(client, &QLocalSocket::readyRead, this, [this, client]() {
                    QByteArray data = client->readAll();
                    QJsonDocument doc = QJsonDocument::fromJson(data);
                    if (doc.isObject()) {
                        auto obj = doc.object();
                        if (obj["command"].toString() == "browser.open") {
                            openUrlInBrowser(obj["url"].toString());
                        }
                    }
                    client->deleteLater();
                });
            }
        });

        // Set env vars for shell integration
        qputenv("PRETTYMUX_SOCKET", socketPath.toUtf8());
        qputenv("PRETTYMUX", "1");
        // BASH_ENV auto-sources our shell integration in every bash instance
        qputenv("BASH_ENV", "/home/pe/newnewrepos/w/yo/prettymux/src/qt/prettymux-shell-integration.sh");

        // Try to restore previous session, or create fresh workspace
        QFile sessionFile(sessionPath());
        if (sessionFile.exists()) {
            addWorkspace(); // need at least one for restore to work on
            restoreSession();
        } else {
            addWorkspace();
        }

        // Auto-save session every 30 seconds
        auto* autoSaveTimer = new QTimer(this);
        connect(autoSaveTimer, &QTimer::timeout, this, &PrettyMuxWindow::saveSession);
        autoSaveTimer->start(30000);

        // Port scanner (disable with PRETTYMUX_PORT_SCAN=0)
        QByteArray portScanEnv = qgetenv("PRETTYMUX_PORT_SCAN");
        portScanEnabled = (portScanEnv != "0");
        if (portScanEnabled) {
            portScanTimer = new QTimer(this);
            connect(portScanTimer, &QTimer::timeout, this, &PrettyMuxWindow::scanPorts);
            portScanTimer->start(5000);
        }
    }

    void connectPane(PaneWidget* pane) {
        connect(pane, &PaneWidget::splitRequested, this, &PrettyMuxWindow::handlePaneSplit);
    }

    void handlePaneSplit(PaneWidget* source, Qt::Orientation orientation) {
        if (activeWorkspace < 0 || activeWorkspace >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[activeWorkspace];

        QWidget* par = source->parentWidget();
        QSplitter* parentSplitter = qobject_cast<QSplitter*>(par);

        fprintf(stderr, "[split] orientation=%s source=%p size=%dx%d parent=%p parentSplitter=%p\n",
            orientation == Qt::Horizontal ? "H" : "V",
            (void*)source, source->width(), source->height(),
            (void*)par, (void*)parentSplitter);

        // Get source size before reparenting
        QSize sourceSize = source->size();

        auto* newSplitter = new QSplitter(orientation);
        newSplitter->setStyleSheet(QString("QSplitter::handle { background: %1; width: 3px; }").arg(currentTheme().highlight));
        connectSplitterOverlay(newSplitter);

        auto* newPane = new PaneWidget();
        connectPane(newPane);
        ws.panes.push_back(newPane);

        if (parentSplitter) {
            int idx = parentSplitter->indexOf(source);
            QList<int> parentSizes = parentSplitter->sizes();
            source->setParent(nullptr);
            newSplitter->addWidget(source);
            newSplitter->addWidget(newPane);
            parentSplitter->insertWidget(idx, newSplitter);
            parentSplitter->setSizes(parentSizes);
        } else {
            source->setParent(nullptr);
            newSplitter->addWidget(source);
            newSplitter->addWidget(newPane);
            newSplitter->resize(sourceSize);

            int stackIdx = terminalStack->indexOf(ws.container);
            terminalStack->removeWidget(ws.container);
            terminalStack->insertWidget(stackIdx, newSplitter);
            ws.container = newSplitter;
            terminalStack->setCurrentWidget(newSplitter);
        }

        // Equal stretch for both children
        newSplitter->setStretchFactor(0, 1);
        newSplitter->setStretchFactor(1, 1);

        // Force layout and equal sizes
        newSplitter->show();
        source->show();
        newPane->show();
        QApplication::processEvents();

        int total = (orientation == Qt::Horizontal) ? newSplitter->width() : newSplitter->height();
        int half = qMax(total / 2, 100);
        newSplitter->setSizes({half, half});

        newPane->allTerminals()[0]->setFocus();
    }

public slots:
    void addWorkspace() {
        Workspace ws;
        ws.name = QString("workspace %1").arg(workspaces.size() + 1);

        auto* pane = new PaneWidget();
        connectPane(pane);
        ws.panes.push_back(pane);
        ws.container = pane;

        terminalStack->addWidget(pane);
        workspaces.push_back(ws);

        QListWidgetItem* item = new QListWidgetItem(ws.name);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        tabList->addItem(item);
        tabList->setCurrentRow(workspaces.size() - 1);
    }

    void addTabInFocusedPane() {
        if (activeWorkspace < 0 || activeWorkspace >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[activeWorkspace];

        for (auto* pane : ws.panes) {
            if (pane->isAncestorOf(QApplication::focusWidget())) {
                pane->addNewTab();
                return;
            }
        }
        if (!ws.panes.empty()) ws.panes[0]->addNewTab();
    }

    void switchWorkspace(int index) {
        if (index < 0 || index >= (int)workspaces.size()) return;

        // Save notes from previous workspace
        if (activeWorkspace >= 0 && activeWorkspace < (int)workspaces.size() && notesPanel)
            workspaces[activeWorkspace].notes = notesPanel->toPlainText();

        activeWorkspace = index;
        terminalStack->setCurrentWidget(workspaces[index].container);

        Workspace& ws = workspaces[index];
        if (!ws.panes.empty()) {
            auto terms = ws.panes[0]->allTerminals();
            if (!terms.empty()) terms[0]->setFocus();
        }

        // Load notes for new workspace
        if (notesPanel && notesPanel->isVisible())
            notesPanel->setPlainText(ws.notes);

        ws.notification.clear();
        refreshSidebarItem(index);
        tabList->item(index)->setForeground(QColor("#a6adc8"));
    }

    void addBrowserTab(const QString& url) {
        const auto& t = currentTheme();
        // Container: address bar + webview
        QWidget* container = new QWidget();
        QVBoxLayout* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // Address bar
        QWidget* addressBar = new QWidget();
        addressBar->setFixedHeight(36);
        addressBar->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;").arg(t.surface, t.overlay));
        QHBoxLayout* barLayout = new QHBoxLayout(addressBar);
        barLayout->setContentsMargins(4, 4, 4, 4);
        barLayout->setSpacing(4);

        QString btnStyle = QString("background: %1; color: %2; border: none; border-radius: 4px;").arg(t.overlay, t.fg);
        QPushButton* backBtn = new QPushButton("<");
        backBtn->setFixedSize(28, 28);
        backBtn->setStyleSheet(btnStyle);

        QPushButton* fwdBtn = new QPushButton(">");
        fwdBtn->setFixedSize(28, 28);
        fwdBtn->setStyleSheet(btnStyle);

        QPushButton* reloadBtn = new QPushButton("R");
        reloadBtn->setFixedSize(28, 28);
        reloadBtn->setStyleSheet(btnStyle);

        QLineEdit* urlBar = new QLineEdit(url);
        urlBar->setStyleSheet(QString(
            "background: %1; color: %2; border: none; border-radius: 4px;"
            "padding: 4px 8px; selection-background-color: %3;"
        ).arg(t.overlay, t.fg, t.highlight));

        // URL autocomplete from history
        auto* completerModel = new QStringListModel(urlHistory, urlBar);
        auto* completer = new QCompleter(completerModel, urlBar);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        completer->setMaxVisibleItems(8);
        // Update model when history changes
        connect(urlBar, &QLineEdit::returnPressed, urlBar, [completerModel, this]() {
            completerModel->setStringList(urlHistory);
        });
        completer->popup()->setStyleSheet(QString(
            "QListView { background: %1; color: %2; border: 1px solid %3; padding: 2px; }"
            "QListView::item { padding: 4px 8px; }"
            "QListView::item:selected { background: %4; }"
        ).arg(t.surface, t.fg, t.overlay, t.overlay));
        urlBar->setCompleter(completer);

        barLayout->addWidget(backBtn);
        barLayout->addWidget(fwdBtn);
        barLayout->addWidget(reloadBtn);
        barLayout->addWidget(urlBar);

        layout->addWidget(addressBar);

        // WebView with custom page for target=_blank handling
        QWebEngineView* view = new QWebEngineView();
        auto* page = new PrettyMuxPage([this](const QUrl& url) {
            addBrowserTab(url.toString());
        }, view);
        view->setPage(page);

        // Enable developer tools and right-click inspect
        view->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        view->setContextMenuPolicy(Qt::DefaultContextMenu);
        page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);

        view->setUrl(QUrl(url));
        layout->addWidget(view);

        // Wire up buttons
        connect(backBtn, &QPushButton::clicked, view, &QWebEngineView::back);
        connect(fwdBtn, &QPushButton::clicked, view, &QWebEngineView::forward);
        connect(reloadBtn, &QPushButton::clicked, view, &QWebEngineView::reload);

        // Enter in URL bar navigates and records history
        connect(urlBar, &QLineEdit::returnPressed, this, [this, view, urlBar]() {
            QString text = urlBar->text().trimmed();
            if (!text.startsWith("http://") && !text.startsWith("https://") && !text.startsWith("file://")) {
                if (text.contains(".") && !text.contains(" "))
                    text = "https://" + text;
                else
                    text = "https://www.google.com/search?q=" + QUrl::toPercentEncoding(text);
            }
            addUrlToHistory(text);
            view->setUrl(QUrl(text));
        });

        // Update URL bar when page navigates and record history
        connect(view, &QWebEngineView::urlChanged, this, [this, urlBar](const QUrl& newUrl) {
            urlBar->setText(newUrl.toString());
            addUrlToHistory(newUrl.toString());
        });

        // Update tab title
        connect(view, &QWebEngineView::titleChanged, this, [this, container](const QString& title) {
            int idx = browserTabs->indexOf(container);
            if (idx >= 0) browserTabs->setTabText(idx, title.left(20));
        });

        browserTabs->addTab(container, "Loading...");
        browserTabs->setCurrentWidget(container);
    }

    // Terminal exit is now handled by PaneWidget internally

    // Update pane tab text for a surface
    void updatePaneTabText(SurfaceLocation& loc, const QString& text) {
        if (!loc.pane || loc.tabIdx < 0 || loc.tabIdx >= loc.pane->tabs->count()) return;
        loc.pane->tabs->setTabText(loc.tabIdx, text.left(30));
    }

    Q_INVOKABLE void updateWorkspaceTitle(quint64 surfacePtr, const QString& title) {
        auto loc = findSurfaceLocation((void*)surfacePtr);
        if (loc.workspaceIdx < 0) return;

        workspaces[loc.workspaceIdx].title = title;
        refreshSidebarItem(loc.workspaceIdx);

        // Update the pane tab: if title looks like a path, show only last component
        QString shortTitle = title;
        if (shortTitle.contains('/')) {
            int lastSlash = shortTitle.lastIndexOf('/');
            if (lastSlash >= 0 && lastSlash < shortTitle.length() - 1)
                shortTitle = shortTitle.mid(lastSlash + 1);
            if (shortTitle.isEmpty()) shortTitle = "/";
            // Full path as tooltip
            if (loc.pane && loc.tabIdx >= 0 && loc.tabIdx < loc.pane->tabs->count())
                loc.pane->tabs->setTabToolTip(loc.tabIdx, title);
        }
        updatePaneTabText(loc, shortTitle);
    }

    Q_INVOKABLE void updateWorkspaceCwd(quint64 surfacePtr, const QString& cwd) {
        auto loc = findSurfaceLocation((void*)surfacePtr);
        if (loc.workspaceIdx < 0) return;
        Workspace& ws = workspaces[loc.workspaceIdx];
        ws.cwd = cwd;

        // Store CWD on the terminal widget for session restore
        if (loc.term) loc.term->lastCwd = cwd;

        // Detect git branch + status
        QProcess git;
        git.setWorkingDirectory(cwd);
        git.start("git", {"rev-parse", "--abbrev-ref", "HEAD"});
        git.waitForFinished(500);
        if (git.exitCode() == 0) {
            ws.gitBranch = git.readAllStandardOutput().trimmed();

            // Dirty file count
            QProcess gitStatus;
            gitStatus.setWorkingDirectory(cwd);
            gitStatus.start("git", {"status", "--porcelain"});
            gitStatus.waitForFinished(500);
            if (gitStatus.exitCode() == 0) {
                QByteArray output = gitStatus.readAllStandardOutput();
                ws.gitDirtyCount = output.isEmpty() ? 0 : output.split('\n').count() - (output.endsWith('\n') ? 1 : 0);
            } else {
                ws.gitDirtyCount = 0;
            }

            // Ahead/behind upstream
            QProcess gitRevList;
            gitRevList.setWorkingDirectory(cwd);
            gitRevList.start("git", {"rev-list", "--left-right", "--count", "HEAD...@{upstream}"});
            gitRevList.waitForFinished(500);
            if (gitRevList.exitCode() == 0) {
                QString ab = gitRevList.readAllStandardOutput().trimmed();
                QStringList parts = ab.split('\t');
                ws.gitAhead = parts.size() > 0 ? parts[0].toInt() : 0;
                ws.gitBehind = parts.size() > 1 ? parts[1].toInt() : 0;
            } else {
                ws.gitAhead = 0;
                ws.gitBehind = 0;
            }
        } else {
            ws.gitBranch.clear();
            ws.gitDirtyCount = 0;
            ws.gitAhead = 0;
            ws.gitBehind = 0;
        }

        refreshSidebarItem(loc.workspaceIdx);

        // Update pane tab: short name + full path tooltip
        QString shortCwd = cwd;
        int lastSlash = shortCwd.lastIndexOf('/');
        if (lastSlash >= 0 && lastSlash < shortCwd.length() - 1)
            shortCwd = shortCwd.mid(lastSlash + 1);
        if (shortCwd.isEmpty()) shortCwd = "/";
        updatePaneTabText(loc, shortCwd);

        // Set tooltip to full path
        if (loc.pane && loc.tabIdx >= 0 && loc.tabIdx < loc.pane->tabs->count())
            loc.pane->tabs->setTabToolTip(loc.tabIdx, cwd);
    }

    void refreshSidebarItem(int idx) {
        if (idx < 0 || idx >= tabList->count()) return;
        if (idx >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[idx];

        // Build rich display text
        // First line: user's custom name if renamed, otherwise auto title
        QStringList lines;
        if (ws.userRenamed) {
            lines << ws.name;
        } else if (!ws.title.isEmpty()) {
            // Shorten path-like titles
            QString t = ws.title;
            if (t.contains('/')) {
                int ls = t.lastIndexOf('/');
                if (ls >= 0 && ls < t.length() - 1) t = t.mid(ls + 1);
            }
            lines << t;
        } else {
            lines << ws.name;
        }
        if (!ws.gitBranch.isEmpty()) {
            QString gitLine = QString("⎇ %1").arg(ws.gitBranch);
            if (ws.gitDirtyCount > 0)
                gitLine += QString(" *%1").arg(ws.gitDirtyCount);
            if (ws.gitAhead > 0)
                gitLine += QString(" ↑%1").arg(ws.gitAhead);
            if (ws.gitBehind > 0)
                gitLine += QString(" ↓%1").arg(ws.gitBehind);
            lines << gitLine;
        }
        if (!ws.cwd.isEmpty()) {
            QString shortCwd = ws.cwd;
            QString home = QDir::homePath();
            if (shortCwd.startsWith(home))
                shortCwd = "~" + shortCwd.mid(home.length());
            lines << shortCwd;
        }
        // listeningPorts line removed, "New port:" notification is enough
        if (!ws.notification.isEmpty()) {
            lines << ws.notification;
        }
        if (ws.broadcast) {
            lines << "BROADCAST";
        }
        if (ws.zoomed) {
            lines << "ZOOMED";
        }

        tabList->item(idx)->setText(lines.join("\n"));
        if (ws.newPort > 0)
            tabList->item(idx)->setToolTip(QString("Click to open http://localhost:%1 in browser").arg(ws.newPort));
        else
            tabList->item(idx)->setToolTip("");

        // Color sidebar item based on state
        if (ws.broadcast)
            tabList->item(idx)->setForeground(QColor("#f38ba8"));
        else if (!ws.gitBranch.isEmpty() && ws.gitDirtyCount > 0)
            tabList->item(idx)->setForeground(QColor("#f9e2af"));
        else if (!ws.gitBranch.isEmpty() && ws.gitDirtyCount == 0)
            tabList->item(idx)->setForeground(QColor("#a6e3a1"));
        else
            tabList->item(idx)->setForeground(QColor("#a6adc8"));
    }

    // Find workspace index that contains a given GhosttyWidget pointer
    int findWorkspaceForSurface(void* surfacePtr) {
        for (int wi = 0; wi < (int)workspaces.size(); wi++) {
            for (auto* pane : workspaces[wi].panes) {
                for (auto* term : pane->allTerminals()) {
                    if (term->getSurface() == surfacePtr) return wi;
                }
            }
        }
        return activeWorkspace;
    }

    void updateBellBadge() {
        int unread = 0;
        for (auto& n : notifications) if (!n.read) unread++;

        // Draw icon with badge overlay
        QPixmap pixmap(28, 28);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);

        // Draw bell from theme icon
        QIcon bellIcon = QIcon::fromTheme("notification-symbolic",
                         QIcon::fromTheme("preferences-system-notifications"));
        bellIcon.paint(&painter, 2, 2, 24, 24);

        if (unread > 0) {
            // Red badge circle
            painter.setBrush(QColor("#f38ba8"));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(16, 0, 12, 12);
            // Count text
            painter.setPen(QColor("#1e1e2e"));
            QFont f = painter.font();
            f.setPixelSize(8);
            f.setBold(true);
            painter.setFont(f);
            painter.drawText(QRect(16, 0, 12, 12), Qt::AlignCenter, QString::number(unread));
        }
        painter.end();

        bellBtn->setIcon(QIcon(pixmap));
        bellBtn->setIconSize(QSize(28, 28));
    }

    void addNotificationEntry(const QString& title, const QString& body, SurfaceLocation& loc) {
        notifications.push_back({title, body, loc.workspaceIdx, loc.pane, loc.tabIdx, false});
        // Cap at 50
        if (notifications.size() > 50)
            notifications.erase(notifications.begin());
        updateBellBadge();
    }

    void showNotificationDropdown() {
        const auto& t = currentTheme();
        QMenu* menu = new QMenu(this);
        menu->setStyleSheet(QString(
            "QMenu { background: %1; border: 1px solid %2; padding: 4px; min-width: 300px; }"
            "QMenu::item { color: %3; padding: 8px 12px; }"
            "QMenu::item:selected { background: %2; }"
            "QMenu::item:disabled { color: %4; }"
            "QMenu::separator { background: %2; height: 1px; margin: 4px 8px; }"
        ).arg(t.bg, t.overlay, t.fg, t.muted));

        if (notifications.empty()) {
            menu->addAction("No notifications")->setEnabled(false);
        } else {
            for (int i = (int)notifications.size() - 1; i >= 0; i--) {
                auto& n = notifications[i];
                QString label = n.title.isEmpty()
                    ? n.body.left(50)
                    : QString("%1: %2").arg(n.title, n.body).left(50);

                QAction* action = menu->addAction(label);
                if (n.read) {
                    action->setEnabled(false);
                } else {
                    int idx = i;
                    connect(action, &QAction::triggered, this, [this, idx]() {
                        auto& entry = notifications[idx];
                        entry.read = true;
                        // Navigate to workspace
                        if (entry.workspaceIdx >= 0 && entry.workspaceIdx < (int)workspaces.size())
                            tabList->setCurrentRow(entry.workspaceIdx);
                        // Navigate to tab
                        if (entry.pane && entry.tabIdx >= 0 && entry.tabIdx < entry.pane->tabs->count()) {
                            entry.pane->tabs->setCurrentIndex(entry.tabIdx);
                            if (entry.tabIdx < (int)entry.pane->terminals.size())
                                entry.pane->terminals[entry.tabIdx]->setFocus();
                        }
                        updateBellBadge();
                    });
                }
            }

            menu->addSeparator();
            QAction* clearAction = menu->addAction("Clear all");
            connect(clearAction, &QAction::triggered, this, [this]() {
                notifications.clear();
                updateBellBadge();
            });
        }

        menu->popup(bellBtn->mapToGlobal(QPoint(0, bellBtn->height())));
        // Auto-delete when closed
        connect(menu, &QMenu::aboutToHide, menu, &QMenu::deleteLater);
    }

    void showShortcutOverlay() {
        auto* existing = findChild<QWidget*>("shortcutOverlay");
        if (existing) { existing->deleteLater(); return; }

        const auto& t = currentTheme();
        QColor bgc(t.bg);
        // Pre-compute theme-aware key button styles for reuse
        QString keyBtnNormal = QString(
            "QPushButton { color: %1; font-size: 12px; font-family: monospace; border: 1px solid %2;"
            "border-radius: 4px; padding: 2px 8px; background: %3; min-width: 100px; }"
            "QPushButton:hover { border-color: %4; color: %5; }"
        ).arg(t.subtext, t.muted, t.overlay, t.accent, t.fg);
        QString keyBtnRecording = QString(
            "QPushButton { color: %1; font-size: 12px; font-family: monospace; border: 2px solid %1;"
            "border-radius: 4px; padding: 2px 8px; background: %2; min-width: 100px; }"
        ).arg(t.yellow, t.muted);
        QString keyBtnConfirmed = QString(
            "QPushButton { color: %1; font-size: 12px; font-family: monospace; border: 1px solid %1;"
            "border-radius: 4px; padding: 2px 8px; background: %2; min-width: 100px; }"
        ).arg(t.green, t.overlay);

        auto* overlay = new QWidget(this);
        overlay->setObjectName("shortcutOverlay");
        overlay->setGeometry(rect());
        overlay->setStyleSheet(QString("background: rgba(%1,%2,%3,0.92);").arg(bgc.red()).arg(bgc.green()).arg(bgc.blue()));

        auto* mainLayout = new QVBoxLayout(overlay);
        mainLayout->setAlignment(Qt::AlignCenter);

        auto* card = new QWidget();
        card->setMaximumWidth(780);
        card->setStyleSheet(QString("background: rgba(%1,%2,%3,0.95); border: 1px solid %4; border-radius: 16px;")
            .arg(bgc.red()).arg(bgc.green()).arg(bgc.blue()).arg(t.overlay));

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(40, 32, 40, 32);
        cardLayout->setSpacing(16);

        auto* title = new QLabel("Keyboard Shortcuts");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet(QString("color: %1; font-size: 20px; font-weight: 600; border: none; background: transparent;").arg(t.fg));
        cardLayout->addWidget(title);

        auto* subtitle = new QLabel("Click any shortcut to rebind it");
        subtitle->setAlignment(Qt::AlignCenter);
        subtitle->setStyleSheet(QString("color: %1; font-size: 12px; border: none; background: transparent;").arg(t.subtext));
        cardLayout->addWidget(subtitle);

        // Recording state (shared via QObject property on overlay)
        // We track which button is being recorded via a custom event filter
        class KeyCaptureFilter : public QObject {
        public:
            QPushButton* activeBtn = nullptr;
            QString activeAction;
            QWidget* overlayWidget;
            QMap<QString, QKeySequence> pendingChanges;
            QString normalStyle, confirmedStyle, recordingStyle;

            KeyCaptureFilter(QWidget* o, const QString& ns, const QString& cs, const QString& rs)
                : QObject(o), overlayWidget(o), normalStyle(ns), confirmedStyle(cs), recordingStyle(rs) {}

            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() != QEvent::KeyPress || !activeBtn) return false;
                QKeyEvent* ke = static_cast<QKeyEvent*>(event);

                // Ignore lone modifier keys
                if (ke->key() == Qt::Key_Shift || ke->key() == Qt::Key_Control ||
                    ke->key() == Qt::Key_Alt || ke->key() == Qt::Key_Meta)
                    return true;

                if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier) {
                    // Cancel recording - show pending change if any, else current binding
                    auto it = pendingChanges.find(activeAction);
                    QKeySequence displaySeq = (it != pendingChanges.end()) ? *it : g_shortcuts->getBinding(activeAction);
                    activeBtn->setText(displaySeq.toString(QKeySequence::NativeText));
                    activeBtn->setStyleSheet(normalStyle);
                    activeBtn = nullptr;
                    activeAction.clear();
                    return true;
                }

                QKeyCombination combo(ke->modifiers(), static_cast<Qt::Key>(ke->key()));
                QKeySequence seq(combo);
                pendingChanges[activeAction] = seq;

                activeBtn->setText(seq.toString(QKeySequence::NativeText));
                activeBtn->setStyleSheet(confirmedStyle);
                auto* btn = activeBtn;
                auto ns = normalStyle;
                QTimer::singleShot(800, btn, [btn, ns]() {
                    btn->setStyleSheet(ns);
                });
                activeBtn = nullptr;
                activeAction.clear();
                return true;
            }
        };

        auto* filter = new KeyCaptureFilter(overlay, keyBtnNormal, keyBtnConfirmed, keyBtnRecording);
        overlay->installEventFilter(filter);

        // Helper: section header
        auto addSection = [&t](QVBoxLayout* col, const char* name) {
            auto* lbl = new QLabel(name);
            lbl->setStyleSheet(QString("color: %1; font-size: 10px; text-transform: uppercase; letter-spacing: 2px; padding-top: 8px; border: none; background: transparent;").arg(t.subtext));
            col->addWidget(lbl);
        };

        // Helper: editable shortcut row
        auto addRow = [filter, overlay](QVBoxLayout* col, const char* label, const QString& action) {
            const auto& t = currentTheme();
            auto* row = new QWidget();
            row->setStyleSheet("border: none; background: transparent;");
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 4, 0, 4);

            auto* lbl = new QLabel(label);
            lbl->setStyleSheet(QString("color: %1; font-size: 13px; border: none; background: transparent;").arg(t.subtext));
            rl->addWidget(lbl);
            rl->addStretch();

            auto* keyBtn = new QPushButton(g_shortcuts->getBinding(action).toString(QKeySequence::NativeText));
            keyBtn->setFocusPolicy(Qt::NoFocus);
            keyBtn->setStyleSheet(filter->normalStyle);

            QObject::connect(keyBtn, &QPushButton::clicked, overlay, [filter, keyBtn, action, overlay]() {
                // Cancel previous recording if any
                if (filter->activeBtn && filter->activeBtn != keyBtn) {
                    filter->activeBtn->setText(g_shortcuts->getBinding(filter->activeAction).toString(QKeySequence::NativeText));
                    filter->activeBtn->setStyleSheet(filter->normalStyle);
                }
                filter->activeBtn = keyBtn;
                filter->activeAction = action;
                keyBtn->setText("Press keys...");
                keyBtn->setStyleSheet(filter->recordingStyle);
                overlay->setFocus();
            });

            rl->addWidget(keyBtn);
            col->addWidget(row);
        };

        // Helper: non-editable shortcut row
        auto addFixed = [&t](QVBoxLayout* col, const char* label, const char* keys) {
            auto* row = new QWidget();
            row->setStyleSheet("border: none; background: transparent;");
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 4, 0, 4);
            auto* lbl = new QLabel(label);
            lbl->setStyleSheet(QString("color: %1; font-size: 12px; border: none; background: transparent;").arg(t.highlight));
            auto* k = new QLabel(keys);
            k->setAlignment(Qt::AlignRight);
            k->setStyleSheet(QString("color: %1; font-size: 11px; font-family: monospace; border: none; background: transparent;").arg(t.muted));
            rl->addWidget(lbl);
            rl->addStretch();
            rl->addWidget(k);
            col->addWidget(row);
        };

        auto* columns = new QHBoxLayout();
        columns->setSpacing(32);

        // Left column
        auto* leftCol = new QVBoxLayout();
        leftCol->setSpacing(4);

        addSection(leftCol, "Workspaces");
        addRow(leftCol, "New workspace", "workspace.new");
        addRow(leftCol, "Close workspace", "workspace.close");
        addRow(leftCol, "Next workspace", "workspace.next");
        addRow(leftCol, "Previous workspace", "workspace.prev");

        addSection(leftCol, "Panes & Tabs");
        addRow(leftCol, "New terminal tab", "pane.tab.new");
        addRow(leftCol, "Zoom pane", "pane.zoom");

        addSection(leftCol, "Power Features");
        addRow(leftCol, "Terminal search", "terminal.search");
        addRow(leftCol, "Broadcast mode", "broadcast.toggle");
        addRow(leftCol, "Quick notes", "notes.toggle");
        addRow(leftCol, "Command history", "history.show");
        leftCol->addStretch();

        // Right column
        auto* rightCol = new QVBoxLayout();
        rightCol->setSpacing(4);

        addSection(rightCol, "Browser");
        addRow(rightCol, "Toggle browser", "browser.toggle");
        addRow(rightCol, "New browser tab", "browser.new");
        addRow(rightCol, "Inspector docked", "devtools.docked");
        addRow(rightCol, "Inspector window", "devtools.window");

        addSection(rightCol, "Window");
        addRow(rightCol, "Search palette", "search.show");
        addRow(rightCol, "Shortcuts overlay", "shortcuts.show");
        addRow(rightCol, "Cycle theme", "theme.cycle");
        addRow(rightCol, "Picture in picture", "pip.toggle");

        addSection(rightCol, "Fixed Shortcuts");
        addFixed(rightCol, "Switch workspace 1-9", "Ctrl+1-9");
        addFixed(rightCol, "Focus pane", "Alt+Arrow");
        addFixed(rightCol, "Cycle tabs", "Ctrl+Tab");
        addFixed(rightCol, "Copy / Paste", "Ctrl+Shift+C/V");
        addFixed(rightCol, "Fullscreen", "F11");
        rightCol->addStretch();

        columns->addLayout(leftCol);

        auto* divider = new QFrame();
        divider->setFrameShape(QFrame::VLine);
        divider->setStyleSheet(QString("color: %1; border: none; background: %1; max-width: 1px;").arg(t.overlay));
        columns->addWidget(divider);

        columns->addLayout(rightCol);
        cardLayout->addLayout(columns);

        // Buttons bar
        auto* btnBar = new QWidget();
        btnBar->setStyleSheet("border: none; background: transparent;");
        auto* btnLayout = new QHBoxLayout(btnBar);
        btnLayout->setContentsMargins(0, 8, 0, 0);

        auto* resetBtn = new QPushButton("Reset to Defaults");
        resetBtn->setFocusPolicy(Qt::NoFocus);
        resetBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 4px; padding: 6px 16px;").arg(t.muted, t.fg));
        connect(resetBtn, &QPushButton::clicked, overlay, [this, overlay]() {
            g_shortcuts->resetToDefaults();
            overlay->deleteLater();
            showShortcutOverlay(); // Reopen with defaults
        });
        btnLayout->addWidget(resetBtn);

        btnLayout->addStretch();

        auto* cancelBtn = new QPushButton("Cancel");
        cancelBtn->setFocusPolicy(Qt::NoFocus);
        cancelBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 4px; padding: 6px 16px;").arg(t.muted, t.fg));
        connect(cancelBtn, &QPushButton::clicked, overlay, [overlay]() {
            overlay->deleteLater();
        });
        btnLayout->addWidget(cancelBtn);

        auto* saveBtn = new QPushButton("Save");
        saveBtn->setFocusPolicy(Qt::NoFocus);
        saveBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 4px; padding: 6px 16px; font-weight: bold;").arg(t.green, t.bg));
        connect(saveBtn, &QPushButton::clicked, overlay, [this, overlay, filter]() {
            for (auto it = filter->pendingChanges.begin(); it != filter->pendingChanges.end(); ++it)
                g_shortcuts->setBinding(it.key(), it.value());
            g_shortcuts->saveToFile();
            overlay->deleteLater();
        });
        btnLayout->addWidget(saveBtn);

        cardLayout->addWidget(btnBar);

        mainLayout->addWidget(card);

        // Close on Escape (when not recording) or Ctrl+Shift+K
        overlay->setFocusPolicy(Qt::StrongFocus);
        connect(new QShortcut(QKeySequence(Qt::Key_Escape), overlay), &QShortcut::activated,
            overlay, [overlay, filter]() {
                if (filter->activeBtn) {
                    // Cancel recording - show pending change if any, else current binding
                    auto it = filter->pendingChanges.find(filter->activeAction);
                    QKeySequence displaySeq = (it != filter->pendingChanges.end()) ? *it : g_shortcuts->getBinding(filter->activeAction);
                    filter->activeBtn->setText(displaySeq.toString(QKeySequence::NativeText));
                    filter->activeBtn->setStyleSheet(filter->normalStyle);
                    filter->activeBtn = nullptr;
                    filter->activeAction.clear();
                    return;
                }
                overlay->deleteLater();
            });
        connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), overlay), &QShortcut::activated,
            overlay, &QWidget::deleteLater);
        overlay->show();
        overlay->raise();
        overlay->setFocus();
    }

    void showSearchOverlay() {
        // Remove existing
        auto* existing = findChild<QWidget*>("searchOverlay");
        if (existing) { existing->deleteLater(); return; }

        const auto& t = currentTheme();
        QColor bgc(t.bg);

        // Overlay
        auto* overlay = new QWidget(this);
        overlay->setObjectName("searchOverlay");
        overlay->setGeometry(rect());
        overlay->setStyleSheet(QString("background: rgba(%1,%2,%3,0.88);").arg(bgc.red()).arg(bgc.green()).arg(bgc.blue()));

        auto* outerLayout = new QVBoxLayout(overlay);
        outerLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        outerLayout->setContentsMargins(0, 80, 0, 0);

        // Search card
        auto* card = new QWidget();
        card->setFixedWidth(560);
        card->setMaximumHeight(500);
        card->setStyleSheet(QString(
            "background: %1;"
            "border: 1px solid %2;"
            "border-radius: 12px;"
        ).arg(t.bg, t.muted));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(0, 0, 0, 0);
        cardLayout->setSpacing(0);

        // Search input
        auto* inputWrapper = new QWidget();
        inputWrapper->setStyleSheet(QString("background: transparent; border-bottom: 1px solid %1; border-radius: 0;").arg(t.overlay));
        auto* inputLayout = new QHBoxLayout(inputWrapper);
        inputLayout->setContentsMargins(20, 16, 20, 16);

        auto* searchIcon = new QLabel(">");
        searchIcon->setStyleSheet(QString("color: %1; font-size: 18px; font-weight: bold; border: none; background: transparent;").arg(t.accent));
        inputLayout->addWidget(searchIcon);

        auto* searchInput = new QLineEdit();
        searchInput->setPlaceholderText("Search workspaces, tabs, browser...");
        searchInput->setStyleSheet(QString(
            "background: transparent; border: none; color: %1;"
            "font-size: 16px; padding: 4px 8px; selection-background-color: %2;"
        ).arg(t.fg, t.highlight));
        inputLayout->addWidget(searchInput);
        cardLayout->addWidget(inputWrapper);

        // Results list
        QColor accentC(t.accent);
        QString accentRgb = QString("%1,%2,%3").arg(accentC.red()).arg(accentC.green()).arg(accentC.blue());
        auto* resultsList = new QListWidget();
        resultsList->setStyleSheet(QString(
            "QListWidget { background: transparent; border: none; outline: none; padding: 4px 0; }"
            "QListWidget::item { color: %1; padding: 10px 20px; border: none; border-radius: 0; }"
            "QListWidget::item:selected { background: rgba(%2, 0.12); }"
            "QListWidget::item:hover { background: rgba(%2, 0.08); }"
        ).arg(t.fg, accentRgb));
        resultsList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        resultsList->setFocusPolicy(Qt::NoFocus);
        cardLayout->addWidget(resultsList);

        // Hint bar
        auto* hintBar = new QWidget();
        hintBar->setStyleSheet(QString("background: transparent; border-top: 1px solid %1; border-radius: 0;").arg(t.overlay));
        auto* hintLayout = new QHBoxLayout(hintBar);
        hintLayout->setContentsMargins(20, 8, 20, 8);
        auto* hintText = new QLabel("↑↓ navigate    Enter select    Esc close");
        hintText->setStyleSheet(QString("color: %1; font-size: 11px; border: none; background: transparent;").arg(t.muted));
        hintLayout->addWidget(hintText);
        cardLayout->addWidget(hintBar);

        outerLayout->addWidget(card);

        // Gather all searchable items
        struct SearchItem {
            QString name;
            QString detail;
            QString icon;
            QString kind;
            int workspaceIdx;
            PaneWidget* pane;
            int tabIdx;
            int browserTabIdx;
        };
        auto* items = new std::vector<SearchItem>();

        // Workspaces
        for (int wi = 0; wi < (int)workspaces.size(); wi++) {
            auto& ws = workspaces[wi];
            items->push_back({
                ws.userRenamed ? ws.name : (ws.title.isEmpty() ? ws.name : ws.title),
                ws.cwd.isEmpty() ? "" : ws.cwd,
                QString::fromUtf8("\xe2\x96\xa3"), // ▣
                "workspace",
                wi, nullptr, -1, -1
            });

            // Pane tabs
            for (auto* pane : ws.panes) {
                for (int ti = 0; ti < pane->tabs->count(); ti++) {
                    QString tabName = pane->tabs->tabText(ti);
                    QString tooltip = pane->tabs->tabToolTip(ti);
                    items->push_back({
                        tabName,
                        tooltip.isEmpty() ? ws.name : tooltip,
                        QString::fromUtf8("\xe2\x96\xb8"), // ▸
                        "terminal",
                        wi, pane, ti, -1
                    });
                }
            }
        }

        // Browser tabs
        for (int bi = 0; bi < browserTabs->count(); bi++) {
            QString title = browserTabs->tabText(bi);
            QWidget* w = browserTabs->widget(bi);
            auto* view = w ? w->findChild<QWebEngineView*>() : nullptr;
            QString url = view ? view->url().toString() : "";
            items->push_back({
                title,
                url,
                QString::fromUtf8("\xe2\x97\x89"), // ◉
                "browser",
                -1, nullptr, -1, bi
            });
        }

        // Populate results
        auto populateResults = [resultsList, items](const QString& query) {
            resultsList->clear();
            for (auto& item : *items) {
                if (!query.isEmpty() &&
                    !item.name.contains(query, Qt::CaseInsensitive) &&
                    !item.detail.contains(query, Qt::CaseInsensitive) &&
                    !item.kind.contains(query, Qt::CaseInsensitive))
                    continue;

                // Format: icon  name                     kind
                QString text = QString("%1  %2").arg(item.icon, item.name);
                auto* listItem = new QListWidgetItem(text);

                // Color the kind badge
                QColor kindColor = item.kind == "workspace" ? QColor("#cba6f7")
                    : item.kind == "terminal" ? QColor("#a6e3a1")
                    : QColor("#89b4fa");

                listItem->setData(Qt::UserRole, resultsList->count());
                listItem->setToolTip(QString("%1\n%2").arg(item.detail, item.kind));
                resultsList->addItem(listItem);
            }
            if (resultsList->count() > 0)
                resultsList->setCurrentRow(0);
        };

        populateResults("");

        // Search as you type
        connect(searchInput, &QLineEdit::textChanged, overlay, [populateResults](const QString& text) {
            populateResults(text);
        });

        // Navigate with arrow keys
        searchInput->installEventFilter(overlay);

        // Handle Enter and arrow keys via event filter on overlay
        auto* overlayPtr = overlay;
        auto* itemsPtr = items;

        connect(resultsList, &QListWidget::itemDoubleClicked, overlay,
            [this, overlayPtr, itemsPtr, resultsList](QListWidgetItem* listItem) {
                int idx = listItem->data(Qt::UserRole).toInt();
                if (idx < 0 || idx >= (int)itemsPtr->size()) return;
                auto& item = (*itemsPtr)[idx];

                if (item.workspaceIdx >= 0)
                    tabList->setCurrentRow(item.workspaceIdx);
                if (item.pane && item.tabIdx >= 0) {
                    item.pane->tabs->setCurrentIndex(item.tabIdx);
                    if (item.tabIdx < (int)item.pane->terminals.size())
                        item.pane->terminals[item.tabIdx]->setFocus();
                }
                if (item.browserTabIdx >= 0) {
                    browserTabs->setCurrentIndex(item.browserTabIdx);
                    browserTabs->setVisible(true);
                }
                overlayPtr->deleteLater();
            });

        // Key handling
        connect(searchInput, &QLineEdit::returnPressed, overlay,
            [resultsList]() {
                auto* item = resultsList->currentItem();
                if (item) emit resultsList->itemDoubleClicked(item);
            });

        // Arrow key navigation
        QObject::connect(new QShortcut(QKeySequence(Qt::Key_Down), searchInput), &QShortcut::activated,
            overlay, [resultsList]() {
                int next = resultsList->currentRow() + 1;
                if (next < resultsList->count()) resultsList->setCurrentRow(next);
            });
        QObject::connect(new QShortcut(QKeySequence(Qt::Key_Up), searchInput), &QShortcut::activated,
            overlay, [resultsList]() {
                int prev = resultsList->currentRow() - 1;
                if (prev >= 0) resultsList->setCurrentRow(prev);
            });

        // Close
        connect(new QShortcut(QKeySequence(Qt::Key_Escape), overlay), &QShortcut::activated,
            overlay, [overlayPtr, itemsPtr]() { delete itemsPtr; overlayPtr->deleteLater(); });
        connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), overlay), &QShortcut::activated,
            overlay, [overlayPtr, itemsPtr]() { delete itemsPtr; overlayPtr->deleteLater(); });

        overlay->show();
        overlay->raise();
        searchInput->setFocus();
    }

    void openDevTools(bool inWindow = false) {
        QWidget* current = browserTabs->currentWidget();
        if (!current) return;
        auto* view = current->findChild<QWebEngineView*>();
        if (!view) return;

        if (inWindow) {
            // Open in separate window
            auto* devToolsView = new QWebEngineView();
            auto* devToolsPage = new QWebEnginePage(devToolsView);
            devToolsView->setPage(devToolsPage);
            view->page()->setDevToolsPage(devToolsPage);
            devToolsView->setWindowTitle("Developer Tools - prettymux");
            devToolsView->resize(900, 600);
            devToolsView->show();
            return;
        }

        // Docked mode: check if already exists, toggle visibility
        auto* existingSplitter = current->findChild<QSplitter*>("devToolsSplitter");
        if (existingSplitter && existingSplitter->count() > 1) {
            auto* devContainer = existingSplitter->widget(1);
            if (devContainer) {
                devContainer->setVisible(!devContainer->isVisible());
                return;
            }
        }

        auto* layout = current->layout();
        if (!layout) return;

        layout->removeWidget(view);

        auto* splitter = new QSplitter(Qt::Vertical);
        splitter->setObjectName("devToolsSplitter");
        splitter->setStyleSheet("QSplitter::handle { background: #313244; height: 3px; }");
        splitter->addWidget(view);

        // Dev tools container with close bar
        auto* devContainer = new QWidget();
        auto* devLayout = new QVBoxLayout(devContainer);
        devLayout->setContentsMargins(0, 0, 0, 0);
        devLayout->setSpacing(0);

        // Close bar
        auto* closeBar = new QWidget();
        closeBar->setFixedHeight(20);
        closeBar->setStyleSheet("background: #181825;");
        auto* closeBarLayout = new QHBoxLayout(closeBar);
        closeBarLayout->setContentsMargins(4, 0, 4, 0);
        closeBarLayout->addStretch();
        auto* closeBtn = new QPushButton("x");
        closeBtn->setFixedSize(16, 16);
        closeBtn->setStyleSheet("background: #45475a; color: #cdd6f4; border: none; border-radius: 3px;");
        connect(closeBtn, &QPushButton::clicked, devContainer, [devContainer]() {
            devContainer->setVisible(false);
        });
        closeBarLayout->addWidget(closeBtn);
        devLayout->addWidget(closeBar);

        auto* devToolsView = new QWebEngineView();
        auto* devToolsPage = new QWebEnginePage(devToolsView);
        devToolsView->setPage(devToolsPage);
        view->page()->setDevToolsPage(devToolsPage);
        devLayout->addWidget(devToolsView);

        splitter->addWidget(devContainer);
        splitter->setSizes({400, 300});
        layout->addWidget(splitter);
    }

    Q_INVOKABLE void openUrlInBrowser(const QString& url) {
        addBrowserTab(url);
        browserTabs->setVisible(true);
    }

    SurfaceLocation findSurfaceLocation(void* surfacePtr) {
        SurfaceLocation loc = {-1, nullptr, -1, nullptr};
        for (int wi = 0; wi < (int)workspaces.size(); wi++) {
            for (auto* pane : workspaces[wi].panes) {
                for (int ti = 0; ti < (int)pane->terminals.size(); ti++) {
                    if (pane->terminals[ti]->getSurface() == surfacePtr) {
                        loc.workspaceIdx = wi;
                        loc.pane = pane;
                        loc.tabIdx = ti;
                        loc.term = pane->terminals[ti];
                        return loc;
                    }
                }
            }
        }
        return loc;
    }

    GhosttyWidget* findTerminalForSurface(void* surfacePtr) {
        return findSurfaceLocation(surfacePtr).term;
    }

    // Flash a terminal widget border briefly
    void flashTerminal(GhosttyWidget* term) {
        if (!term) return;
        // Find the PaneWidget that contains this terminal and flash its border
        for (auto& ws : workspaces) {
            for (auto* pane : ws.panes) {
                for (auto* t : pane->allTerminals()) {
                    if (t == term) {
                        QString origStyle = pane->styleSheet();
                        pane->setStyleSheet("border: 3px solid #f38ba8; border-radius: 0px;");
                        QTimer::singleShot(800, pane, [pane]() {
                            pane->setStyleSheet("border: 2px solid #313244; border-radius: 0px;");
                        });
                        return;
                    }
                }
            }
        }
    }

    // Pulse a tab between two colors until it becomes active
    void highlightPaneTab(PaneWidget* pane, int tabIdx) {
        if (!pane || tabIdx < 0 || tabIdx >= pane->tabs->count()) return;
        auto* bar = pane->tabs->tabBar();
        auto* timer = new QTimer(pane);
        int* phase = new int(0);
        timer->setInterval(600);
        connect(timer, &QTimer::timeout, pane, [bar, pane, tabIdx, timer, phase]() {
            // Stop if tab is now active or removed
            if (pane->tabs->currentIndex() == tabIdx || tabIdx >= pane->tabs->count()) {
                bar->setTabTextColor(tabIdx, QColor("#6c7086"));
                timer->stop();
                timer->deleteLater();
                delete phase;
                return;
            }
            // Alternate between red and dim
            (*phase)++;
            if (*phase % 2 == 0)
                bar->setTabTextColor(tabIdx, QColor("#f38ba8"));
            else
                bar->setTabTextColor(tabIdx, QColor("#45475a"));
        });
        bar->setTabTextColor(tabIdx, QColor("#f38ba8"));
        timer->start();

        // Also stop when tab is clicked
        connect(pane->tabs, &QTabWidget::currentChanged, timer, [timer, bar, tabIdx, phase](int idx) {
            if (idx == tabIdx) {
                bar->setTabTextColor(tabIdx, QColor("#6c7086"));
                timer->stop();
                timer->deleteLater();
                delete phase;
            }
        });
    }

    Q_INVOKABLE void showNotificationFor(quint64 surfacePtr, const QString& title, const QString& body) {
        auto loc = findSurfaceLocation((void*)surfacePtr);
        bool isActiveWorkspace = (loc.workspaceIdx == activeWorkspace);
        bool isActiveTab = false;

        if (loc.pane) {
            isActiveTab = (loc.tabIdx == loc.pane->tabs->currentIndex());
        }

        // Always flash the pane border
        flashTerminal(loc.term);

        // If watching the exact same tab in the active workspace, just flash, no notification
        if (isActiveWorkspace && isActiveTab && isActiveWindow()) {
            return;
        }

        // Store in notification list
        addNotificationEntry(title, body, loc);

        // Highlight the specific tab in the pane if it's a background tab
        if (loc.pane && !isActiveTab) {
            highlightPaneTab(loc.pane, loc.tabIdx);
        }

        // Update sidebar for non-active workspaces
        if (loc.workspaceIdx >= 0 && loc.workspaceIdx < (int)workspaces.size()) {
            workspaces[loc.workspaceIdx].notification = body.left(80);
            refreshSidebarItem(loc.workspaceIdx);
            if (!isActiveWorkspace)
                tabList->item(loc.workspaceIdx)->setForeground(QColor("#f38ba8"));
        }

        // Desktop notification for background tabs or inactive workspaces
        QString t = title.isEmpty() ? "prettymux" : title;

        QProcess* proc = new QProcess(this);
        proc->setProgram("notify-send");
        proc->setArguments({
            t, body,
            "--app-name=prettymux",
            "--action=focus=Focus Terminal",
            "--wait"
        });

        int wsIdx = loc.workspaceIdx;
        PaneWidget* pane = loc.pane;
        int tabIdx = loc.tabIdx;
        int notifIdx = (int)notifications.size() - 1; // last added entry

        connect(proc, &QProcess::finished, this, [this, proc, wsIdx, pane, tabIdx, notifIdx](int exitCode) {
            if (exitCode == 0) {
                raise();
                activateWindow();
                if (wsIdx >= 0 && wsIdx < (int)workspaces.size())
                    tabList->setCurrentRow(wsIdx);
                if (pane && tabIdx >= 0 && tabIdx < pane->tabs->count()) {
                    pane->tabs->setCurrentIndex(tabIdx);
                    if (tabIdx < (int)pane->terminals.size())
                        pane->terminals[tabIdx]->setFocus();
                }
                // Mark as read in dropdown
                if (notifIdx >= 0 && notifIdx < (int)notifications.size())
                    notifications[notifIdx].read = true;
                updateBellBadge();
            }
            proc->deleteLater();
        });

        proc->start();
    }

    Q_INVOKABLE void showBellFor(quint64 surfacePtr) {
        int bellWs = findWorkspaceForSurface((void*)surfacePtr);

        // Always flash the terminal
        flashTerminal(findTerminalForSurface((void*)surfacePtr));

        // Only notify desktop if not watching that workspace
        if (bellWs == activeWorkspace && isActiveWindow()) return;

        if (bellWs >= 0 && bellWs < tabList->count()) {
            auto item = tabList->item(bellWs);
            item->setForeground(QColor("#fab387"));
            QTimer::singleShot(1000, this, [this, item]() {
                item->setForeground(QColor("#cdd6f4"));
            });
        }
        QProcess::startDetached("notify-send", {"prettymux", "Bell", "--app-name=prettymux"});
    }

    void toggleBrowser(bool show) {
        browserTabs->setVisible(show);
    }

    // ── Alt+Arrow pane navigation ──
    void navigatePane(int dx, int dy) {
        if (activeWorkspace < 0 || activeWorkspace >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[activeWorkspace];
        if (ws.panes.size() <= 1) return;

        PaneWidget* current = nullptr;
        for (auto* pane : ws.panes) {
            if (pane->isVisible() && pane->isAncestorOf(QApplication::focusWidget())) {
                current = pane;
                break;
            }
        }
        if (!current) current = ws.panes[0];

        QPoint currentCenter = current->mapToGlobal(QPoint(current->width()/2, current->height()/2));
        PaneWidget* best = nullptr;
        int bestDist = INT_MAX;

        for (auto* pane : ws.panes) {
            if (pane == current || !pane->isVisible()) continue;
            QPoint center = pane->mapToGlobal(QPoint(pane->width()/2, pane->height()/2));
            int diffX = center.x() - currentCenter.x();
            int diffY = center.y() - currentCenter.y();

            bool valid = false;
            if (dx > 0 && diffX > 0) valid = true;
            if (dx < 0 && diffX < 0) valid = true;
            if (dy > 0 && diffY > 0) valid = true;
            if (dy < 0 && diffY < 0) valid = true;
            if (!valid) continue;

            int dist = abs(diffX) + abs(diffY);
            if (dist < bestDist) { bestDist = dist; best = pane; }
        }

        if (best) {
            int idx = best->tabs->currentIndex();
            if (idx >= 0 && idx < (int)best->terminals.size())
                best->terminals[idx]->setFocus();
        }
    }

    // ── Pane zoom (Ctrl+Shift+Z) ──

    void saveSplitterSizesRecursive(QWidget* w, std::vector<QList<int>>& sizes) {
        auto* s = qobject_cast<QSplitter*>(w);
        if (s) {
            sizes.push_back(s->sizes());
            for (int i = 0; i < s->count(); i++)
                saveSplitterSizesRecursive(s->widget(i), sizes);
        }
    }

    void restoreSplitterSizesRecursive(QWidget* w, const std::vector<QList<int>>& sizes, int& idx) {
        auto* s = qobject_cast<QSplitter*>(w);
        if (s && idx < (int)sizes.size()) {
            s->setSizes(sizes[idx++]);
            for (int i = 0; i < s->count(); i++)
                restoreSplitterSizesRecursive(s->widget(i), sizes, idx);
        }
    }

    void toggleZoom() {
        if (activeWorkspace < 0 || activeWorkspace >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[activeWorkspace];

        if (ws.zoomed) {
            // Unzoom: show all panes, restore sizes
            for (auto* pane : ws.panes) pane->show();
            if (!ws.savedSplitterSizes.empty()) {
                int idx = 0;
                restoreSplitterSizesRecursive(ws.container, ws.savedSplitterSizes, idx);
                ws.savedSplitterSizes.clear();
            }
            if (ws.zoomedPane)
                ws.zoomedPane->setStyleSheet("border: 2px solid #313244; border-radius: 0px;");
            ws.zoomed = false;
            ws.zoomedPane = nullptr;
            refreshSidebarItem(activeWorkspace);
            return;
        }

        if (ws.panes.size() <= 1) return;

        // Find focused pane
        PaneWidget* focused = nullptr;
        for (auto* pane : ws.panes) {
            if (pane->isAncestorOf(QApplication::focusWidget())) {
                focused = pane;
                break;
            }
        }
        if (!focused && !ws.panes.empty()) focused = ws.panes[0];
        if (!focused) return;

        // Save splitter sizes before hiding
        ws.savedSplitterSizes.clear();
        saveSplitterSizesRecursive(ws.container, ws.savedSplitterSizes);

        // Hide all other panes
        for (auto* pane : ws.panes) {
            if (pane != focused) pane->hide();
        }
        ws.zoomed = true;
        ws.zoomedPane = focused;
        focused->setStyleSheet("border: 2px solid #f9e2af; border-radius: 0px;");
        refreshSidebarItem(activeWorkspace);
    }

    // ── Terminal search (Ctrl+Shift+F) ──
    void triggerSearch() {
        if (activeWorkspace < 0 || activeWorkspace >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[activeWorkspace];

        for (auto* pane : ws.panes) {
            if (pane->isAncestorOf(QApplication::focusWidget())) {
                int idx = pane->tabs->currentIndex();
                if (idx >= 0 && idx < (int)pane->terminals.size()) {
                    auto* surface = (ghostty_surface_t)pane->terminals[idx]->getSurface();
                    if (surface)
                        ghostty_surface_binding_action(surface, "search_forward", 14);
                }
                return;
            }
        }
    }

    // ── Broadcast mode (Ctrl+Shift+Enter) ──
    void toggleBroadcast() {
        if (activeWorkspace < 0 || activeWorkspace >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[activeWorkspace];
        ws.broadcast = !ws.broadcast;
        refreshSidebarItem(activeWorkspace);
    }

    // ── Port scanner ──
    void scanPorts() {
        QStringList ports;
        for (const QString& path : {QString("/proc/net/tcp"), QString("/proc/net/tcp6")}) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            f.readLine(); // skip header
            while (!f.atEnd()) {
                QByteArray line = f.readLine().trimmed();
                QList<QByteArray> fields;
                // Split by whitespace, collapsing multiple spaces
                for (const QByteArray& part : line.split(' ')) {
                    if (!part.isEmpty()) fields.append(part);
                }
                if (fields.size() < 4) continue;
                if (fields[3] != "0A") continue; // 0A = LISTEN
                QByteArray localAddr = fields[1];
                int colonPos = localAddr.lastIndexOf(':');
                if (colonPos < 0) continue;
                bool ok;
                uint port = localAddr.mid(colonPos + 1).toUInt(&ok, 16);
                if (!ok || port == 0) continue;
                // Only show dev ports (1024-65535), skip system services
                if (port < 1024) continue;
                // Skip common system service ports
                if (port == 5432 || port == 5433 || port == 5434  // postgres
                    || port == 6379 || port == 6380               // redis
                    || port == 11434                               // ollama
                    || port == 4723 || port == 5037 || port == 5054 // adb/avahi
                    || port >= 35000)                               // ephemeral ports
                    continue;
                QString portStr = QString::number(port);
                if (!ports.contains(portStr))
                    ports.append(portStr);
            }
        }
        std::sort(ports.begin(), ports.end(), [](const QString& a, const QString& b) {
            return a.toInt() < b.toInt();
        });

        // Detect new ports
        QStringList newPorts;
        for (const QString& p : ports) {
            if (!lastKnownPorts.contains(p))
                newPorts.append(p);
        }
        lastKnownPorts = ports;

        // Only show ports on workspaces that have a running command
        // (title contains something other than just a path)
        for (int i = 0; i < (int)workspaces.size(); i++) {
            auto& ws = workspaces[i];
            bool hasRunningProcess = false;

            // Check if any terminal in this workspace has a running process
            // A running process means the title is NOT just a directory path
            if (!ws.title.isEmpty() && !ws.title.contains('/') && ws.title != "bash" && ws.title != "zsh" && ws.title != "fish" && ws.title != "sh") {
                hasRunningProcess = true;
            }

            QStringList wsPorts = hasRunningProcess ? ports : QStringList();
            if (ws.listeningPorts != wsPorts) {
                ws.listeningPorts = wsPorts;
                refreshSidebarItem(i);
            }
        }

        // Notify about new ports and offer to open in browser
        if (!newPorts.isEmpty() && activeWorkspace >= 0 && activeWorkspace < (int)workspaces.size()) {
            auto& ws = workspaces[activeWorkspace];
            if (!ws.title.isEmpty() && !ws.title.contains('/') && ws.title != "bash" && ws.title != "zsh") {
                QString port = newPorts.first();
                ws.notification = QString("-> localhost:%1").arg(newPorts.join(", "));
                ws.newPort = port.toInt();
                refreshSidebarItem(activeWorkspace);

                // Desktop notification with "Open in Browser" action
                auto* proc = new QProcess(this);
                proc->setProgram("notify-send");
                proc->setArguments({
                    "prettymux", QString("Port %1 detected").arg(port),
                    "--app-name=prettymux",
                    "--action=open=Open in Browser",
                    "--wait"
                });
                connect(proc, &QProcess::finished, this, [this, proc, port](int exitCode) {
                    if (exitCode == 0) {
                        addBrowserTab(QString("http://localhost:%1").arg(port));
                        browserTabs->setVisible(true);
                        raise();
                        activateWindow();
                    }
                    proc->deleteLater();
                });
                proc->start();
            }
        }
    }

    // ── Quick Notes toggle ──
    void toggleNotes() {
        if (!notesPanel) return;
        if (notesPanel->isVisible()) {
            if (activeWorkspace >= 0 && activeWorkspace < (int)workspaces.size())
                workspaces[activeWorkspace].notes = notesPanel->toPlainText();
            notesPanel->hide();
        } else {
            if (activeWorkspace >= 0 && activeWorkspace < (int)workspaces.size())
                notesPanel->setPlainText(workspaces[activeWorkspace].notes);
            notesPanel->show();
            notesPanel->setFocus();
        }
    }

    // ── Activity indicator (called from action_cb on RENDER) ──
    Q_INVOKABLE void markActivity(quint64 surfacePtr) {
        auto loc = findSurfaceLocation((void*)surfacePtr);
        if (loc.workspaceIdx < 0 || !loc.pane || !loc.term) return;
        if (loc.tabIdx == loc.pane->tabs->currentIndex()) return;
        if (loc.term->hasNewOutput) return;  // already marked
        loc.term->hasNewOutput = true;
        loc.pane->markTabActivity(loc.tabIdx);
    }

    // ── Progress bar (called from action_cb on PROGRESS_REPORT) ──
    Q_INVOKABLE void updateTerminalProgress(quint64 surfacePtr, int state, int percent) {
        auto loc = findSurfaceLocation((void*)surfacePtr);
        if (loc.workspaceIdx < 0 || !loc.pane || !loc.term) return;
        loc.term->progressState = state;
        loc.term->progressPercent = percent;
        loc.pane->updateTabProgress(loc.tabIdx, state, percent);
    }

    // Broadcast helpers (public so static functions can access them)
    bool isBroadcastForWorkspace(GhosttyWidget* term) {
        for (auto& ws : workspaces) {
            if (!ws.broadcast) continue;
            for (auto* pane : ws.panes) {
                for (auto* t : pane->allTerminals()) {
                    if (t == term) return true;
                }
            }
        }
        return false;
    }

    std::vector<GhosttyWidget*> getAllTerminalsInWorkspace(GhosttyWidget* term) {
        std::vector<GhosttyWidget*> result;
        for (auto& ws : workspaces) {
            for (auto* pane : ws.panes) {
                for (auto* t : pane->allTerminals()) {
                    if (t == term) {
                        for (auto* p : ws.panes)
                            for (auto* tt : p->allTerminals())
                                result.push_back(tt);
                        return result;
                    }
                }
            }
        }
        return result;
    }

    // ── URL history helper ──
    void addUrlToHistory(const QString& url) {
        if (url.isEmpty() || url == "about:blank") return;
        urlHistory.removeAll(url);
        urlHistory.prepend(url);
        while (urlHistory.size() > 500) urlHistory.removeLast();
    }

    // ── Theme system ──
    void applyTheme() {
        const auto& t = currentTheme();

        // QPalette
        QPalette palette;
        palette.setColor(QPalette::Window, QColor(t.bg));
        palette.setColor(QPalette::WindowText, QColor(t.fg));
        palette.setColor(QPalette::Base, QColor(t.surface));
        palette.setColor(QPalette::Text, QColor(t.fg));
        palette.setColor(QPalette::Button, QColor(t.overlay));
        palette.setColor(QPalette::ButtonText, QColor(t.fg));
        palette.setColor(QPalette::Highlight, QColor(t.accent));
        qApp->setPalette(palette);

        // Main window
        setStyleSheet(QString("QMainWindow { background: %1; } QToolTip { color: %2; background: %3; border: 1px solid %4; padding: 4px; }").arg(t.bg, t.fg, t.overlay, t.muted));

        // Sidebar
        if (sidebar)
            sidebar->setStyleSheet(QString("background: %1; border-right: 1px solid %2;").arg(t.surface, t.overlay));

        // Tab list
        if (tabList)
            tabList->setStyleSheet(QString(
                "QListWidget { background: %1; border: none; outline: none; }"
                "QListWidget::item { color: #a6adc8; padding: 10px 12px; border-left: 3px solid transparent; }"
                "QListWidget::item:selected { background: rgba(137,180,250,0.15); border-left: 3px solid %2; color: %3; }"
                "QListWidget::item:hover { background: %4; }"
            ).arg(t.surface, t.blue, t.fg, t.bg));

        // Browser tabs
        if (browserTabs)
            browserTabs->setStyleSheet(QString(
                "QTabWidget::pane { border: none; background: %1; }"
                "QTabBar::tab { background: %2; color: %3; padding: 6px 12px; border: none; border-bottom: 2px solid transparent; }"
                "QTabBar::tab:selected { color: %4; border-bottom: 2px solid %5; background: %1; }"
                "QTabBar::tab:hover { color: %4; background: %6; }"
                "QTabBar::close-button { image: none; }"
            ).arg(t.bg, t.surface, t.subtext, t.fg, t.accent, t.overlay));

        // Terminal stack
        if (terminalStack)
            terminalStack->setStyleSheet(QString("background: %1;").arg(t.bg));

        // Notes panel
        if (notesPanel)
            notesPanel->setStyleSheet(QString(
                "QTextEdit { background: %1; color: %2; border-top: 1px solid %3;"
                "padding: 12px; font-family: monospace; font-size: 13px; selection-background-color: %4; }"
            ).arg(t.bg, t.fg, t.overlay, t.highlight));

        // Splitter handles
        if (mainSplitter)
            mainSplitter->setStyleSheet(QString("QSplitter::handle { background: %1; width: 4px; }").arg(t.overlay));
        if (terminalArea)
            terminalArea->setStyleSheet(QString("QSplitter::handle { background: %1; height: 3px; }").arg(t.highlight));

        // All pane widgets
        for (auto& ws : workspaces)
            for (auto* pane : ws.panes)
                pane->applyTabStyle();

        // Outer splitter handle
        if (outerSplit)
            outerSplit->setStyleSheet(QString("QSplitter::handle { background: %1; width: 3px; }").arg(t.overlay));

        // Bell button
        if (bellBtn)
            bellBtn->setStyleSheet(QString("background: %1; border: none; border-radius: 4px;").arg(t.overlay));

        // Sidebar header and toolbar
        if (sidebar) {
            auto* header = sidebar->findChild<QWidget*>("sidebarHeader");
            if (header)
                header->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2; padding: 8px;").arg(t.surface, t.overlay));
            auto* toolbar = sidebar->findChild<QWidget*>("sidebarToolbar");
            if (toolbar)
                toolbar->setStyleSheet(QString("background: %1; border-top: 1px solid %2;").arg(t.surface, t.overlay));
        }

        // Sidebar search box and sub-widgets
        if (sidebar) {
            for (auto* le : sidebar->findChildren<QLineEdit*>()) {
                le->setStyleSheet(QString(
                    "background: %1; color: %2; border: none; border-radius: 4px;"
                    "padding: 6px 10px; margin: 6px 8px; selection-background-color: %3;"
                ).arg(t.overlay, t.fg, t.highlight));
            }
            for (auto* btn : sidebar->findChildren<QPushButton*>()) {
                if (btn == bellBtn) continue;
                btn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 3px; padding: 4px 8px;").arg(t.overlay, t.fg));
            }
            for (auto* lbl : sidebar->findChildren<QLabel*>()) {
                if (lbl->text() == "prettymux")
                    lbl->setStyleSheet(QString("color: %1; font-weight: bold;").arg(t.accent));
            }
        }

        // Browser address bars and controls
        if (browserTabs) {
            QString btnStyle = QString("background: %1; color: %2; border: none; border-radius: 4px;").arg(t.overlay, t.fg);
            for (int i = 0; i < browserTabs->count(); i++) {
                QWidget* container = browserTabs->widget(i);
                if (!container) continue;
                // Find address bar (fixed height 36 widget)
                for (auto* child : container->children()) {
                    auto* w = qobject_cast<QWidget*>(child);
                    if (w && w->maximumHeight() == 36) {
                        w->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;").arg(t.surface, t.overlay));
                    }
                }
                // URL bars
                for (auto* urlBar : container->findChildren<QLineEdit*>()) {
                    urlBar->setStyleSheet(QString(
                        "background: %1; color: %2; border: none; border-radius: 4px;"
                        "padding: 4px 8px; selection-background-color: %3;"
                    ).arg(t.overlay, t.fg, t.highlight));
                    if (urlBar->completer() && urlBar->completer()->popup()) {
                        urlBar->completer()->popup()->setStyleSheet(QString(
                            "QListView { background: %1; color: %2; border: 1px solid %3; padding: 2px; }"
                            "QListView::item { padding: 4px 8px; }"
                            "QListView::item:selected { background: %4; }"
                        ).arg(t.surface, t.fg, t.overlay, t.overlay));
                    }
                }
                // Address bar buttons (28x28)
                for (auto* btn : container->findChildren<QPushButton*>()) {
                    if (btn->maximumWidth() == 28)
                        btn->setStyleSheet(btnStyle);
                }
            }
            // Browser corner new-tab button (24x24)
            for (auto* btn : browserTabs->findChildren<QPushButton*>()) {
                if (btn->maximumWidth() == 24)
                    btn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 4px;").arg(t.overlay, t.fg));
            }
        }

        // PiP window
        if (pipWindow) {
            auto* pipFrame = pipWindow->findChild<QWidget*>("pipFrame");
            if (pipFrame)
                pipFrame->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 8px;").arg(t.bg, t.muted));
            auto* titleBar = pipWindow->findChild<QWidget*>("pipTitleBar");
            if (titleBar)
                titleBar->setStyleSheet(QString("background: %1; border-radius: 8px 8px 0 0; border: none;").arg(t.surface));
            auto* titleLabel = pipWindow->findChild<QLabel*>("pipTitle");
            if (titleLabel)
                titleLabel->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent; border: none;").arg(t.fg));
            auto* pipCloseBtn = pipWindow->findChild<QPushButton*>("pipCloseBtn");
            if (pipCloseBtn)
                pipCloseBtn->setStyleSheet(QString("background: %1; color: %2; border: none; border-radius: 3px;").arg(t.muted, t.fg));
        }

        // Save theme preference
        QString configDir = QDir::homePath() + "/.config/prettymux";
        QDir().mkpath(configDir);
        QFile f(configDir + "/theme.conf");
        if (f.open(QIODevice::WriteOnly)) {
            f.write(kThemes[g_currentTheme].name.toUtf8());
            f.close();
        }
    }

    void cycleTheme() {
        g_currentTheme = (g_currentTheme + 1) % kThemeCount;
        applyTheme();

        // Brief notification
        auto* label = new QLabel(QString("Theme: %1").arg(currentTheme().name), this);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QString(
            "background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 12px 24px; font-size: 14px;"
        ).arg(currentTheme().overlay, currentTheme().fg, currentTheme().muted));
        label->adjustSize();
        label->move((width() - label->width()) / 2, height() / 2 - label->height() / 2);
        label->show();
        label->raise();
        QTimer::singleShot(1000, label, &QLabel::deleteLater);
    }

    // ── Command history search (Ctrl+Shift+H) ──
    void showHistoryOverlay() {
        auto* existing = findChild<QWidget*>("historyOverlay");
        if (existing) { existing->deleteLater(); return; }

        // Capture focused terminal before overlay steals focus
        GhosttyWidget* focusedTerm = nullptr;
        if (activeWorkspace >= 0 && activeWorkspace < (int)workspaces.size()) {
            Workspace& ws = workspaces[activeWorkspace];
            for (auto* pane : ws.panes) {
                if (pane->isAncestorOf(QApplication::focusWidget())) {
                    int idx = pane->tabs->currentIndex();
                    if (idx >= 0 && idx < (int)pane->terminals.size())
                        focusedTerm = pane->terminals[idx];
                    break;
                }
            }
            if (!focusedTerm && !ws.panes.empty()) {
                auto* pane = ws.panes[0];
                int idx = pane->tabs->currentIndex();
                if (idx >= 0 && idx < (int)pane->terminals.size())
                    focusedTerm = pane->terminals[idx];
            }
        }

        const auto& t = currentTheme();
        auto* overlay = new QWidget(this);
        overlay->setObjectName("historyOverlay");
        overlay->setGeometry(rect());
        QColor bgc(t.bg);
        overlay->setStyleSheet(QString("background: rgba(%1,%2,%3, 0.88);").arg(bgc.red()).arg(bgc.green()).arg(bgc.blue()));

        auto* outerLayout = new QVBoxLayout(overlay);
        outerLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        outerLayout->setContentsMargins(0, 80, 0, 0);

        auto* card = new QWidget();
        card->setFixedWidth(600);
        card->setMaximumHeight(500);
        card->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 12px;").arg(t.bg, t.muted));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(0, 0, 0, 0);
        cardLayout->setSpacing(0);

        // Input
        auto* inputWrapper = new QWidget();
        inputWrapper->setStyleSheet(QString("background: transparent; border-bottom: 1px solid %1; border-radius: 0;").arg(t.overlay));
        auto* inputLayout = new QHBoxLayout(inputWrapper);
        inputLayout->setContentsMargins(20, 16, 20, 16);

        auto* icon = new QLabel("H");
        icon->setStyleSheet(QString("color: %1; font-size: 18px; font-weight: bold; border: none; background: transparent;").arg(t.accent));
        inputLayout->addWidget(icon);

        auto* searchInput = new QLineEdit();
        searchInput->setPlaceholderText("Search command history...");
        searchInput->setStyleSheet(QString(
            "background: transparent; border: none; color: %1; font-size: 16px; padding: 4px 8px; selection-background-color: %2;"
        ).arg(t.fg, t.highlight));
        inputLayout->addWidget(searchInput);
        cardLayout->addWidget(inputWrapper);

        // Results
        QColor accentC(t.accent);
        QString accentRgb = QString("%1,%2,%3").arg(accentC.red()).arg(accentC.green()).arg(accentC.blue());
        auto* resultsList = new QListWidget();
        resultsList->setStyleSheet(QString(
            "QListWidget { background: transparent; border: none; outline: none; padding: 4px 0; }"
            "QListWidget::item { color: %1; padding: 8px 20px; border: none; border-radius: 0; font-family: monospace; font-size: 13px; }"
            "QListWidget::item:selected { background: rgba(%2, 0.12); }"
            "QListWidget::item:hover { background: rgba(%2, 0.08); }"
        ).arg(t.fg, accentRgb));
        resultsList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        resultsList->setFocusPolicy(Qt::NoFocus);
        cardLayout->addWidget(resultsList);

        // Hint
        auto* hintBar = new QWidget();
        hintBar->setStyleSheet(QString("background: transparent; border-top: 1px solid %1; border-radius: 0;").arg(t.overlay));
        auto* hintLayout = new QHBoxLayout(hintBar);
        hintLayout->setContentsMargins(20, 8, 20, 8);
        auto* hintText = new QLabel("Enter: type into terminal    Esc: close");
        hintText->setStyleSheet(QString("color: %1; font-size: 11px; border: none; background: transparent;").arg(t.muted));
        hintLayout->addWidget(hintText);
        cardLayout->addWidget(hintBar);

        outerLayout->addWidget(card);

        // Read history file
        QString histFile = qgetenv("HISTFILE");
        if (histFile.isEmpty()) histFile = QDir::homePath() + "/.bash_history";
        QStringList* commands = new QStringList();
        QFile hf(histFile);
        if (hf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!hf.atEnd()) {
                QString line = QString::fromUtf8(hf.readLine()).trimmed();
                if (!line.isEmpty() && !line.startsWith('#'))
                    commands->prepend(line); // most recent first
            }
            hf.close();
        }
        commands->removeDuplicates();

        auto populateResults = [resultsList, commands](const QString& query) {
            resultsList->clear();
            int count = 0;
            for (const QString& cmd : *commands) {
                if (count >= 100) break;
                if (!query.isEmpty() && !cmd.contains(query, Qt::CaseInsensitive)) continue;
                resultsList->addItem(cmd);
                count++;
            }
            if (resultsList->count() > 0) resultsList->setCurrentRow(0);
        };
        populateResults("");

        connect(searchInput, &QLineEdit::textChanged, overlay, [populateResults](const QString& text) {
            populateResults(text);
        });

        // Select command -> type into the terminal that was focused when overlay opened
        auto selectCommand = [this, overlay, resultsList, commands, focusedTerm]() {
            auto* item = resultsList->currentItem();
            if (!item) return;
            QString cmd = item->text();
            delete commands;
            overlay->deleteLater();

            // Type into the captured focused terminal
            if (focusedTerm && focusedTerm->getSurface()) {
                auto* surface = (ghostty_surface_t)focusedTerm->getSurface();
                QByteArray utf8 = cmd.toUtf8();
                ghostty_surface_text(surface, utf8.constData(), utf8.size());
            }
        };

        connect(resultsList, &QListWidget::itemDoubleClicked, overlay, [selectCommand](QListWidgetItem*) {
            selectCommand();
        });
        connect(searchInput, &QLineEdit::returnPressed, overlay, selectCommand);

        // Arrow keys
        QObject::connect(new QShortcut(QKeySequence(Qt::Key_Down), searchInput), &QShortcut::activated,
            overlay, [resultsList]() {
                int next = resultsList->currentRow() + 1;
                if (next < resultsList->count()) resultsList->setCurrentRow(next);
            });
        QObject::connect(new QShortcut(QKeySequence(Qt::Key_Up), searchInput), &QShortcut::activated,
            overlay, [resultsList]() {
                int prev = resultsList->currentRow() - 1;
                if (prev >= 0) resultsList->setCurrentRow(prev);
            });

        // Close
        connect(new QShortcut(QKeySequence(Qt::Key_Escape), overlay), &QShortcut::activated,
            overlay, [overlay, commands]() { delete commands; overlay->deleteLater(); });
        connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H), overlay), &QShortcut::activated,
            overlay, [overlay, commands]() { delete commands; overlay->deleteLater(); });

        overlay->show();
        overlay->raise();
        searchInput->setFocus();
    }

    // ── Picture in Picture (Ctrl+Shift+M) ──
    void togglePiP() {
        // If PiP is already showing, close it (restores view)
        if (pipWindow) {
            pipWindow->close();
            return;
        }

        QWidget* current = browserTabs->currentWidget();
        if (!current) return;
        auto* view = current->findChild<QWebEngineView*>();
        if (!view) return;

        int tabIdx = browserTabs->currentIndex();

        pipWindow = new PiPWindow(nullptr);
        pipWindow->setView(view, tabIdx, browserTabs, current);

        connect(pipWindow, &PiPWindow::restoreRequested, this, [this]() {
            if (!pipWindow || !pipWindow->view) return;

            // Move view back to its original container
            QWidget* container = pipWindow->originalContainer;
            if (container) {
                auto* layout = container->layout();
                if (layout) layout->addWidget(pipWindow->view);
            }

            pipWindow->deleteLater();
            pipWindow = nullptr;
        });

        // Position near bottom-right of main window
        QPoint pos = mapToGlobal(QPoint(width() - 580, height() - 400));
        pipWindow->move(pos);
        pipWindow->show();
    }

    // ── Resize overlay ──
    void connectSplitterOverlay(QSplitter* splitter) {
        connect(splitter, &QSplitter::splitterMoved, this, [this, splitter](int, int) {
            // Show overlay with pane dimensions
            auto* existing = findChild<QLabel*>("resizeOverlayLabel");
            if (existing) existing->deleteLater();

            QStringList dims;
            for (int i = 0; i < splitter->count(); i++) {
                QWidget* w = splitter->widget(i);
                dims << QString("%1x%2").arg(w->width()).arg(w->height());
            }

            auto* label = new QLabel(dims.join("  |  "), this);
            label->setObjectName("resizeOverlayLabel");
            label->setAlignment(Qt::AlignCenter);
            label->setStyleSheet(QString(
                "background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 6px 14px; font-size: 12px; font-family: monospace;"
            ).arg(currentTheme().overlay, currentTheme().fg, currentTheme().muted));
            label->adjustSize();

            // Position near the splitter handle
            QPoint splitterCenter = splitter->mapToGlobal(QPoint(splitter->width() / 2, splitter->height() / 2));
            QPoint localPos = mapFromGlobal(splitterCenter);
            label->move(localPos.x() - label->width() / 2, localPos.y() - label->height() / 2);
            label->show();
            label->raise();

            QTimer::singleShot(1200, label, &QLabel::deleteLater);
        });
    }

    // ── Session save/restore ──

    QString sessionPath() {
        QString dir = QDir::homePath() + "/.prettymux/sessions";
        QDir().mkpath(dir);
        return dir + "/last.json";
    }

    void saveSession() {
        QJsonObject root;
        root["version"] = 1;
        root["windowX"] = x();
        root["windowY"] = y();
        root["windowW"] = width();
        root["windowH"] = height();
        root["activeWorkspace"] = activeWorkspace;
        root["browserVisible"] = browserTabs->isVisible();

        // Save browser tabs
        QJsonArray browserArr;
        for (int i = 0; i < browserTabs->count(); i++) {
            QWidget* w = browserTabs->widget(i);
            auto* view = w->findChild<QWebEngineView*>();
            if (view) {
                QJsonObject tab;
                tab["url"] = view->url().toString();
                tab["title"] = browserTabs->tabText(i);
                browserArr.append(tab);
            }
        }
        root["browserTabs"] = browserArr;

        // Save URL history
        QJsonArray urlHistArr;
        for (const QString& u : urlHistory) urlHistArr.append(u);
        root["urlHistory"] = urlHistArr;
        root["theme"] = kThemes[g_currentTheme].name;

        // Save workspaces
        QJsonArray wsArr;
        for (auto& ws : workspaces) {
            QJsonObject wsObj;
            wsObj["name"] = ws.name;
            wsObj["userRenamed"] = ws.userRenamed;

            QJsonArray panesArr;
            for (auto* pane : ws.panes) {
                QJsonObject paneObj;
                QJsonArray tabsArr;
                for (int i = 0; i < (int)pane->terminals.size(); i++) {
                    QJsonObject tabObj;
                    tabObj["name"] = pane->tabs->tabText(i);
                    tabObj["cwd"] = pane->terminals[i]->lastCwd;

                    // Try to dump terminal scrollback
                    auto* term = pane->terminals[i];
                    if (term->getSurface()) {
                        auto* surface = (ghostty_surface_t)term->getSurface();
                        ghostty_surface_size_s sz = ghostty_surface_size(surface);
                        // Select all visible text
                        ghostty_selection_s sel;
                        memset(&sel, 0, sizeof(sel));
                        sel.bottom_right.x = sz.columns > 0 ? sz.columns - 1 : 79;
                        sel.bottom_right.y = sz.rows > 0 ? sz.rows - 1 : 23;
                        ghostty_text_s text = {};
                        if (ghostty_surface_read_text(surface, sel, &text)) {
                            if (text.text && text.text_len > 0) {
                                // Save last 5000 chars of scrollback
                                size_t start = text.text_len > 5000 ? text.text_len - 5000 : 0;
                                tabObj["scrollback"] = QString::fromUtf8(text.text + start, text.text_len - start);
                            }
                            ghostty_surface_free_text(surface, &text);
                        }
                    }

                    tabsArr.append(tabObj);
                }
                paneObj["tabs"] = tabsArr;
                paneObj["activeTab"] = pane->tabs->currentIndex();
                panesArr.append(paneObj);
            }
            wsObj["panes"] = panesArr;
            wsObj["notes"] = ws.notes;
            wsArr.append(wsObj);
        }
        root["workspaces"] = wsArr;

        QFile f(sessionPath());
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(root).toJson());
            f.close();
        }
    }

    void restoreSession() {
        QFile f(sessionPath());
        if (!f.open(QIODevice::ReadOnly)) return;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject()) return;

        QJsonObject root = doc.object();
        if (root["version"].toInt() != 1) return;

        // Restore window geometry
        int wx = root["windowX"].toInt(100);
        int wy = root["windowY"].toInt(100);
        int ww = root["windowW"].toInt(1400);
        int wh = root["windowH"].toInt(900);
        setGeometry(wx, wy, ww, wh);

        // Restore URL history
        QJsonArray urlHistArr = root["urlHistory"].toArray();
        urlHistory.clear();
        for (auto val : urlHistArr) urlHistory.append(val.toString());

        // Restore theme
        QString savedTheme = root["theme"].toString();
        for (int i = 0; i < kThemeCount; i++) {
            if (kThemes[i].name == savedTheme) { g_currentTheme = i; break; }
        }

        // Restore browser visibility
        browserTabs->setVisible(root["browserVisible"].toBool(true));

        // Restore browser tabs
        QJsonArray browserArr = root["browserTabs"].toArray();
        // Remove default welcome tab first
        while (browserTabs->count() > 0) {
            QWidget* w = browserTabs->widget(0);
            browserTabs->removeTab(0);
            w->deleteLater();
        }
        for (auto val : browserArr) {
            QJsonObject tab = val.toObject();
            addBrowserTab(tab["url"].toString());
        }
        if (browserTabs->count() == 0)
            addBrowserTab("file:///home/pe/newnewrepos/w/yo/prettymux/src/qt/welcome.html");

        // Restore workspaces
        QJsonArray wsArr = root["workspaces"].toArray();
        if (wsArr.isEmpty()) return;

        // Remove the default workspace created in constructor
        // (we'll recreate from saved state)
        while (!workspaces.empty()) {
            Workspace& ws = workspaces.back();
            terminalStack->removeWidget(ws.container);
            for (auto* p : ws.panes) p->deleteLater();
            ws.container->deleteLater();
            workspaces.pop_back();
        }
        while (tabList->count() > 0)
            delete tabList->takeItem(0);

        for (auto wsVal : wsArr) {
            QJsonObject wsObj = wsVal.toObject();
            addWorkspace();
            Workspace& ws = workspaces.back();
            ws.name = wsObj["name"].toString(ws.name);
            ws.userRenamed = wsObj["userRenamed"].toBool(false);
            ws.notes = wsObj["notes"].toString();
            tabList->item(workspaces.size() - 1)->setText(ws.name);

            // Restore panes (first pane already created by addWorkspace)
            QJsonArray panesArr = wsObj["panes"].toArray();
            for (int pi = 0; pi < panesArr.size(); pi++) {
                QJsonObject paneObj = panesArr[pi].toObject();
                PaneWidget* pane;
                if (pi == 0) {
                    pane = ws.panes[0];
                } else {
                    // Split to create new pane
                    pane = new PaneWidget();
                    connectPane(pane);
                    ws.panes.push_back(pane);
                    // Add to splitter
                    auto* splitter = qobject_cast<QSplitter*>(ws.container);
                    if (splitter) {
                        splitter->addWidget(pane);
                    }
                }

                // Restore tabs in this pane
                QJsonArray tabsArr = paneObj["tabs"].toArray();
                for (int ti = 0; ti < tabsArr.size(); ti++) {
                    QJsonObject tabObj = tabsArr[ti].toObject();
                    QString savedCwd = tabObj["cwd"].toString();
                    if (ti > 0) pane->addNewTab(savedCwd);

                    // Set tab name
                    QString name = tabObj["name"].toString();
                    if (!name.isEmpty())
                        pane->tabs->setTabText(ti, name);

                    // cd to saved directory after terminal starts
                    if (!savedCwd.isEmpty() && ti < (int)pane->terminals.size()) {
                        auto* term = pane->terminals[ti];
                        term->lastCwd = savedCwd;
                        QTimer::singleShot(500, term, [term, savedCwd]() {
                            if (term->getSurface()) {
                                auto* s = (ghostty_surface_t)term->getSurface();
                                // Type the cd command
                                QString cmd = QString("cd '%1' && clear").arg(
                                    QString(savedCwd).replace("'", "'\\''"));
                                QByteArray utf8 = cmd.toUtf8();
                                ghostty_surface_text(s, utf8.constData(), utf8.size());
                                // Send Enter key
                                ghostty_input_key_s ke = {};
                                ke.action = GHOSTTY_ACTION_PRESS;
                                ke.keycode = 36; // XKB keycode for Return
                                ke.text = "\n";
                                ghostty_surface_key(s, ke);
                            }
                        });
                    }
                }

                int activeTab = paneObj["activeTab"].toInt(0);
                if (activeTab < pane->tabs->count())
                    pane->tabs->setCurrentIndex(activeTab);
            }
        }

        // Restore active workspace
        int aw = root["activeWorkspace"].toInt(0);
        if (aw >= 0 && aw < (int)workspaces.size())
            tabList->setCurrentRow(aw);
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        saveSession();
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        // Dispatch configurable shortcuts via ShortcutManager
        if (g_shortcuts) {
            QString action = g_shortcuts->matchShortcut(event);
            if (!action.isEmpty()) {
                if (action == "workspace.new") { addWorkspace(); return; }
                if (action == "workspace.close") {
                    if ((int)workspaces.size() > 1 && activeWorkspace >= 0) {
                        Workspace& ws = workspaces[activeWorkspace];
                        terminalStack->removeWidget(ws.container);
                        for (auto* p : ws.panes) p->deleteLater();
                        ws.container->deleteLater();
                        workspaces.erase(workspaces.begin() + activeWorkspace);
                        delete tabList->takeItem(activeWorkspace);
                        if (activeWorkspace >= (int)workspaces.size())
                            activeWorkspace = workspaces.size() - 1;
                        tabList->setCurrentRow(activeWorkspace);
                    }
                    return;
                }
                if (action == "workspace.next") {
                    if (!workspaces.empty())
                        tabList->setCurrentRow((activeWorkspace + 1) % workspaces.size());
                    return;
                }
                if (action == "workspace.prev") {
                    if (!workspaces.empty())
                        tabList->setCurrentRow((activeWorkspace - 1 + workspaces.size()) % workspaces.size());
                    return;
                }
                if (action == "pane.tab.new") { addTabInFocusedPane(); return; }
                if (action == "browser.toggle") { browserTabs->setVisible(!browserTabs->isVisible()); return; }
                if (action == "browser.new") { addBrowserTab("file:///home/pe/newnewrepos/w/yo/prettymux/src/qt/welcome.html"); return; }
                if (action == "devtools.docked") { openDevTools(false); return; }
                if (action == "devtools.window") { openDevTools(true); return; }
                if (action == "shortcuts.show") { showShortcutOverlay(); return; }
                if (action == "search.show") { showSearchOverlay(); return; }
                if (action == "pane.zoom") { toggleZoom(); return; }
                if (action == "terminal.search") { triggerSearch(); return; }
                if (action == "broadcast.toggle") { toggleBroadcast(); return; }
                if (action == "notes.toggle") { toggleNotes(); return; }
                if (action == "theme.cycle") { cycleTheme(); return; }
                if (action == "history.show") { showHistoryOverlay(); return; }
                if (action == "pip.toggle") { togglePiP(); return; }
            }
        }

        // Ctrl+1-9 switch workspace
        if (event->modifiers() == Qt::ControlModifier &&
            event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
            int idx = event->key() - Qt::Key_1;
            if (idx < (int)workspaces.size())
                tabList->setCurrentRow(idx);
            return;
        }

        // Alt+Arrow: navigate between panes
        if (event->modifiers() == Qt::AltModifier) {
            switch (event->key()) {
                case Qt::Key_Left:  navigatePane(-1, 0); return;
                case Qt::Key_Right: navigatePane(1, 0);  return;
                case Qt::Key_Up:    navigatePane(0, -1); return;
                case Qt::Key_Down:  navigatePane(0, 1);  return;
                default: break;
            }
        }

        // F11 fullscreen
        if (event->key() == Qt::Key_F11 && event->modifiers() == Qt::NoModifier) {
            if (isFullScreen()) showNormal(); else showFullScreen();
            return;
        }

        // Browser-specific shortcuts (Ctrl without Shift, only when browser focused)
        if (event->modifiers() == Qt::ControlModifier && browserTabs->isVisible()) {
            QWidget* focused = QApplication::focusWidget();
            bool browserFocused = focused && browserTabs->isAncestorOf(focused);
            if (browserFocused) {
                if (event->key() == Qt::Key_L) {
                    // Focus address bar and select all
                    QWidget* current = browserTabs->currentWidget();
                    if (current) {
                        auto* urlBar = current->findChild<QLineEdit*>();
                        if (urlBar) {
                            urlBar->setFocus();
                            urlBar->selectAll();
                        }
                    }
                    return;
                }
                if (event->key() == Qt::Key_W) {
                    // Close current browser tab
                    int idx = browserTabs->currentIndex();
                    if (browserTabs->count() > 1) {
                        QWidget* w = browserTabs->widget(idx);
                        browserTabs->removeTab(idx);
                        w->deleteLater();
                    }
                    return;
                }
            }
        }

        QMainWindow::keyPressEvent(event);
    }
};

// ── Broadcast functions (defined after PrettyMuxWindow) ──

static bool isBroadcastEnabled(GhosttyWidget* source) {
    return g_window && g_window->isBroadcastForWorkspace(source);
}

static void doBroadcastKey(GhosttyWidget* source, ghostty_input_key_s ke) {
    if (!g_window) return;
    auto terms = g_window->getAllTerminalsInWorkspace(source);
    for (auto* t : terms) {
        if (t != source && t->getSurface())
            ghostty_surface_key((ghostty_surface_t)t->getSurface(), ke);
    }
}

static void doBroadcastText(GhosttyWidget* source, const char* text, size_t len) {
    if (!g_window) return;
    auto terms = g_window->getAllTerminalsInWorkspace(source);
    for (auto* t : terms) {
        if (t != source && t->getSurface())
            ghostty_surface_text((ghostty_surface_t)t->getSurface(), text, len);
    }
}

// ── Action callback (after PrettyMuxWindow is defined) ──

static bool action_cb(ghostty_app_t, ghostty_target_s target, ghostty_action_s action) {
    if (!g_window) return false;

    // Extract surface pointer to identify which workspace this came from
    quint64 sp = 0;
    if (target.tag == GHOSTTY_TARGET_SURFACE)
        sp = (quint64)target.target.surface;

    if (action.tag == GHOSTTY_ACTION_OPEN_URL) {
        auto open_url = action.action.open_url;
        if (open_url.url && open_url.len > 0) {
            QString url = QString::fromUtf8(open_url.url, open_url.len);
            QMetaObject::invokeMethod(g_window, "openUrlInBrowser",
                Qt::QueuedConnection, Q_ARG(QString, url));
            return true;
        }
    }

    if (action.tag == GHOSTTY_ACTION_DESKTOP_NOTIFICATION) {
        auto notif = action.action.desktop_notification;
        QString title = notif.title ? QString::fromUtf8(notif.title) : "";
        QString body = notif.body ? QString::fromUtf8(notif.body) : "";
        QMetaObject::invokeMethod(g_window, "showNotificationFor",
            Qt::QueuedConnection, Q_ARG(quint64, sp),
            Q_ARG(QString, title), Q_ARG(QString, body));
        return true;
    }

    if (action.tag == GHOSTTY_ACTION_RING_BELL) {
        QMetaObject::invokeMethod(g_window, "showBellFor",
            Qt::QueuedConnection, Q_ARG(quint64, sp));
        return true;
    }

    if (action.tag == GHOSTTY_ACTION_SET_TITLE) {
        auto t = action.action.set_title;
        if (t.title) {
            QString title = QString::fromUtf8(t.title);
            QMetaObject::invokeMethod(g_window, "updateWorkspaceTitle",
                Qt::QueuedConnection, Q_ARG(quint64, sp), Q_ARG(QString, title));
        }
        return true;
    }

    if (action.tag == GHOSTTY_ACTION_PWD) {
        auto p = action.action.pwd;
        if (p.pwd) {
            QString pwd = QString::fromUtf8(p.pwd);
            QMetaObject::invokeMethod(g_window, "updateWorkspaceCwd",
                Qt::QueuedConnection, Q_ARG(quint64, sp), Q_ARG(QString, pwd));
        }
        return true;
    }

    if (action.tag == GHOSTTY_ACTION_COMMAND_FINISHED) {
        auto cmd = action.action.command_finished;
        double secs = cmd.duration / 1000000000.0;
        if (secs > 3.0) {
            QString body = cmd.exit_code == 0
                ? QString("Command completed in %1s").arg(secs, 0, 'f', 1)
                : QString("Command failed (exit %1) after %2s").arg(cmd.exit_code).arg(secs, 0, 'f', 1);
            QMetaObject::invokeMethod(g_window, "showNotificationFor",
                Qt::QueuedConnection, Q_ARG(quint64, sp),
                Q_ARG(QString, QString("prettymux")),
                Q_ARG(QString, body));
        }
        return true;
    }

    if (action.tag == GHOSTTY_ACTION_SHOW_CHILD_EXITED) {
        auto child = action.action.child_exited;
        QString body = QString("Process exited with code %1").arg(child.exit_code);
        QMetaObject::invokeMethod(g_window, "showNotificationFor",
            Qt::QueuedConnection, Q_ARG(quint64, sp),
            Q_ARG(QString, QString("prettymux")),
            Q_ARG(QString, body));
        return true;
    }

    // Activity indicator: RENDER fires when a terminal has new content
    if (action.tag == GHOSTTY_ACTION_RENDER) {
        if (sp) {
            QMetaObject::invokeMethod(g_window, "markActivity",
                Qt::QueuedConnection, Q_ARG(quint64, sp));
        }
        return true;
    }

    // Progress bar: track progress state per surface
    if (action.tag == GHOSTTY_ACTION_PROGRESS_REPORT) {
        auto pr = action.action.progress_report;
        QMetaObject::invokeMethod(g_window, "updateTerminalProgress",
            Qt::QueuedConnection, Q_ARG(quint64, sp),
            Q_ARG(int, (int)pr.state), Q_ARG(int, (int)pr.progress));
        return true;
    }

    // Search: acknowledge ghostty's built-in search
    if (action.tag == GHOSTTY_ACTION_START_SEARCH) {
        return true;
    }

    if (action.tag == GHOSTTY_ACTION_SEARCH_TOTAL) {
        return true;
    }

    if (action.tag == GHOSTTY_ACTION_SEARCH_SELECTED) {
        return true;
    }

    return false;
}

// ── Main ──

int main(int argc, char* argv[]) {
    // Force X11/XCB to get native window decorations on GNOME/Wayland
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    // Pick up GNOME/GTK text scaling and font settings
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // Read GNOME text-scaling-factor if QT_SCALE_FACTOR not already set
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        FILE* fp = popen("gsettings get org.gnome.desktop.interface text-scaling-factor 2>/dev/null", "r");
        if (fp) {
            char buf[32];
            if (fgets(buf, sizeof(buf), fp)) {
                double scale = atof(buf);
                if (scale > 0.5 && scale < 4.0) {
                    char env[32];
                    snprintf(env, sizeof(env), "%.2f", scale);
                    qputenv("QT_SCALE_FACTOR", env);
                }
            }
            pclose(fp);
        }
    }

    QApplication app(argc, argv);

    // Read GNOME system font
    {
        FILE* fp = popen("gsettings get org.gnome.desktop.interface font-name 2>/dev/null", "r");
        if (fp) {
            char buf[128];
            if (fgets(buf, sizeof(buf), fp)) {
                // Format: 'Font Name Size'
                QString fontStr = QString::fromUtf8(buf).trimmed().remove('\'');
                // Extract size from end
                int lastSpace = fontStr.lastIndexOf(' ');
                if (lastSpace > 0) {
                    QString family = fontStr.left(lastSpace);
                    int size = fontStr.mid(lastSpace + 1).toInt();
                    if (size > 0) {
                        QFont font(family, size);
                        QApplication::setFont(font);
                    }
                }
            }
            pclose(fp);
        }
    }

    // Load saved theme or detect system preference
    {
        QFile themeFile(QDir::homePath() + "/.config/prettymux/theme.conf");
        if (themeFile.open(QIODevice::ReadOnly)) {
            QString name = QString::fromUtf8(themeFile.readAll()).trimmed();
            themeFile.close();
            for (int i = 0; i < kThemeCount; i++) {
                if (kThemes[i].name == name) { g_currentTheme = i; break; }
            }
        } else {
            // Detect system preference via gsettings
            FILE* fp2 = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
            if (fp2) {
                char buf2[64];
                if (fgets(buf2, sizeof(buf2), fp2)) {
                    QString scheme = QString::fromUtf8(buf2).trimmed().remove('\'');
                    if (scheme == "prefer-light" || scheme == "default")
                        g_currentTheme = 1; // Light theme
                }
                pclose(fp2);
            }
        }
    }

    // Apply initial palette from theme
    {
        const auto& t = currentTheme();
        QPalette palette;
        palette.setColor(QPalette::Window, QColor(t.bg));
        palette.setColor(QPalette::WindowText, QColor(t.fg));
        palette.setColor(QPalette::Base, QColor(t.surface));
        palette.setColor(QPalette::Text, QColor(t.fg));
        palette.setColor(QPalette::Button, QColor(t.overlay));
        palette.setColor(QPalette::ButtonText, QColor(t.fg));
        palette.setColor(QPalette::Highlight, QColor(t.accent));
        app.setPalette(palette);
    }

    // Init ghostty
    if (ghostty_init(0, nullptr) != 0) {
        fprintf(stderr, "ghostty_init failed\n");
        return 1;
    }

    ghostty_config_t config = ghostty_config_new();
    ghostty_config_load_default_files(config);
    ghostty_config_finalize(config);

    ghostty_runtime_config_s rc = {};
    rc.wakeup_cb = wakeup_cb;
    rc.action_cb = action_cb;
    rc.read_clipboard_cb = read_clipboard_cb;
    rc.confirm_read_clipboard_cb = confirm_read_clipboard_cb;
    rc.write_clipboard_cb = write_clipboard_cb;
    rc.close_surface_cb = close_surface_cb;

    g_app = ghostty_app_new(&rc, config);
    if (!g_app) {
        fprintf(stderr, "ghostty_app_new failed\n");
        return 1;
    }

    ShortcutManager shortcuts;
    g_shortcuts = &shortcuts;

    PrettyMuxWindow window;
    g_window = &window;
    window.show();

    QObject::connect(&app, &QApplication::aboutToQuit, [&]() {
        QLocalServer::removeServer(window.socketPath);
        g_window = nullptr;
    });

    return app.exec();
}

#include "main.moc"
