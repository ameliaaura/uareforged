// UA Reforged - the one-time "anonymous usage stats" question (JavaScript for Automation, macOS built-in).
//   osascript -l JavaScript consent.js <icon file> <heading> <text> <yes button> <no button>
// Prints the button pressed, or nothing when the dialog was dismissed. Nothing is pre-selected: Return does
// not press either button; Esc (and only Esc) presses the "no" button. The "yes" button is tinted with the
// system accent colour, but it is not the default button. Every text goes in as an argument, never into a
// script, so no text can change what runs.
// Testing: UA_DIALOG_SNAP=<folder> saves the dialog as a PNG there (the dialog only, no screen) and answers
// it as dismissed (= no), without showing it.
ObjC.import('Cocoa');

function run(argv) {
    var icon = argv[0], heading = argv[1], text = argv[2], yes = argv[3], no = argv[4];
    var app = $.NSApplication.sharedApplication;
    var snap = ObjC.unwrap($.NSProcessInfo.processInfo.environment.objectForKey('UA_DIALOG_SNAP')) || '';
    app.setActivationPolicy(snap ? $.NSApplicationActivationPolicyProhibited : $.NSApplicationActivationPolicyRegular);

    var alert = $.NSAlert.alloc.init;
    alert.messageText = $(heading);
    alert.informativeText = $(text);
    if (icon) {
        var img = $.NSImage.alloc.initWithContentsOfFile($(icon));
        if (!img.isNil()) alert.icon = img;
    }
    var by = alert.addButtonWithTitle($(yes));    // 1000
    var bn = alert.addButtonWithTitle($(no));     // 1001
    by.keyEquivalent = $('');                     // Return presses nothing
    bn.keyEquivalent = $('\u001b');               // Esc = no
    try { by.bezelColor = $.NSColor.controlAccentColor; } catch (e) { }

    if (snap) {
        // light appearance for the picture (in Dark Mode the text would be white on a transparent background)
        app.appearance = $.NSAppearance.appearanceNamed($.NSAppearanceNameAqua);
        alert.window.appearance = $.NSAppearance.appearanceNamed($.NSAppearanceNameAqua);
        alert.layout;
        var v = alert.window.contentView, r = v.bounds;
        var rep = v.bitmapImageRepForCachingDisplayInRect(r);
        v.cacheDisplayInRectToBitmapImageRep(r, rep);
        var png = rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({}));
        var n = 1, fm = $.NSFileManager.defaultManager, p;
        do { p = snap + '/' + ('0' + n).slice(-2) + '-usage-stats.png'; n++; } while (fm.fileExistsAtPath($(p)));
        png.writeToFileAtomically($(p), true);
        return '';
    }

    app.activateIgnoringOtherApps(true);
    var code = alert.runModal;
    return code == 1000 ? yes : (code == 1001 ? no : '');
}
