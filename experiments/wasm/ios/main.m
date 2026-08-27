// A WKWebView and nothing else.
//
// The browser build is the product; this is the shell the plan says an iOS app
// would be. It ships no runtime, no renderer and no game -- it points a web view
// at the page and gets out of the way, which is also what makes it useful as a
// measuring instrument: anything that fails here fails in WebKit, not in a
// native layer this app added.
//
// The URL comes from the DOLWEB_URL environment variable (simctl passes it
// straight through), falling back to the Info.plist key of the same name. In
// the Simulator, 127.0.0.1 is the *Mac's* loopback, so `serve.py game` on the
// host is reachable with no extra plumbing.

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

@interface DolWebViewController : UIViewController <WKNavigationDelegate, WKScriptMessageHandler>
@property(nonatomic, strong) WKWebView *webView;
@property(nonatomic, strong) UILabel *status;
@end

@implementation DolWebViewController

- (NSString *)pageURLString {
    NSString *env = NSProcessInfo.processInfo.environment[@"DOLWEB_URL"];
    if (env.length) return env;
    NSString *plist = NSBundle.mainBundle.infoDictionary[@"DOLWEB_URL"];
    if (plist.length) return plist;
    return @"http://127.0.0.1:8712/";
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    WKWebViewConfiguration *config = [WKWebViewConfiguration new];
    config.allowsInlineMediaPlayback = YES;
    // The page starts audio behind a user gesture of its own, so do not make
    // WebKit demand a second one.
    config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    config.limitsNavigationsToAppBoundDomains = NO;
    // Console output is the only channel a headless-ish run has; forward it.
    NSString *hook =
        @"(function(){const s=(k)=>{const o=console[k].bind(console);"
        @"console[k]=(...a)=>{try{window.webkit.messageHandlers.dolweb.postMessage("
        @"k+': '+a.map(String).join(' '));}catch(e){}o(...a);};};"
        @"['log','warn','error'].forEach(s);"
        @"window.addEventListener('error',e=>{try{window.webkit.messageHandlers"
        @".dolweb.postMessage('uncaught: '+e.message);}catch(_){}});})();";
    WKUserScript *script =
        [[WKUserScript alloc] initWithSource:hook
                               injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                            forMainFrameOnly:YES];
    [config.userContentController addUserScript:script];
    [config.userContentController addScriptMessageHandler:self name:@"dolweb"];

    self.webView = [[WKWebView alloc] initWithFrame:self.view.bounds configuration:config];
    self.webView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.webView.navigationDelegate = self;
    self.webView.scrollView.bounces = NO;
    self.webView.opaque = NO;
    self.webView.backgroundColor = UIColor.blackColor;
    if (@available(iOS 16.4, *)) self.webView.inspectable = YES;
    [self.view addSubview:self.webView];

    self.status = [[UILabel alloc] initWithFrame:CGRectMake(12, 48, self.view.bounds.size.width - 24, 60)];
    self.status.numberOfLines = 3;
    self.status.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.status.textColor = [UIColor colorWithWhite:0.7 alpha:1.0];
    self.status.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.view addSubview:self.status];

    NSString *url = [self pageURLString];
    self.status.text = [NSString stringWithFormat:@"loading %@", url];
    NSLog(@"[dolweb] loading %@", url);
    [self.webView loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
}

- (void)userContentController:(WKUserContentController *)ucc
      didReceiveScriptMessage:(WKScriptMessage *)message {
    NSLog(@"[page] %@", message.body);
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)nav {
    NSLog(@"[dolweb] navigation finished");
    // Report what this WebKit actually offers. requestAdapter() is the one that
    // matters and the one the Simulator fails: it exposes navigator.gpu and then
    // hands back null.
    NSString *probe =
        @"(async () => {"
        @"  let adapter = 'no navigator.gpu';"
        @"  if (navigator.gpu) {"
        @"    try { const a = await navigator.gpu.requestAdapter();"
        @"          adapter = a ? ('adapter: ' + JSON.stringify(a.info || {})) : 'requestAdapter() null'; }"
        @"    catch (e) { adapter = 'requestAdapter threw: ' + e; }"
        @"  }"
        @"  return [adapter,"
        @"          'wasm=' + (typeof WebAssembly),"
        @"          'sab=' + (typeof SharedArrayBuffer),"
        @"          'coi=' + self.crossOriginIsolated,"
        @"          'dpr=' + devicePixelRatio,"
        @"          'mem=' + (navigator.deviceMemory || '?')].join('  ');"
        @"})()";
    [webView callAsyncJavaScript:probe
                       arguments:nil
                         inFrame:nil
                  inContentWorld:WKContentWorld.pageWorld
               completionHandler:^(id result, NSError *error) {
        NSString *text = error ? error.localizedDescription : [result description];
        NSLog(@"[dolweb] capabilities: %@", text);
        self.status.text = text;
    }];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)nav
      withError:(NSError *)error {
    NSLog(@"[dolweb] load failed: %@", error.localizedDescription);
    self.status.text = [NSString stringWithFormat:@"load failed: %@", error.localizedDescription];
}

- (BOOL)prefersStatusBarHidden { return YES; }
@end

@interface DolWebAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation DolWebAppDelegate
- (BOOL)application:(UIApplication *)app
    didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [DolWebViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil,
                                 NSStringFromClass(DolWebAppDelegate.class));
    }
}
