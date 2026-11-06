// HP2 launcher window (owner 2026-11-06: language UA/EN in the launcher, English by default,
// remembered). JXA + AppKit, run by Contents/MacOS/HP2 before the game:
//   osascript -l JavaScript launcher.js <launcher.toml> [art.png]
// Shows the game art (Resources/launcher/art.png: the title screen, tools/launcher/make_art.sh),
// the language (English / Українська / Українська — текст: Ukrainian text, English speech) and Play.
// Play writes [launcher] language = en | uk | uk_text into launcher.toml and prints "play"; closing
// the window prints "quit". HP2_LAUNCHER_AUTO=play|quit answers without the window (tests).
// HP2_LAUNCHER_SNAPSHOT=<png> draws the window off screen into a PNG and prints its labels
// (tests/scripts/launcher_ui.sh). Gamepad (as ROTK): D-pad up/down picks Language / Play,
// left/right changes the language, A plays, B quits. HP2_LAUNCHER_PAD_SCRIPT="right a" feeds
// those presses to the same handler (tests). The window's own text follows the chosen language.
// Port of ROTKPort/tools/launcher/launcher.js (fps and gamma rows dropped: HP2 runs at 60).
ObjC.import('AppKit');
ObjC.import('Foundation');
ObjC.import('GameController');

function readText(path) {
  const s = $.NSString.stringWithContentsOfFileEncodingError(path, $.NSUTF8StringEncoding, null);
  return s.isNil() ? '' : s.js;
}
function writeText(path, text) {
  $(text).writeToFileAtomicallyEncodingError(path, true, $.NSUTF8StringEncoding, null);
}
function getSetting(text, key, def) {
  const m = text.match(new RegExp('^\\s*' + key + '\\s*=\\s*"?([^"\\n]*)"?', 'm'));
  return m ? m[1].trim() : def;
}
function putSettings(text, kv) {
  let lines = text.split('\n');
  if (!lines.some(l => l.trim() === '[launcher]')) lines = ['[launcher]'].concat(lines);
  for (const k of Object.keys(kv)) {
    const line = k + ' = "' + kv[k] + '"';
    const i = lines.findIndex(l => new RegExp('^\\s*' + k + '\\s*=').test(l));
    if (i >= 0) lines[i] = line;
    else lines.splice(lines.findIndex(l => l.trim() === '[launcher]') + 1, 0, line);
  }
  return lines.join('\n').replace(/\n*$/, '\n');
}

const sel = (popup) => Number(popup.indexOfSelectedItem);  // AppKit numbers come back as strings (PIT-D02)
const LANGS = ['en', 'uk', 'uk_text'];

