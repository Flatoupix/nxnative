// Mac app launcher (CFBundleExecutable of the .app that dm/mac/pack_app.py packs): sets the host's environment from the bundle, then runs the Mac host (Contents/MacOS/dmhost) as a child.
// The host is not exec'ed: macOS privacy checks (TCC) attribute a child's file access to the app that launched it, while a launcher that hands its process over (a shell script exec'ing
// dmhost) gets EPERM on every open on a removable volume, with no prompt, signed or not. The launcher forwards SIGTERM / SIGINT / SIGHUP to the host and returns its exit code.
// Bundle layout: Resources/romfs (the game's RomFS), Resources/msl/<b-chain | vk | gl> (the Mac shader libraries), Resources/aoc/<index> (DLC), Resources/pso (the pipeline archive made
// offline, handed to the host as DM_PSO_BUNDLE as the iOS app's main.m does), Resources/dm (the rewritten modules, the shader tables and pairs, font/, env.txt). Saves, the pipeline list and temporary files go to ~/Library/Application Support/nxnative/<title ID>/, the host's output to ~/Library/Logs/nxnative/<title ID>.log
// (the previous run's kept as <title ID>.prev.log). Environment switches: one KEY=VALUE per line in <Application Support folder>/env.txt, then Resources/dm/env.txt (the first one wins; neither
// replaces a variable already set), as the iOS app reads Documents/env.txt and dm/env.txt.
#include <CoreFoundation/CoreFoundation.h>
#import <Cocoa/Cocoa.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pid_t g_child;

static void forward(int sig) { if (g_child > 0) kill(g_child, sig); }

static int info_string(const char *key, char *out, size_t n) {   // a string key of the bundle's Info.plist
    CFBundleRef b = CFBundleGetMainBundle(); if (!b) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(b, k); CFRelease(k);
    return v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetCString(v, out, (CFIndex)n, kCFStringEncodingUTF8) && *out;
}

