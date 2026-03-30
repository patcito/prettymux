#include "browser_view.h"

#ifdef Q_OS_MACOS

#include <QByteArray>
#include <QEvent>

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

class WKBrowserView;

@interface PrettyMuxWKDelegate : NSObject <WKNavigationDelegate, WKUIDelegate>
- (instancetype)initWithOwner:(WKBrowserView*)owner;
@end

class WKBrowserView final : public BrowserView {
public:
    explicit WKBrowserView(QWidget* parent = nullptr)
        : BrowserView(parent)
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        delegate_ = [[PrettyMuxWKDelegate alloc] initWithOwner:this];
    }

    ~WKBrowserView() override
    {
        if (webView_) {
            [webView_ setNavigationDelegate:nil];
            [webView_ setUIDelegate:nil];
            [webView_ removeFromSuperview];
            webView_ = nil;
        }
        delegate_ = nil;
    }

    void navigate(const QUrl& url) override
    {
        pendingUrl_ = url;
        ensureWebView();
        if (!webView_)
            return;

        QByteArray bytes = url.toString().toUtf8();
        NSString* urlString = [NSString stringWithUTF8String:bytes.constData()];
        NSURL* nsUrl = [NSURL URLWithString:urlString];
        if (!nsUrl)
            return;

        pendingUrl_ = QUrl();
        [webView_ loadRequest:[NSURLRequest requestWithURL:nsUrl]];
    }

    void goBack() override
    {
        ensureWebView();
        if (webView_ && [webView_ canGoBack])
            [webView_ goBack];
    }

    void goForward() override
    {
        ensureWebView();
        if (webView_ && [webView_ canGoForward])
            [webView_ goForward];
    }

    void reloadPage() override
    {
        ensureWebView();
        if (webView_)
            [webView_ reload];
    }

    QUrl currentUrl() const override { return currentUrl_; }
    QString currentTitle() const override { return currentTitle_; }

    void syncState()
    {
        if (!webView_)
            return;

        NSURL* url = [webView_ URL];
        NSString* title = [webView_ title];

        if (url) {
            QString nextUrl = QString::fromUtf8([[url absoluteString] UTF8String]);
            if (currentUrl_.toString() != nextUrl) {
                currentUrl_ = QUrl(nextUrl);
                emit urlChanged(currentUrl_);
            }
        }

        if (title) {
            QString nextTitle = QString::fromUtf8([title UTF8String]);
            if (currentTitle_ != nextTitle) {
                currentTitle_ = nextTitle;
                emit titleChanged(currentTitle_);
            }
        }
    }

    void handleNewTabRequest(NSURL* url)
    {
        if (!url)
            return;
        QString nextUrl = QString::fromUtf8([[url absoluteString] UTF8String]);
        if (!nextUrl.isEmpty())
            emit newTabRequested(QUrl(nextUrl));
    }

protected:
    bool event(QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Resize:
        case QEvent::WinIdChange:
        case QEvent::ParentChange:
            ensureWebView();
            updateNativeFrame();
            break;
        case QEvent::Hide:
            if (webView_)
                [webView_ setHidden:YES];
            break;
        default:
            break;
        }

        return BrowserView::event(event);
    }

private:
    void ensureWebView()
    {
        NSView* hostView = (__bridge NSView*)(reinterpret_cast<void*>(winId()));
        if (!hostView)
            return;

        if (!webView_) {
            WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
            webView_ = [[WKWebView alloc] initWithFrame:[hostView bounds] configuration:config];
            [webView_ setNavigationDelegate:delegate_];
            [webView_ setUIDelegate:delegate_];
            [webView_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        }

        if ([webView_ superview] != hostView) {
            [webView_ removeFromSuperview];
            [hostView addSubview:webView_];
        }

        [webView_ setHidden:NO];
        updateNativeFrame();

        if (pendingUrl_.isValid() && !pendingUrl_.isEmpty()) {
            QUrl initialUrl = pendingUrl_;
            pendingUrl_ = QUrl();
            navigate(initialUrl);
        }
    }

    void updateNativeFrame()
    {
        if (!webView_)
            return;

        NSView* hostView = (__bridge NSView*)(reinterpret_cast<void*>(winId()));
        if (!hostView)
            return;

        [webView_ setFrame:[hostView bounds]];
    }

    WKWebView* webView_ = nil;
    PrettyMuxWKDelegate* delegate_ = nil;
    QUrl pendingUrl_;
    QUrl currentUrl_;
    QString currentTitle_;
};

@implementation PrettyMuxWKDelegate {
    WKBrowserView* _owner;
}

- (instancetype)initWithOwner:(WKBrowserView*)owner
{
    self = [super init];
    if (self)
        _owner = owner;
    return self;
}

- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation
{
    (void)webView;
    (void)navigation;
    _owner->syncState();
}

- (void)webView:(WKWebView*)webView didCommitNavigation:(WKNavigation*)navigation
{
    (void)webView;
    (void)navigation;
    _owner->syncState();
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation
{
    (void)webView;
    (void)navigation;
    _owner->syncState();
}

- (nullable WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)navigationAction
                    windowFeatures:(WKWindowFeatures*)windowFeatures
{
    (void)webView;
    (void)configuration;
    (void)windowFeatures;
    _owner->handleNewTabRequest(navigationAction.request.URL);
    return nil;
}

@end

BrowserView* createBrowserView(QWidget* parent)
{
    return new WKBrowserView(parent);
}

#endif
