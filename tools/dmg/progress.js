// HP2 installer progress window (osascript -l JavaScript progress.js <progress file>): a small
// AppKit window with a bar and the stage text, read every half second from the file that
// build_game.sh writes ("N text" lines, N = 0..100); closes itself at 100.
ObjC.import('Cocoa');
function run(argv) {
  const file = argv[0];
  const app = $.NSApplication.sharedApplication;
  app.setActivationPolicy($.NSApplicationActivationPolicyRegular);
  const win = $.NSWindow.alloc.initWithContentRectStyleMaskBackingDefer(
    $.NSMakeRect(0, 0, 460, 120), $.NSWindowStyleMaskTitled, $.NSBackingStoreBuffered, false);
  // interface language: HP2_UI_LANG (the launcher's saved choice), English unless uk
  const ui = $.NSProcessInfo.processInfo.environment.objectForKey('HP2_UI_LANG');
  const uk = !ui.isNil() && ui.js === 'uk';
  win.title = uk ? 'HP2: встановлення' : 'HP2: installing';
  const label = $.NSTextField.labelWithString(uk ? 'Підготовка' : 'Preparing');
  label.frame = $.NSMakeRect(20, 70, 420, 24);
  const bar = $.NSProgressIndicator.alloc.initWithFrame($.NSMakeRect(20, 35, 420, 20));
  bar.indeterminate = false; bar.minValue = 0; bar.maxValue = 100;
  win.contentView.addSubview(label); win.contentView.addSubview(bar);
  win.center; win.makeKeyAndOrderFront(null); app.activateIgnoringOtherApps(true);
  for (;;) {
    const s = $.NSString.stringWithContentsOfFileEncodingError(file, $.NSUTF8StringEncoding, null);
    if (!s.isNil()) {
      const lines = s.js.trim().split('\n'), last = lines[lines.length - 1] || '';
      const m = last.match(/^(\d+) (.*)$/);
      if (m) { bar.doubleValue = +m[1]; label.stringValue = m[2]; if (+m[1] >= 100) break; }
    }
    $.NSRunLoop.currentRunLoop.runUntilDate($.NSDate.dateWithTimeIntervalSinceNow(0.5));
  }
  $.NSRunLoop.currentRunLoop.runUntilDate($.NSDate.dateWithTimeIntervalSinceNow(1.5));
}
