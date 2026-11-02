// UA Reforged - progress window for the app's downloads (JavaScript for Automation, macOS built-in).
//   osascript -l JavaScript progress.js <download dir> <title>
// Reads <dir>/status ("<percent>\t<text>[\t<stage>]", written by downloads.sh) four times a second and shows it with
// a progress bar. <stage> (optional) is shown above the text, e.g. "Step 1 of 2: the game"; during "Set up everything"
// the bar shows the progress of the whole setup. Cancel writes <dir>/cancel (the launcher stops the download and keeps
// the part for a later resume). The window closes itself when the status says "done" or the status file goes away.
ObjC.import('Cocoa');
ObjC.import('stdlib');

function run(argv) {
    var dir = argv[0], heading = argv[1] || 'Download';
    var statusPath = dir + '/status', cancelPath = dir + '/cancel';
    var app = $.NSApplication.sharedApplication;
    // testing: UA_DIALOG_SNAP=<folder> keeps the window off screen and saves one picture of it mid-download
    var snap = ObjC.unwrap($.NSProcessInfo.processInfo.environment.objectForKey('UA_DIALOG_SNAP')) || '', snapped = false;
    app.setActivationPolicy(snap ? $.NSApplicationActivationPolicyProhibited : $.NSApplicationActivationPolicyRegular);

    var W = 480, H = 170;
    var win = $.NSWindow.alloc.initWithContentRectStyleMaskBackingDefer(
        $.NSMakeRect(0, 0, W, H), $.NSWindowStyleMaskTitled, $.NSBackingStoreBuffered, false);
    win.title = 'UA Reforged';
    win.releasedWhenClosed = false;

    var title = $.NSTextField.labelWithString(heading);
    title.font = $.NSFont.boldSystemFontOfSize(13);
    title.frame = $.NSMakeRect(20, H - 40, W - 40, 20);
    var stage = $.NSTextField.labelWithString('');
    stage.font = $.NSFont.systemFontOfSizeWeight(12, 0.3);
    stage.textColor = $.NSColor.secondaryLabelColor;
    stage.frame = $.NSMakeRect(20, H - 62, W - 40, 18);
    var text = $.NSTextField.labelWithString('Checking the download link...');
    text.frame = $.NSMakeRect(20, H - 84, W - 40, 18);
    var bar = $.NSProgressIndicator.alloc.initWithFrame($.NSMakeRect(20, H - 112, W - 40, 20));
    bar.indeterminate = false;
    bar.minValue = 0;
    bar.maxValue = 100;

    var seen = false, missing = 0;
    function readStatus() {
        var s = $.NSString.stringWithContentsOfFileEncodingError(statusPath, $.NSUTF8StringEncoding, null);
        if (s.isNil()) return null;
        var f = ObjC.unwrap(s).split('\n')[0].split('\t');
        if (f.length < 2) return null;
        return { pct: parseFloat(f[0]) || 0, text: f[1], stage: f.length > 2 ? f[2] : '' };
    }

    if (!$.PackProgressUI) {
        ObjC.registerSubclass({
            name: 'PackProgressUI',
            methods: {
                'tick:': {
                    types: ['void', ['id']],
                    implementation: function (timer) {
                        var st = readStatus();
                        if (st === null) {
                            if (seen && ++missing > 8) $.exit(0);
                            return;
                        }
                        seen = true; missing = 0;
                        if (st.text === 'done') $.exit(0);
                        bar.doubleValue = st.pct;
                        text.stringValue = $(st.text);
                        stage.stringValue = $(st.stage);
                        if (snap && !snapped && st.pct >= 5 && st.text.indexOf('Downloading') === 0) { snapped = true; snapWindow(); }
                    }
                },
                'cancelPressed:': {
                    types: ['void', ['id']],
                    implementation: function (sender) {
                        $('cancel').writeToFileAtomicallyEncodingError(cancelPath, true, $.NSUTF8StringEncoding, null);
                        text.stringValue = $('Stopping...');
                        $.exit(2);
                    }
                }
            }
        });
    }
    var handler = $.PackProgressUI.alloc.init;
    function snapWindow() {
        var v = win.contentView, r = v.bounds;
        var rep = v.bitmapImageRepForCachingDisplayInRect(r);
        v.cacheDisplayInRectToBitmapImageRep(r, rep);
        rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({})).writeToFileAtomically($(snap + '/progress.png'), true);
    }

    var cancel = $.NSButton.buttonWithTitleTargetAction('Cancel', handler, 'cancelPressed:');
    cancel.frame = $.NSMakeRect(W - 120, 14, 100, 32);
    cancel.keyEquivalent = $('\u001b');

    var view = win.contentView;
    view.addSubview(title);
    view.addSubview(stage);
    view.addSubview(text);
    view.addSubview(bar);
    view.addSubview(cancel);

    $.NSTimer.scheduledTimerWithTimeIntervalTargetSelectorUserInfoRepeats(0.25, handler, 'tick:', $(), true);
    win.center;
    if (!snap) {
        win.makeKeyAndOrderFront(null);
        app.activateIgnoringOtherApps(true);
    }
    app.run;
}