static void alert(const char *title, const char *msg) {   // a dialog (the app has no window of its own), and the same line on stderr
    fprintf(stderr, "[RED] %s\n", msg);
    char *script = "on run argv\ndisplay alert (item 1 of argv) message (item 2 of argv) as critical\nend run";
    char *argv[] = {"osascript", "-e", script, (char *)title, (char *)msg, NULL}; pid_t pid; int st;
    if (!posix_spawnp(&pid, "osascript", NULL, NULL, argv, environ)) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

static int modules_present(const char *dm) {   // the packer's inventory detects even a missing binary/table pair
    CFBundleRef b = CFBundleGetMainBundle(); if (!b) return 0;
    CFTypeRef files = CFBundleGetValueForInfoDictionaryKey(b, CFSTR("DMModuleFiles"));
    if (!files || CFGetTypeID(files) != CFArrayGetTypeID() || !CFArrayGetCount(files)) return 0;
    for (CFIndex i = 0; i < CFArrayGetCount(files); i++) {
        CFTypeRef v = CFArrayGetValueAtIndex(files, i); char name[PATH_MAX], path[PATH_MAX]; struct stat st;
        if (CFGetTypeID(v) != CFStringGetTypeID() || !CFStringGetCString(v, name, sizeof name, kCFStringEncodingUTF8) || !*name || strchr(name, '/')) return 0;
        int n = snprintf(path, sizeof path, "%s/%s", dm, name);
        if (n < 0 || (size_t)n >= sizeof path || stat(path, &st) || !S_ISREG(st.st_mode) || access(path, R_OK)) return 0;
    }
    return 1;
}

static void mkdirs(const char *path) {   // mkdir -p
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static void env_file(const char *path) {   // KEY=VALUE lines, # starts a comment; a variable already set stays
    FILE *f = fopen(path, "r"); if (!f) return;
    char ln[4096];
    while (fgets(ln, sizeof ln, f)) {
        char *s = ln; while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s); while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        char *eq = strchr(s, '=');
        if (*s == '#' || !eq || eq == s) continue;
        *eq = 0; char *k = s; for (char *t = eq - 1; t > k && (*t == ' ' || *t == '\t'); t--) *t = 0;
        if (!getenv(k)) { setenv(k, eq + 1, 0); fprintf(stderr, "launcher: env %s=%s (%s)\n", k, eq + 1, path); }
    }
    fclose(f);
}

// The Mac build has a small native launcher, rather than relying on a shell or
// an iOS-only settings screen. It deliberately only exposes switches that the
// Mac host can apply without modifying the game: window/drawable size,
// presentation rate, fullscreen and Apple's Metal HUD. Values in env.txt stay
// authoritative; the other choices are kept in the app's standard preferences.
static NSString *pref_string(NSString *key, NSString *fallback) {
    NSString *v = [[NSUserDefaults standardUserDefaults] stringForKey:key];
    return v.length ? v : fallback;
}

@interface DmLauncherSettings : NSObject <NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSPopUpButton *resolution;
@property(nonatomic, strong) NSPopUpButton *fps;
@property(nonatomic, strong) NSButton *fullscreen;
@property(nonatomic, strong) NSButton *metalHud;
@property(nonatomic) BOOL cancelled;
@property(nonatomic, copy) NSString *title;
@property(nonatomic) BOOL lockResolution;
@property(nonatomic) BOOL lockFps;
@property(nonatomic) BOOL lockFullscreen;
@property(nonatomic) BOOL lockHud;
- (instancetype)initWithTitle:(NSString *)title;
- (BOOL)run;
- (void)launch:(id)sender;
- (void)reset:(id)sender;
- (void)cancel:(id)sender;
@end

@implementation DmLauncherSettings

- (NSTextField *)label:(NSString *)text frame:(NSRect)frame note:(BOOL)note {
    NSTextField *field = [NSTextField labelWithString:text];
    field.frame = frame;
    field.font = note ? [NSFont systemFontOfSize:11] : [NSFont systemFontOfSize:13];
    if (note) field.textColor = [NSColor secondaryLabelColor];
    return field;
}

- (instancetype)initWithTitle:(NSString *)title {
    if ((self = [super init])) {
        _title = [title copy];
        _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 350)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
        _window.title = [NSString stringWithFormat:@"%@ — Réglages", title];
        _window.delegate = self;
        _window.releasedWhenClosed = NO;
        NSView *content = _window.contentView;
        [content addSubview:[self label:@"Réglages de lancement" frame:NSMakeRect(32, 296, 450, 28) note:NO]];
        [content addSubview:[self label:@"Ces choix sont mémorisés pour les prochains lancements." frame:NSMakeRect(32, 270, 450, 18) note:YES]];

        [content addSubview:[self label:@"Résolution" frame:NSMakeRect(32, 222, 145, 22) note:NO]];
        _resolution = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(190, 217, 285, 30) pullsDown:NO];
        [_resolution addItemsWithTitles:@[@"1280 × 720", @"1600 × 900", @"1920 × 1080"]];
        [content addSubview:_resolution];

        [content addSubview:[self label:@"Framerate" frame:NSMakeRect(32, 176, 145, 22) note:NO]];
        _fps = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(190, 171, 285, 30) pullsDown:NO];
        [_fps addItemsWithTitles:@[@"30 fps", @"60 fps"]];
        [content addSubview:_fps];

        _fullscreen = [NSButton checkboxWithTitle:@"Plein écran" target:nil action:nil];
        _fullscreen.frame = NSMakeRect(32, 130, 215, 24);
        [content addSubview:_fullscreen];
        _metalHud = [NSButton checkboxWithTitle:@"Afficher les statistiques Metal" target:nil action:nil];
        _metalHud.frame = NSMakeRect(32, 96, 330, 24);
        [content addSubview:_metalHud];
        [content addSubview:[self label:@"La résolution correspond à la taille de la fenêtre et du rendu. Le plein écran utilise la résolution de l’écran." frame:NSMakeRect(32, 54, 455, 32) note:YES]];

        NSButton *reset = [NSButton buttonWithTitle:@"Réinitialiser" target:self action:@selector(reset:)];
        reset.frame = NSMakeRect(32, 16, 115, 30);
        [content addSubview:reset];
        NSButton *cancel = [NSButton buttonWithTitle:@"Quitter" target:self action:@selector(cancel:)];
        cancel.frame = NSMakeRect(320, 16, 75, 30);
        [content addSubview:cancel];
        NSButton *launch = [NSButton buttonWithTitle:@"Lancer" target:self action:@selector(launch:)];
        launch.frame = NSMakeRect(405, 16, 80, 30);
        launch.keyEquivalent = @"\r";
        [content addSubview:launch];

        NSString *resolution = pref_string(@"mac.resolution", @"1280x720");
        NSArray *resolutions = @[@"1280x720", @"1600x900", @"1920x1080"];
        const char *rw = getenv("DM_WINDOW_WIDTH"), *rh = getenv("DM_WINDOW_HEIGHT");
        if (rw && rh) resolution = [NSString stringWithFormat:@"%sx%s", rw, rh];
        NSInteger ri = [resolutions indexOfObject:resolution];
        [_resolution selectItemAtIndex:ri == NSNotFound ? 0 : ri];
        const char *fe = getenv("DM_FPS_TARGET");
        NSString *fps = fe ? [NSString stringWithUTF8String:fe] : pref_string(@"mac.fps", @"60");
        [_fps selectItemAtIndex:[fps intValue] == 30 ? 0 : 1];
        const char *full = getenv("DM_FULLSCREEN");
        [_fullscreen setState:(full ? atoi(full) : [pref_string(@"mac.fullscreen", @"0") intValue]) ? NSControlStateValueOn : NSControlStateValueOff];
        const char *hud = getenv("DM_METAL_HUD");
        [_metalHud setState:(hud ? atoi(hud) : [pref_string(@"mac.metalHud", @"0") intValue]) ? NSControlStateValueOn : NSControlStateValueOff];
        _lockResolution = rw && rh;
        _lockFps = fe || getenv("DM_PRESENT_INTERVAL");
        _lockFullscreen = full != NULL;
        _lockHud = hud != NULL;
        _resolution.enabled = !_lockResolution;
        _fps.enabled = !_lockFps;
        _fullscreen.enabled = !_lockFullscreen;
        _metalHud.enabled = !_lockHud;
    }
    return self;
}

