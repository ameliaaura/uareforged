// UA Reforged - a dialog with more room than "display dialog" (JavaScript for Automation, macOS built-in).
//   osascript -l JavaScript dialog.js <icon file> <heading> <text> <checkbox label> <checkbox on 0|1>
//                                     <menu label> <menu items, | separated> <menu index> <button 1 = default> [button 2...]
// Empty checkbox / menu labels leave them out. Prints "<button>\t<checkbox 0|1>\t<menu index>" (button empty when
// closed). Every text goes in as an argument, never into a script, so no text can change what runs.
// Testing: UA_DIALOG_SNAP=<folder> saves the dialog as a PNG there (the dialog only, no screen) and answers it with
// the default button and the default checkbox / menu values, without showing it.
ObjC.import('Cocoa');
ObjC.import('stdlib');

function run(argv) {
    var icon = argv[0], heading = argv[1], text = argv[2];
    var checkLabel = argv[3], checkOn = argv[4] === '1';
    var menuLabel = argv[5], menuItems = argv[6] ? argv[6].split('|') : [], menuSel = parseInt(argv[7], 10) || 0;
    var buttons = argv.slice(8);
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
    buttons.forEach(function (b) { alert.addButtonWithTitle($(b)); });

    var W = 300, rows = [], check = null, menu = null;
    if (menuLabel && menuItems.length) {
        var row = $.NSView.alloc.initWithFrame($.NSMakeRect(0, 0, W, 26));
        var l = $.NSTextField.labelWithString($(menuLabel));
        l.frame = $.NSMakeRect(0, 4, 80, 18);
        menu = $.NSPopUpButton.alloc.initWithFramePullsDown($.NSMakeRect(82, 0, W - 82, 26), false);
        menuItems.forEach(function (m) { menu.addItemWithTitle($(m)); });
        menu.selectItemAtIndex(Math.min(menuSel, menuItems.length - 1));
        row.addSubview(l); row.addSubview(menu);
        rows.push(row);
    }
    if (checkLabel) {
        check = $.NSButton.checkboxWithTitleTargetAction($(checkLabel), $(), null);
        check.state = checkOn ? $.NSControlStateValueOn : $.NSControlStateValueOff;
        check.sizeToFit;
        var cw = Math.max(W, check.frame.size.width);
        check.frame = $.NSMakeRect(0, 0, cw, 22);
        W = cw;
        rows.push(check);
    }
    if (rows.length) {
        var h = 0; rows.forEach(function (r) { h += r.frame.size.height + 8; });
        var box = $.NSView.alloc.initWithFrame($.NSMakeRect(0, 0, W, h));
        var y = h;
        rows.forEach(function (r) { y -= r.frame.size.height + 8; var f = r.frame; r.frame = $.NSMakeRect(0, y + 4, f.size.width, f.size.height); box.addSubview(r); });
        alert.accessoryView = box;
    }

    function result(button) {
        return button + '\t' + (check && check.state == $.NSControlStateValueOn ? '1' : '0') + '\t' + (menu ? menu.indexOfSelectedItem : 0);
    }

    if (snap) {
        alert.layout;
        var v = alert.window.contentView, r = v.bounds;
        var rep = v.bitmapImageRepForCachingDisplayInRect(r);
        v.cacheDisplayInRectToBitmapImageRep(r, rep);
        var png = rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({}));
        var n = 1, fm = $.NSFileManager.defaultManager, p;
        do { p = snap + '/' + ('0' + n).slice(-2) + '-dialog.png'; n++; } while (fm.fileExistsAtPath($(p)));
        png.writeToFileAtomically($(p), true);
        return result(buttons[0]);
    }

    app.activateIgnoringOtherApps(true);
    var code = alert.runModal;   // 1000 = the first button, 1001 the second, ...
    var i = code - 1000;
    return result(i >= 0 && i < buttons.length ? buttons[i] : '');
}
