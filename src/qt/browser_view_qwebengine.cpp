#include "browser_view.h"

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)

#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

class PrettyMuxPage final : public QWebEnginePage {
public:
    explicit PrettyMuxPage(BrowserView* owner, QObject* parent = nullptr)
        : QWebEnginePage(parent), owner_(owner) {}

    QWebEnginePage* createWindow(WebWindowType) override
    {
        auto* tempPage = new QWebEnginePage(this);
        QObject::connect(tempPage, &QWebEnginePage::urlChanged, owner_,
                         [this, tempPage](const QUrl& url) {
                             if (!url.isEmpty() && url.toString() != "about:blank")
                                 emit owner_->newTabRequested(url);
                             tempPage->deleteLater();
                         });
        return tempPage;
    }

private:
    BrowserView* owner_;
};

class QWebEngineBrowserView final : public BrowserView {
public:
    explicit QWebEngineBrowserView(QWidget* parent = nullptr)
        : BrowserView(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        view_ = new QWebEngineView(this);
        auto* page = new PrettyMuxPage(this, view_);
        view_->setPage(page);
        view_->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        view_->setContextMenuPolicy(Qt::DefaultContextMenu);
        page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        layout->addWidget(view_);

        connect(view_, &QWebEngineView::urlChanged, this, &BrowserView::urlChanged);
        connect(view_, &QWebEngineView::titleChanged, this, &BrowserView::titleChanged);
    }

    void navigate(const QUrl& url) override { view_->setUrl(url); }
    void goBack() override { view_->back(); }
    void goForward() override { view_->forward(); }
    void reloadPage() override { view_->reload(); }
    QUrl currentUrl() const override { return view_->url(); }
    QString currentTitle() const override { return view_->title(); }
    bool supportsDevTools() const override { return true; }

    void showDevTools(bool inWindow) override
    {
        auto* devToolsView = new QWebEngineView();
        auto* devToolsPage = new QWebEnginePage(devToolsView);
        devToolsView->setPage(devToolsPage);
        view_->page()->setDevToolsPage(devToolsPage);

        if (inWindow) {
            devToolsView->setAttribute(Qt::WA_DeleteOnClose);
            devToolsView->setWindowTitle("Developer Tools - prettymux");
            devToolsView->resize(900, 600);
            devToolsView->show();
            return;
        }

        auto* dock = new QWidget(this, Qt::Tool);
        dock->setAttribute(Qt::WA_DeleteOnClose);
        dock->setWindowTitle("Developer Tools - prettymux");
        auto* layout = new QVBoxLayout(dock);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(devToolsView);
        dock->resize(900, 600);
        dock->show();
    }

private:
    QWebEngineView* view_ = nullptr;
};

BrowserView* createBrowserView(QWidget* parent)
{
    return new QWebEngineBrowserView(parent);
}

#endif