- (void)saveAndApply {
    NSArray *widths = @[@"1280", @"1600", @"1920"];
    NSArray *heights = @[@"720", @"900", @"1080"];
    NSInteger r = self.resolution.indexOfSelectedItem;
    int fps = self.fps.indexOfSelectedItem == 0 ? 30 : 60;
    if (!self.lockResolution) {
        [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithFormat:@"%@x%@", widths[r], heights[r]] forKey:@"mac.resolution"];
        NSString *width = widths[r], *height = heights[r];
        setenv("DM_WINDOW_WIDTH", width.UTF8String, 1); setenv("DM_WINDOW_HEIGHT", height.UTF8String, 1);
    }
    if (!self.lockFps) {
        [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithFormat:@"%d", fps] forKey:@"mac.fps"];
        setenv("DM_FPS_TARGET", fps == 30 ? "30" : "60", 1);
        setenv("DM_PRESENT_INTERVAL", fps == 30 ? "game" : "display", 1);
    }
    if (!self.lockFullscreen) {
        int on = self.fullscreen.state == NSControlStateValueOn;
        [[NSUserDefaults standardUserDefaults] setObject:on ? @"1" : @"0" forKey:@"mac.fullscreen"];
        setenv("DM_FULLSCREEN", on ? "1" : "0", 1);
    }
    if (!self.lockHud) {
        int on = self.metalHud.state == NSControlStateValueOn;
        [[NSUserDefaults standardUserDefaults] setObject:on ? @"1" : @"0" forKey:@"mac.metalHud"];
        setenv("DM_METAL_HUD", on ? "1" : "0", 1);
    }
    [[NSUserDefaults standardUserDefaults] synchronize];
}

- (BOOL)run {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    [self.window center]; [self.window makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:self.window];
    [self.window orderOut:nil];
    return !self.cancelled;
}

- (void)launch:(id)sender { (void)sender; [self saveAndApply]; self.cancelled = NO; [NSApp abortModal]; }
- (void)reset:(id)sender {
    (void)sender;
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"mac.resolution"];
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"mac.fps"];
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"mac.fullscreen"];
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"mac.metalHud"];
    if (!self.lockResolution) [self.resolution selectItemAtIndex:0];
    if (!self.lockFps) [self.fps selectItemAtIndex:1];
    if (!self.lockFullscreen) [self.fullscreen setState:NSControlStateValueOff];
    if (!self.lockHud) [self.metalHud setState:NSControlStateValueOff];
}
- (void)cancel:(id)sender { (void)sender; self.cancelled = YES; [NSApp abortModal]; }
- (BOOL)windowShouldClose:(id)sender { (void)sender; [self cancel:nil]; return NO; }
@end

static int show_launcher(const char *name) {
    if (getenv("DM_LAUNCHER") && !strcmp(getenv("DM_LAUNCHER"), "0")) return 1;
    DmLauncherSettings *settings = [[DmLauncherSettings alloc] initWithTitle:[NSString stringWithUTF8String:name ?: "nxnative"]];
    return [settings run] ? 1 : 0;
}

