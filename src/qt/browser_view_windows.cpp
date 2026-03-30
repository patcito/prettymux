#include "browser_view.h"

#ifdef Q_OS_WIN

#include <QEvent>

#include <string>
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

class WebView2BrowserView final : public BrowserView {
public:
    explicit WebView2BrowserView(QWidget* parent = nullptr)
        : BrowserView(parent)
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
    }

    ~WebView2BrowserView() override
    {
        if (controller_)
            controller_->Close();
    }

    void navigate(const QUrl& url) override
    {
        pendingUrl_ = url;
        ensureController();
        if (webview_)
            navigateNow(pendingUrl_);
    }

    void goBack() override
    {
        ensureController();
        if (webview_)
            webview_->GoBack();
    }

    void goForward() override
    {
        ensureController();
        if (webview_)
            webview_->GoForward();
    }

    void reloadPage() override
    {
        ensureController();
        if (webview_)
            webview_->Reload();
    }

    QUrl currentUrl() const override { return currentUrl_; }
    QString currentTitle() const override { return currentTitle_; }

protected:
    bool event(QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Resize:
        case QEvent::WinIdChange:
        case QEvent::ParentChange:
            ensureController();
            updateBounds();
            updateVisibility();
            break;
        case QEvent::Hide:
            updateVisibility();
            break;
        default:
            break;
        }

        return BrowserView::event(event);
    }

private:
    void ensureController()
    {
        if (creatingController_ || controller_)
            return;

        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd)
            return;

        creatingController_ = true;
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            nullptr,
            nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, hwnd](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    creatingController_ = false;
                    if (FAILED(result) || !environment)
                        return result;

                    environment_ = environment;
                    return environment->CreateCoreWebView2Controller(
                        hwnd,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(controllerResult) || !controller)
                                    return controllerResult;

                                controller_ = controller;
                                controller_->get_CoreWebView2(&webview_);
                                configureWebView();
                                updateBounds();
                                updateVisibility();
                                if (pendingUrl_.isValid() && !pendingUrl_.isEmpty())
                                    navigateNow(pendingUrl_);
                                return S_OK;
                            })
                            .Get());
                })
                .Get());
        if (FAILED(hr))
            creatingController_ = false;
    }

    void configureWebView()
    {
        if (!webview_)
            return;

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDefaultContextMenusEnabled(TRUE);
            settings->put_AreDevToolsEnabled(FALSE);
        }

        webview_->add_SourceChanged(
            Callback<ICoreWebView2SourceChangedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                    updateCurrentUrl();
                    return S_OK;
                })
                .Get(),
            &sourceChangedToken_);

        webview_->add_DocumentTitleChanged(
            Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [this](ICoreWebView2*, IUnknown*) -> HRESULT {
                    updateCurrentTitle();
                    return S_OK;
                })
                .Get(),
            &titleChangedToken_);

        webview_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    LPWSTR uri = nullptr;
                    if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                        QString nextUrl = QString::fromWCharArray(uri);
                        emit newTabRequested(QUrl(nextUrl));
                        CoTaskMemFree(uri);
                    }
                    args->put_Handled(TRUE);
                    return S_OK;
                })
                .Get(),
            &newWindowToken_);
    }

    void navigateNow(const QUrl& url)
    {
        if (!webview_)
            return;

        std::wstring wide = url.toString().toStdWString();
        webview_->Navigate(wide.c_str());
    }

    void updateBounds()
    {
        if (!controller_)
            return;

        RECT bounds{0, 0, width(), height()};
        controller_->put_Bounds(bounds);
    }

    void updateVisibility()
    {
        if (controller_)
            controller_->put_IsVisible(isVisible());
    }

    void updateCurrentUrl()
    {
        if (!webview_)
            return;

        LPWSTR uri = nullptr;
        if (FAILED(webview_->get_Source(&uri)) || !uri)
            return;

        QString nextUrl = QString::fromWCharArray(uri);
        CoTaskMemFree(uri);

        if (currentUrl_.toString() != nextUrl) {
            currentUrl_ = QUrl(nextUrl);
            emit urlChanged(currentUrl_);
        }
    }

    void updateCurrentTitle()
    {
        if (!webview_)
            return;

        LPWSTR title = nullptr;
        if (FAILED(webview_->get_DocumentTitle(&title)) || !title)
            return;

        QString nextTitle = QString::fromWCharArray(title);
        CoTaskMemFree(title);

        if (currentTitle_ != nextTitle) {
            currentTitle_ = nextTitle;
            emit titleChanged(currentTitle_);
        }
    }

    bool creatingController_ = false;
    QUrl pendingUrl_;
    QUrl currentUrl_;
    QString currentTitle_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    EventRegistrationToken sourceChangedToken_{};
    EventRegistrationToken titleChangedToken_{};
    EventRegistrationToken newWindowToken_{};
};

BrowserView* createBrowserView(QWidget* parent)
{
    return new WebView2BrowserView(parent);
}

#endif
