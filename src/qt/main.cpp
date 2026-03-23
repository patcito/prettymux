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
#include <QSystemTrayIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
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

class PaneWidget; // forward declaration

// ── GhosttyWidget — QOpenGLWidget hosting a ghostty surface ──

class GhosttyWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

    ghostty_surface_t surface = nullptr;
    QTimer* tickTimer = nullptr;
    QByteArray m_textBuf;
    bool m_exitEmitted = false;

public:
    GhosttyWidget(QWidget* parent = nullptr) : QOpenGLWidget(parent) {
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
        config.working_directory = getenv("HOME");

        // Pass BROWSER and PRETTYMUX env vars so child processes
        // open URLs in the embedded browser
        static ghostty_env_var_s env_vars[2];
        static QByteArray socket_val = qgetenv("PRETTYMUX_SOCKET");
        env_vars[0] = { "PRETTYMUX_SOCKET", socket_val.constData() };
        env_vars[1] = { "PRETTYMUX", "1" };
        config.env_vars = env_vars;
        config.env_var_count = 2;

        // Inject shell functions that override open/xdg-open
        static QByteArray initInput;
        initInput = QByteArray(
            "xdg-open() { case \"$1\" in http://*|https://*) "
            "echo '{\"command\":\"browser.open\",\"url\":\"'\"$1\"'\"}' "
            "| socat - UNIX-CONNECT:\"$PRETTYMUX_SOCKET\" 2>/dev/null && return;; esac; "
            "/usr/bin/xdg-open \"$@\"; }; "
            "open() { xdg-open \"$@\"; }; clear\n"
        );
        config.initial_input = initInput.constData();

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

        // Ctrl+Shift+T: intercept before ghostty (ghostty binds it to new_tab)
        if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_T) {
            event->ignore();
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

        // Ctrl++: increase font size
        if (event->modifiers() == Qt::ControlModifier && (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)) {
            ghostty_surface_binding_action(surface, "increase_font_size:1", 21);
            update();
            return;
        }

        // Ctrl+-: decrease font size
        if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_Minus) {
            ghostty_surface_binding_action(surface, "decrease_font_size:1", 21);
            update();
            return;
        }

        // Ctrl+0: reset font size
        if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_0) {
            ghostty_surface_binding_action(surface, "reset_font_size", 15);
            update();
            return;
        }

        // Ctrl+Shift+V or Ctrl+V: paste
        if ((event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_V) ||
            (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_V)) {
            QString text = QApplication::clipboard()->text();
            if (!text.isEmpty()) {
                QByteArray utf8 = text.toUtf8();
                ghostty_surface_text(surface, utf8.constData(), utf8.size());
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

        // Only set text for plain printable keys, not modifier combos.
        // For Ctrl+key combos, ghostty handles encoding internally
        // based on the keycode and mods.
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            QByteArray utf8 = event->text().toUtf8();
            if (!utf8.isEmpty() && utf8[0] >= 0x20) {
                m_textBuf = utf8;
                m_textBuf.append('\0');
                ke.text = m_textBuf.constData();
            }
        }

        ke.unshifted_codepoint = 0;

        ghostty_surface_key(surface, ke);
        update();
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (!surface) return;
        ghostty_input_key_s ke = {};
        ke.action = GHOSTTY_ACTION_RELEASE;
        ke.keycode = event->nativeScanCode();
        ke.mods = translateMods(event->modifiers());
        ghostty_surface_key(surface, ke);
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
        if (!utf8.isEmpty())
            ghostty_surface_text(surface, utf8.constData(), utf8.size());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!surface) return;
        setFocus();
        ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_PRESS,
            translateButton(event->button()), translateMods(event->modifiers()));
        ghostty_surface_mouse_pos(surface,
            event->position().x() * devicePixelRatioF(),
            event->position().y() * devicePixelRatioF(),
            translateMods(event->modifiers()));
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!surface) return;
        ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_RELEASE,
            translateButton(event->button()), translateMods(event->modifiers()));
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!surface) return;
        ghostty_surface_mouse_pos(surface,
            event->position().x() * devicePixelRatioF(),
            event->position().y() * devicePixelRatioF(),
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

// ── PaneWidget: a single pane with tab bar, each tab is one ghostty terminal ──
// Split buttons emit splitRequested so the parent can replace this pane
// with a splitter containing this pane + a new pane.

class PaneWidget : public QWidget {
    Q_OBJECT
public:
    QTabWidget* tabs;
    std::vector<GhosttyWidget*> terminals; // one per tab

    PaneWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setStyleSheet("border: 2px solid #313244; border-radius: 0px;");
        setMinimumSize(100, 100);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        tabs = new QTabWidget();
        tabs->setTabsClosable(true);
        tabs->setMovable(true);
        tabs->setStyleSheet(
            "QTabWidget::pane { border: none; background: #1e1e2e; }"
            "QTabBar::tab { background: #181825; color: #6c7086; padding: 4px 10px; border: none; border-bottom: 2px solid transparent; font-size: 12px; }"
            "QTabBar::tab:selected { color: #cdd6f4; border-bottom: 2px solid #a6e3a1; background: #1e1e2e; }"
            "QTabBar::tab:hover { color: #cdd6f4; background: #313244; }"
        );

        // Corner buttons
        QWidget* corner = new QWidget();
        auto* cl = new QHBoxLayout(corner);
        cl->setContentsMargins(2, 2, 2, 2);
        cl->setSpacing(2);

        auto btn = [](const QString& text) {
            auto* b = new QPushButton(text);
            b->setFixedHeight(20);
            b->setStyleSheet("background: #313244; color: #6c7086; border: none; border-radius: 3px; padding: 0 6px; font-size: 11px;");
            return b;
        };

        auto* addBtn = btn("+");
        auto* splitH = btn("⬌");
        auto* splitV = btn("⬍");

        connect(addBtn, &QPushButton::clicked, this, &PaneWidget::addNewTab);
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
    }

    void addNewTab() {
        auto* term = new GhosttyWidget();
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

signals:
    void splitRequested(PaneWidget* source, Qt::Orientation orientation);
};

// ── Workspace: vertical sidebar tab, contains panes arranged in splitters ──

struct Workspace {
    QString name;
    QWidget* container;            // top-level widget (either PaneWidget or QSplitter)
    std::vector<PaneWidget*> panes; // all panes (flat list for lookup)
    QString cwd;
    QString title;
    QString gitBranch;
    QString notification;
};

// ── MainWindow ──

class PrettyMuxWindow : public QMainWindow {
    Q_OBJECT

public:
    QString socketPath;
private:
    QListWidget* tabList;
    QStackedWidget* terminalStack;
    QTabWidget* browserTabs;
    QSplitter* mainSplitter;
    QSystemTrayIcon* trayIcon = nullptr;
    QLocalServer* socketServer = nullptr;
    std::vector<Workspace> workspaces;
    int activeWorkspace = -1;

public:
    PrettyMuxWindow() {
        setWindowTitle("prettymux");
        resize(1400, 900);

        // Central widget
        QWidget* central = new QWidget(this);
        QSplitter* outerSplitter = new QSplitter(Qt::Horizontal, central);
        outerSplitter->setStyleSheet("QSplitter::handle { background: #313244; width: 3px; }");
        QHBoxLayout* mainLayout = new QHBoxLayout(central); mainLayout->setContentsMargins(0,0,0,0); mainLayout->setSpacing(0); mainLayout->addWidget(outerSplitter);

        // ── Sidebar ──
        QWidget* sidebar = new QWidget();
        sidebar->setMinimumWidth(120); sidebar->setMaximumWidth(400);
        sidebar->setStyleSheet("background: #181825; border-right: 1px solid #313244;");

        QVBoxLayout* sidebarLayout = new QVBoxLayout(sidebar);
        sidebarLayout->setContentsMargins(0, 0, 0, 0);
        sidebarLayout->setSpacing(0);

        // Header
        QWidget* header = new QWidget();
        header->setStyleSheet("background: #181825; border-bottom: 1px solid #313244; padding: 8px;");
        QHBoxLayout* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(12, 8, 12, 8);

        QLabel* title = new QLabel("prettymux");
        title->setStyleSheet("color: #cba6f7; font-weight: bold; font-size: 14px;");
        headerLayout->addWidget(title);
        headerLayout->addStretch();

        QPushButton* addBtn = new QPushButton("+");
        addBtn->setFixedSize(24, 24);
        addBtn->setStyleSheet("background: #313244; color: #cdd6f4; border: none; border-radius: 4px; font-size: 16px;");
        connect(addBtn, &QPushButton::clicked, this, &PrettyMuxWindow::addWorkspace);
        headerLayout->addWidget(addBtn);

        sidebarLayout->addWidget(header);

        // Search
        QLineEdit* searchBox = new QLineEdit();
        searchBox->setPlaceholderText("Search workspaces...");
        searchBox->setStyleSheet(
            "background: #313244; color: #cdd6f4; border: none; border-radius: 4px;"
            "padding: 6px 10px; margin: 6px 8px; selection-background-color: #585b70;"
        );
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
        tabList->setStyleSheet(
            "QListWidget { background: #181825; border: none; outline: none; }"
            "QListWidget::item { color: #a6adc8; padding: 10px 12px; border-left: 3px solid transparent; font-size: 12px; }"
            "QListWidget::item:selected { background: #1e3a5f; border-left: 3px solid #89b4fa; color: #cdd6f4; }"
            "QListWidget::item:hover { background: #1e1e2e; }"
        );
        tabList->setEditTriggers(QAbstractItemView::DoubleClicked);
        connect(tabList, &QListWidget::currentRowChanged, this, &PrettyMuxWindow::switchWorkspace);
        connect(tabList->itemDelegate(), &QAbstractItemDelegate::commitData, this, [this](QWidget*) {
            int row = tabList->currentRow();
            if (row >= 0 && row < (int)workspaces.size()) {
                workspaces[row].name = tabList->item(row)->text();
            }
        });
        sidebarLayout->addWidget(tabList);

        // ── Toolbar ──
        QWidget* toolbar = new QWidget();
        toolbar->setStyleSheet("background: #181825; border-top: 1px solid #313244;");
        QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbar);
        toolbarLayout->setContentsMargins(8, 4, 8, 4);

        // Split buttons removed from sidebar, they are now per-pane

        QPushButton* browserBtn = new QPushButton("Browser");
        browserBtn->setStyleSheet("background: #313244; color: #cdd6f4; border: none; border-radius: 3px; padding: 4px 8px;");
        browserBtn->setCheckable(true);
        browserBtn->setChecked(true);
        connect(browserBtn, &QPushButton::toggled, this, &PrettyMuxWindow::toggleBrowser);
        toolbarLayout->addWidget(browserBtn);

        toolbarLayout->addStretch();
        sidebarLayout->addWidget(toolbar);

        outerSplitter->addWidget(sidebar);

        // ── Main area: terminal stack + browser ──
        mainSplitter = new QSplitter(Qt::Horizontal);
        mainSplitter->setStyleSheet("QSplitter::handle { background: #313244; width: 4px; }");

        terminalStack = new QStackedWidget();
        terminalStack->setStyleSheet("background: #1e1e2e;");
        mainSplitter->addWidget(terminalStack);

        // Browser tabs
        browserTabs = new QTabWidget();
        browserTabs->setTabsClosable(true);
        browserTabs->setMovable(true);
        browserTabs->setMinimumWidth(300);
        browserTabs->setStyleSheet(
            "QTabWidget::pane { border: none; background: #1e1e2e; }"
            "QTabBar::tab { background: #181825; color: #6c7086; padding: 6px 12px; border: none; border-bottom: 2px solid transparent; }"
            "QTabBar::tab:selected { color: #cdd6f4; border-bottom: 2px solid #cba6f7; background: #1e1e2e; }"
            "QTabBar::tab:hover { color: #cdd6f4; background: #313244; }"
            "QTabBar::close-button { image: none; }"
        );

        // New tab button
        QPushButton* newTabBtn = new QPushButton("+");
        newTabBtn->setFixedSize(24, 24);
        newTabBtn->setStyleSheet("background: #313244; color: #cdd6f4; border: none; border-radius: 4px;");
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
        outerSplitter->addWidget(mainSplitter); outerSplitter->setSizes({200, 1200});

        setCentralWidget(central);

        // Dark theme
        setStyleSheet("QMainWindow { background: #1e1e2e; }");

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

        // Create first workspace
        addWorkspace();
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
        newSplitter->setStyleSheet("QSplitter::handle { background: #585b70; width: 3px; }");

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
        activeWorkspace = index;
        terminalStack->setCurrentWidget(workspaces[index].container);

        Workspace& ws = workspaces[index];
        if (!ws.panes.empty()) {
            auto terms = ws.panes[0]->allTerminals();
            if (!terms.empty()) terms[0]->setFocus();
        }

        ws.notification.clear();
        refreshSidebarItem(index);
        tabList->item(index)->setForeground(QColor("#a6adc8"));
    }

    void addBrowserTab(const QString& url) {
        // Container: address bar + webview
        QWidget* container = new QWidget();
        QVBoxLayout* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // Address bar
        QWidget* addressBar = new QWidget();
        addressBar->setFixedHeight(36);
        addressBar->setStyleSheet("background: #181825; border-bottom: 1px solid #313244;");
        QHBoxLayout* barLayout = new QHBoxLayout(addressBar);
        barLayout->setContentsMargins(4, 4, 4, 4);
        barLayout->setSpacing(4);

        QPushButton* backBtn = new QPushButton("<");
        backBtn->setFixedSize(28, 28);
        backBtn->setStyleSheet("background: #313244; color: #cdd6f4; border: none; border-radius: 4px;");

        QPushButton* fwdBtn = new QPushButton(">");
        fwdBtn->setFixedSize(28, 28);
        fwdBtn->setStyleSheet("background: #313244; color: #cdd6f4; border: none; border-radius: 4px;");

        QPushButton* reloadBtn = new QPushButton("R");
        reloadBtn->setFixedSize(28, 28);
        reloadBtn->setStyleSheet("background: #313244; color: #cdd6f4; border: none; border-radius: 4px;");

        QLineEdit* urlBar = new QLineEdit(url);
        urlBar->setStyleSheet(
            "background: #313244; color: #cdd6f4; border: none; border-radius: 4px;"
            "padding: 4px 8px; selection-background-color: #585b70;"
        );

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
        view->setUrl(QUrl(url));
        layout->addWidget(view);

        // Wire up buttons
        connect(backBtn, &QPushButton::clicked, view, &QWebEngineView::back);
        connect(fwdBtn, &QPushButton::clicked, view, &QWebEngineView::forward);
        connect(reloadBtn, &QPushButton::clicked, view, &QWebEngineView::reload);

        // Enter in URL bar navigates
        connect(urlBar, &QLineEdit::returnPressed, this, [view, urlBar]() {
            QString text = urlBar->text().trimmed();
            if (!text.startsWith("http://") && !text.startsWith("https://") && !text.startsWith("file://")) {
                if (text.contains(".") && !text.contains(" "))
                    text = "https://" + text;
                else
                    text = "https://www.google.com/search?q=" + QUrl::toPercentEncoding(text);
            }
            view->setUrl(QUrl(text));
        });

        // Update URL bar when page navigates
        connect(view, &QWebEngineView::urlChanged, this, [urlBar](const QUrl& newUrl) {
            urlBar->setText(newUrl.toString());
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

    Q_INVOKABLE void updateWorkspaceTitle(quint64 surfacePtr, const QString& title) {
        int wi = findWorkspaceForSurface((void*)surfacePtr);
        if (wi < 0 || wi >= (int)workspaces.size()) return;
        workspaces[wi].title = title; refreshSidebarItem(wi);
    }

    Q_INVOKABLE void updateWorkspaceCwd(quint64 surfacePtr, const QString& cwd) {
        int wi = findWorkspaceForSurface((void*)surfacePtr);
        if (wi < 0 || wi >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[wi];

        // Detect git branch
        QProcess git;
        git.setWorkingDirectory(cwd);
        git.start("git", {"rev-parse", "--abbrev-ref", "HEAD"});
        git.waitForFinished(500);
        if (git.exitCode() == 0) {
            ws.gitBranch = git.readAllStandardOutput().trimmed();
        } else {
            ws.gitBranch.clear();
        }

        refreshSidebarItem(wi);
    }

    void refreshSidebarItem(int idx) {
        if (idx < 0 || idx >= tabList->count()) return;
        if (idx >= (int)workspaces.size()) return;
        Workspace& ws = workspaces[idx];

        // Build rich display text
        QString text = ws.name;
        if (!ws.title.isEmpty() && ws.title != ws.name) {
            text = ws.title;
        }

        // Add metadata lines
        QStringList lines;
        lines << text;
        if (!ws.gitBranch.isEmpty()) {
            lines << QString("⎇ %1").arg(ws.gitBranch);
        }
        if (!ws.cwd.isEmpty()) {
            QString shortCwd = ws.cwd;
            QString home = QDir::homePath();
            if (shortCwd.startsWith(home))
                shortCwd = "~" + shortCwd.mid(home.length());
            lines << shortCwd;
        }
        if (!ws.notification.isEmpty()) {
            lines << ws.notification;
        }

        tabList->item(idx)->setText(lines.join("\n"));
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

    Q_INVOKABLE void openUrlInBrowser(const QString& url) {
        addBrowserTab(url);
        browserTabs->setVisible(true);
    }

    Q_INVOKABLE void showNotificationFor(quint64 surfacePtr, const QString& title, const QString& body) {
        int notifWorkspace = findWorkspaceForSurface((void*)surfacePtr);

        // Store notification and update sidebar
        if (notifWorkspace >= 0 && notifWorkspace < (int)workspaces.size()) {
            workspaces[notifWorkspace].notification = body.left(80);
            refreshSidebarItem(notifWorkspace);
            tabList->item(notifWorkspace)->setForeground(QColor("#f38ba8"));
        }

        // Use gdbus to send notification with action support
        QString t = title.isEmpty() ? "prettymux" : title;
        QString actionId = QString("focus-%1").arg(notifWorkspace);

        // Start notify process and use --action to get callback
        QProcess* proc = new QProcess(this);
        proc->setProgram("notify-send");
        proc->setArguments({
            t, body,
            "--app-name=prettymux",
            "--action=focus=Focus Terminal",
            "--wait"
        });

        connect(proc, &QProcess::finished, this, [this, proc, notifWorkspace](int exitCode) {
            // exitCode 0 with --wait means the action was clicked
            if (exitCode == 0) {
                // Raise and focus window
                raise();
                activateWindow();
                // Switch to the workspace that triggered the notification
                if (notifWorkspace >= 0 && notifWorkspace < (int)workspaces.size()) {
                    tabList->setCurrentRow(notifWorkspace);
                    Workspace& w = workspaces[notifWorkspace];
                    if (!w.panes.empty()) {
                        auto terms = w.panes[0]->allTerminals();
                        if (!terms.empty()) terms[0]->setFocus();
                    }
                }
            }
            proc->deleteLater();
        });

        proc->start();
    }

    Q_INVOKABLE void showBellFor(quint64 surfacePtr) {
        int bellWs = findWorkspaceForSurface((void*)surfacePtr); if (bellWs >= 0 && bellWs < tabList->count()) {
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

protected:
    void keyPressEvent(QKeyEvent* event) override {
        // Global shortcuts
        if (event->modifiers() == Qt::ControlModifier) {
            if (event->key() == Qt::Key_N) { addWorkspace(); return; }
            if (event->key() == Qt::Key_B) {
                browserTabs->setVisible(!browserTabs->isVisible());
                return;
            }
            if (event->key() == Qt::Key_T) {
                addBrowserTab("file:///home/pe/newnewrepos/w/yo/prettymux/src/qt/welcome.html");
                return;
            }
        }
        if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            if (event->key() == Qt::Key_T) { addTabInFocusedPane(); return; }
        }
        // Ctrl+1-9 switch workspace
        if (event->modifiers() == Qt::ControlModifier &&
            event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
            int idx = event->key() - Qt::Key_1;
            if (idx < (int)workspaces.size())
                tabList->setCurrentRow(idx);
            return;
        }
        QMainWindow::keyPressEvent(event);
    }
};

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

    return false;
}

// ── Main ──

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Dark palette
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#1e1e2e"));
    palette.setColor(QPalette::WindowText, QColor("#cdd6f4"));
    palette.setColor(QPalette::Base, QColor("#181825"));
    palette.setColor(QPalette::Text, QColor("#cdd6f4"));
    palette.setColor(QPalette::Button, QColor("#313244"));
    palette.setColor(QPalette::ButtonText, QColor("#cdd6f4"));
    palette.setColor(QPalette::Highlight, QColor("#cba6f7"));
    app.setPalette(palette);

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