int main(void) {
    char self[PATH_MAX], exe[PATH_MAX]; uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) || !realpath(self, exe)) return 1;
    char contents[PATH_MAX], res[PATH_MAX], host[PATH_MAX], dm[PATH_MAX], romfs[PATH_MAX], p[PATH_MAX];
    snprintf(contents, sizeof contents, "%s", exe); *strrchr(contents, '/') = 0; *strrchr(contents, '/') = 0;   // .../Contents/MacOS/<launcher> → .../Contents
    snprintf(res, sizeof res, "%s/Resources", contents); snprintf(host, sizeof host, "%s/MacOS/dmhost", contents);
    snprintf(dm, sizeof dm, "%s/dm", res); snprintf(romfs, sizeof romfs, "%s/romfs", res);
    char name[256] = "nxnative", id[64] = "", marker[512] = "", lang[32] = "";
    if (!info_string("CFBundleName", name, sizeof name)) snprintf(name, sizeof name, "nxnative");
    info_string("DMRomfsMarker", marker, sizeof marker); info_string("DMLanguage", lang, sizeof lang);
    if (!info_string("DMTitleID", id, sizeof id) || strchr(id, '/')) { alert(name, "This app has no title ID in its Info.plist (DMTitleID): pack it again with build_ipa_from_nsp.py --platform mac-app."); return 1; }
    struct stat st; snprintf(p, sizeof p, "%s/%s", romfs, marker);
    if (access(host, X_OK) || stat(p, &st) || !modules_present(dm)) {
        alert(name, "The game data inside this app is missing or incomplete (Contents/Resources/romfs, Contents/Resources/dm or Contents/MacOS/dmhost): pack it again with build_ipa_from_nsp.py --platform mac-app.");
        return 1;
    }
    const char *home = getenv("HOME"); if (!home || !*home) return 1;
    char sup[PATH_MAX], save[PATH_MAX], temp[PATH_MAX], logs[PATH_MAX], log[PATH_MAX], prev[PATH_MAX];
    snprintf(sup, sizeof sup, "%s/Library/Application Support/nxnative/%s", home, id);
    snprintf(save, sizeof save, "%s/save", sup); snprintf(temp, sizeof temp, "%s/temp", sup);
    snprintf(logs, sizeof logs, "%s/Library/Logs/nxnative", home);
    snprintf(log, sizeof log, "%s/%s.log", logs, id); snprintf(prev, sizeof prev, "%s/%s.prev.log", logs, id);
    mkdirs(save); mkdirs(temp); mkdirs(logs);
    rename(log, prev);
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); setvbuf(stdout, NULL, _IOLBF, 0); }
    fprintf(stderr, "launcher: %s (%s), host %s\n", name, id, host);
    snprintf(p, sizeof p, "%s/env.txt", sup); env_file(p);
    snprintf(p, sizeof p, "%s/env.txt", dm); env_file(p);
    if (!show_launcher(name)) { fprintf(stderr, "launcher: settings window cancelled\n"); return 0; }
    setenv("DM_ROOT", sup, 0); setenv("DM_ROMFS", romfs, 0); setenv("DM_SAVE", save, 0); setenv("DM_TEMP", temp, 0);
    snprintf(p, sizeof p, "%s/msl", res); setenv("DM_MSL_DIR", p, 0);
    snprintf(p, sizeof p, "%s/font", dm); setenv("DM_FONT_DIR", p, 0);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dm); if (!stat(p, &st)) setenv("DM_NVN_SHMAP", p, 0);
    snprintf(p, sizeof p, "%s/aoc", res); if (!stat(p, &st)) setenv("DM_AOC_DIR", p, 0);
    snprintf(p, sizeof p, "%s/pso", res); if (!stat(p, &st)) setenv("DM_PSO_BUNDLE", p, 0);   // the bases of every shader pair: the first start builds none
    if (*lang) setenv("DM_LANG", lang, 0);
    setenv("DM_WINDOW", "1", 0); setenv("DM_AUDIO", "1", 0);
    if (chdir(sup)) { fprintf(stderr, "[RED] launcher: cannot enter %s\n", sup); return 1; }
    struct sigaction sa = {.sa_handler = forward};   // no SA_RESTART: waitpid below sees EINTR and waits again
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    char *argv[] = {host, dm, NULL};
    int err = posix_spawn(&g_child, host, NULL, NULL, argv, environ);
    if (err) { fprintf(stderr, "[RED] launcher: cannot start %s: %s\n", host, strerror(err)); return 1; }
    int status;
    while (waitpid(g_child, &status, 0) < 0) if (errno != EINTR) return 1;
    if (WIFSIGNALED(status)) { fprintf(stderr, "launcher: the host stopped on signal %d\n", WTERMSIG(status)); return 128 + WTERMSIG(status); }
    fprintf(stderr, "launcher: the host exited with %d\n", WEXITSTATUS(status));
    return WEXITSTATUS(status);
}
