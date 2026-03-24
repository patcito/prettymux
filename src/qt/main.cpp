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
#include <QMenu>
#include <QWidgetAction>
#include <QPainter>
#include <QCloseEvent>
#include <QJsonArray>
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
class PrettyMuxWindow; // forward declaration

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

        // Ctrl+Shift+T: intercept before ghostty (ghostty binds it to new_tab)
        if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_T) {
            QMetaObject::invokeMethod(window(), "addTabInFocusedPane", Qt::QueuedConnection);
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
            "QTabBar::tab { background: #181825; color: #6c7086; padding: 4px 10px; border: none; border-bottom: 2px solid transparent; }"
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
            b->setStyleSheet("background: #313244; color: #6c7086; border: none; border-radius: 3px; padding: 0 6px;");
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

signals:
    void splitRequested(PaneWidget* source, Qt::Orientation orientation);
};

// ── Workspace: vertical sidebar tab, contains panes arranged in splitters ──

struct Workspace {
    QString name;
    bool userRenamed = false;      // true if user manually renamed this workspace
    QWidget* container;            // top-level widget (either PaneWidget or QSplitter)
    std::vector<PaneWidget*> panes; // all panes (flat list for lookup)
    QString cwd;
    QString title;
    QString gitBranch;
    QString notification;
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
        title->setStyleSheet("color: #cba6f7; font-weight: bold;");
        headerLayout->addWidget(title);
        headerLayout->addStretch();

        // Bell button with notification count badge
        bellBtn = new QPushButton();
        bellBtn->setFixedSize(28, 28);
        bellBtn->setToolTip("Notifications");
        bellBtn->setIcon(QIcon::fromTheme("notification-symbolic", QIcon::fromTheme("preferences-system-notifications")));
        bellBtn->setStyleSheet("background: #313244; border: none; border-radius: 4px;");
        connect(bellBtn, &QPushButton::clicked, this, &PrettyMuxWindow::showNotificationDropdown);
        headerLayout->addWidget(bellBtn);

        QPushButton* addBtn = new QPushButton("+");
        addBtn->setFixedSize(24, 24);
        addBtn->setStyleSheet("background: #313244; color: #cdd6f4; border: none; border-radius: 4px;");
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
            "QListWidget::item { color: #a6adc8; padding: 10px 12px; border-left: 3px solid transparent; }"
            "QListWidget::item:selected { background: #1e3a5f; border-left: 3px solid #89b4fa; color: #cdd6f4; }"
            "QListWidget::item:hover { background: #1e1e2e; }"
        );
        tabList->setEditTriggers(QAbstractItemView::DoubleClicked);
        tabList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            auto* item = tabList->itemAt(pos);
            if (!item) return;
            int idx = tabList->row(item);
            QMenu menu;
            menu.setStyleSheet(
                "QMenu { background: #1e1e2e; border: 1px solid #313244; }"
                "QMenu::item { color: #cdd6f4; padding: 6px 16px; }"
                "QMenu::item:selected { background: #313244; }"
            );
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
        setStyleSheet("QMainWindow { background: #1e1e2e; } QToolTip { color: #cdd6f4; background: #313244; border: 1px solid #45475a; padding: 4px; }");

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
        QMenu* menu = new QMenu(this);
        menu->setStyleSheet(
            "QMenu { background: #1e1e2e; border: 1px solid #313244; padding: 4px; min-width: 300px; }"
            "QMenu::item { color: #cdd6f4; padding: 8px 12px; }"
            "QMenu::item:selected { background: #313244; }"
            "QMenu::item:disabled { color: #45475a; }"
            "QMenu::separator { background: #313244; height: 1px; margin: 4px 8px; }"
        );

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
