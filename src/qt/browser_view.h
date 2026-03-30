#pragma once

#include <QString>
#include <QUrl>
#include <QWidget>

class BrowserView : public QWidget {
    Q_OBJECT
public:
    explicit BrowserView(QWidget* parent = nullptr) : QWidget(parent) {}
    ~BrowserView() override = default;

    virtual void navigate(const QUrl& url) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reloadPage() = 0;
    virtual QUrl currentUrl() const = 0;
    virtual QString currentTitle() const = 0;
    virtual bool supportsDevTools() const { return false; }
    virtual void showDevTools(bool inWindow) { Q_UNUSED(inWindow); }

signals:
    void urlChanged(const QUrl& url);
    void titleChanged(const QString& title);
    void newTabRequested(const QUrl& url);
};

BrowserView* createBrowserView(QWidget* parent = nullptr);