function run(argv) {
  const settingsPath = argv[0], artPath = argv[1] || '';
  const text = readText(settingsPath);
  const st = { language: LANGS.includes(getSetting(text, 'language', 'en')) ? getSetting(text, 'language', 'en') : 'en' };
  const save = () => writeText(settingsPath, putSettings(text, st));
  const env = $.NSProcessInfo.processInfo.environment;
  const auto = env.objectForKey('HP2_LAUNCHER_AUTO');
  if (!auto.isNil()) {
    if (auto.js === 'play') save();
    return auto.js === 'play' ? 'play' : 'quit';
  }
  const T = {
    uk: { lang: 'Мова', play: 'Грати', quit: 'Вийти', langs: ['English', 'Українська', 'Українська — текст'],
          hint: { en: 'Англійська мова й озвучка.', uk: 'Український текст і озвучка.', uk_text: 'Український текст, англійська озвучка.' } },
    en: { lang: 'Language', play: 'Play', quit: 'Quit', langs: ['English', 'Українська', 'Українська — текст'],
          hint: { en: 'English text and speech.', uk: 'Ukrainian text and speech.', uk_text: 'Ukrainian text, English speech.' } },
  };
  const app = $.NSApplication.sharedApplication;
  app.setActivationPolicy($.NSApplicationActivationPolicyRegular);
  const W = 768, P = 110, H = 298 + P;  // the title logo above (make_art.sh: 3072x1190), controls below
  const win = $.NSWindow.alloc.initWithContentRectStyleMaskBackingDefer(
    $.NSMakeRect(0, 0, W, H), $.NSWindowStyleMaskTitled | $.NSWindowStyleMaskClosable, $.NSBackingStoreBuffered, false);
  win.title = 'Harry Potter and the Chamber of Secrets';
  win.center;
  const view = win.contentView;
  win.backgroundColor = $.NSColor.blackColor;  // (a CGColor through the JXA bridge kills osascript)
  let artInfo = 'art: none';
  if (artPath) {
    const img = $.NSImage.alloc.initWithContentsOfFile(artPath);
    if (!img.isNil()) {
      const iv = $.NSImageView.alloc.initWithFrame($.NSMakeRect(0, P, W, H - P));
      iv.image = img;
      iv.imageScaling = $.NSImageScaleProportionallyUpOrDown;  // never stretched
      view.addSubview(iv);
      artInfo = 'art: view ' + W + 'x' + (H - P) + ', image ' + img.size.width + 'x' + img.size.height;
    }
  }
  const panel = $.NSVisualEffectView.alloc.initWithFrame($.NSMakeRect(0, 0, W, P));
  panel.material = $.NSVisualEffectMaterialHUDWindow;
  panel.blendingMode = $.NSVisualEffectBlendingModeWithinWindow;
  panel.state = $.NSVisualEffectStateActive;
  view.addSubview(panel);
  const labels = [];
  const label = (x, y, w, h) => {
    const l = $.NSTextField.wrappingLabelWithString('');
    l.frame = $.NSMakeRect(x, y, w, h || 20);
    l.textColor = $.NSColor.whiteColor;
    panel.addSubview(l);
    labels.push(l);
    return l;
  };
  const lLang = label(24, 78, 240), lHint = label(24, 14, 440, 20);
  lHint.font = $.NSFont.systemFontOfSize(12);
  lHint.textColor = $.NSColor.colorWithWhiteAlpha(0.82, 1);
  const pLang = $.NSPopUpButton.alloc.initWithFramePullsDown($.NSMakeRect(20, 44, 240, 26), false);
  pLang.addItemsWithTitles($(T.en.langs));
  pLang.selectItemAtIndex(LANGS.indexOf(st.language));
  panel.addSubview(pLang);
  // Buttons take the first click even while the window is not active (ROTK: macOS 14).
  ObjC.registerSubclass({
    name: 'Hp2Button', superclass: 'NSButton',
    methods: { 'acceptsFirstMouse:': { types: ['bool', ['id']], implementation: function () { return true; } } },
  });
  const bPlay = $.Hp2Button.buttonWithTitleTargetAction('', null, null);
  bPlay.frame = $.NSMakeRect(W - 222, 44, 200, 50);
  bPlay.bezelStyle = $.NSBezelStyleRegularSquare;
  bPlay.keyEquivalent = '\r';
  bPlay.font = $.NSFont.boldSystemFontOfSize(22);
  panel.addSubview(bPlay);
  const bQuit = $.Hp2Button.buttonWithTitleTargetAction('', null, null);
  bQuit.frame = $.NSMakeRect(W - 222, 10, 200, 28);
  bQuit.keyEquivalent = '\x1b';
  panel.addSubview(bQuit);
  const cur = () => LANGS[sel(pLang)] || 'en';
  const relabel = () => {
    const t = T[cur() === 'en' ? 'en' : 'uk'];
    lLang.stringValue = t.lang;
    lHint.stringValue = t.hint[cur()];
    bPlay.title = t.play;
    bQuit.title = t.quit;
  };
  relabel();
  let choice = 'quit';
  const rows = [pLang, bPlay];
  let row = 1;
  const pick = (r) => { row = Math.max(0, Math.min(rows.length - 1, r)); win.makeFirstResponder(rows[row]); };
  const press = (k) => {
    if (k === 'up') pick(row - 1);
    else if (k === 'down') pick(row + 1);
    else if (k === 'left' || k === 'right') {
      pLang.selectItemAtIndex(Math.max(0, Math.min(LANGS.length - 1, sel(pLang) + (k === 'left' ? -1 : 1))));
      relabel();
    } else if (k === 'a') { choice = 'play'; app.stopModalWithCode(1); }
    else if (k === 'b') app.stopModalWithCode(0);
  };
  const scriptEnv = env.objectForKey('HP2_LAUNCHER_PAD_SCRIPT');
  const script = scriptEnv.isNil() ? null : scriptEnv.js.split(/\s+/).filter(Boolean);
  if ($.GCController.respondsToSelector('setShouldMonitorBackgroundEvents:')) $.GCController.shouldMonitorBackgroundEvents = true;
  const held = {};
  const tick = () => {
    if (script) { if (script.length) press(script.shift()); return; }
    const cs = $.GCController.controllers;
    const now = {};
    for (let i = 0; i < cs.count; i++) {
      const g = cs.objectAtIndex(i).extendedGamepad;
      if (g.isNil()) continue;
      const on = (b) => !b.isNil() && b.isPressed;
      if (on(g.buttonA)) now.a = 1;
      if (on(g.buttonB)) now.b = 1;
      if (on(g.dpad.up) || g.leftThumbstick.yAxis.value > 0.6) now.up = 1;
      if (on(g.dpad.down) || g.leftThumbstick.yAxis.value < -0.6) now.down = 1;
      if (on(g.dpad.left) || g.leftThumbstick.xAxis.value < -0.6) now.left = 1;
      if (on(g.dpad.right) || g.leftThumbstick.xAxis.value > 0.6) now.right = 1;
    }
    for (const k of ['up', 'down', 'left', 'right', 'b', 'a']) {
      if (now[k] && !held[k]) press(k);
      held[k] = now[k];
    }
  };
  ObjC.registerSubclass({
    name: 'Hp2LauncherTarget',
    methods: {
      'changed:': { types: ['void', ['id']], implementation: function () { relabel(); } },
      'tick:': { types: ['void', ['id']], implementation: function () { tick(); } },
      'play:': { types: ['void', ['id']], implementation: function () { choice = 'play'; app.stopModalWithCode(1); } },
      'quit:': { types: ['void', ['id']], implementation: function () { app.stopModalWithCode(0); } },
      'windowWillClose:': { types: ['void', ['id']], implementation: function () { app.stopModalWithCode(0); } },
    },
  });
  const tgt = $.Hp2LauncherTarget.alloc.init;
  pLang.target = tgt; pLang.action = 'changed:';
  bPlay.target = tgt; bPlay.action = 'play:';
  bQuit.target = tgt; bQuit.action = 'quit:';
  win.delegate = tgt;
  const snap = env.objectForKey('HP2_LAUNCHER_SNAPSHOT');
  if (!snap.isNil()) {
    if (script) while (script.length) press(script.shift());
    const rep = view.bitmapImageRepForCachingDisplayInRect(view.bounds);
    view.cacheDisplayInRectToBitmapImageRep(view.bounds, rep);
    rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({})).writeToFileAtomically(snap.js, true);
    const clipped = labels.filter(l => !l.hidden && l.stringValue.js &&
      l.cell.cellSizeForBounds($.NSMakeRect(0, 0, l.frame.size.width, 10000)).height > l.frame.size.height + 0.5)
      .map(l => l.stringValue.js.slice(0, 24));
    const t = T[cur() === 'en' ? 'en' : 'uk'];
    return ['snapshot', artInfo, 'language: ' + cur(), 'labels: ' + [t.lang, t.play, t.quit].join('|'),
            'hint: ' + t.hint[cur()], 'clipped: ' + (clipped.length ? clipped.join(' | ') : 'none')].join('\n');
  }
  app.activateIgnoringOtherApps(true);
  win.makeKeyAndOrderFront(null);
  pick(1);
  const timer = $.NSTimer.timerWithTimeIntervalTargetSelectorUserInfoRepeats(script ? 0.25 : 1 / 30, tgt, 'tick:', null, true);
  $.NSRunLoop.currentRunLoop.addTimerForMode(timer, $.NSRunLoopCommonModes);
  app.runModalForWindow(win);  // the choice is kept in JS (the modal response is a string, ROTK 049f010)
  if (choice !== 'play') return 'quit';
  st.language = cur();
  save();
  win.orderOut(null);
  // macOS 14 cooperative activation: the game (after execv) becomes active only if we yield to it.
  const owner = env.objectForKey('HP2_LAUNCHER_OWNER');
  if (!owner.isNil() && app.respondsToSelector('yieldActivationToApplication:')) {
    const ra = $.NSRunningApplication.runningApplicationWithProcessIdentifier(parseInt(owner.js, 10));
    if (!ra.isNil()) app.yieldActivationToApplication(ra);
  }
  return 'play';
}
