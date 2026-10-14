/*
 * save_mount.js -- the game's saves in the browser (an Emscripten --pre-js).
 *
 * limeWriteFile / limeLoadSaveFile (runtime/lime_menu.c) keep the iPhone's
 * Documents/ in UMK3_SAVE_DIR: settings, the tower, unlocks, stats. In the
 * browser the default place is the in-memory filesystem, gone on reload, so
 * this mounts IDBFS -- the browser's own storage for this site, in this
 * browser only -- at /save, fills it before main runs, and writes it back a
 * few seconds after anything changes and whenever the page is hidden or
 * closed. Nothing here leaves the browser.
 */
Module.preRun = Module.preRun || [];
Module.preRun.push(function() {
  var DIR = "/save";
  var dependency = "umk3-saves";

  try {
    FS.mkdir(DIR);
    FS.mount(IDBFS, {}, DIR);
  } catch (e) {
    /* Storage unavailable (a private window, say): the game still runs and
     * its saves last for the session, as before. */
    (Module.printErr || console.warn)("saves: " + e.message);
    return;
  }
  ENV.UMK3_SAVE_DIR = DIR;

  addRunDependency(dependency);
  FS.syncfs(true, function(err) {
    if (err) (Module.printErr || console.warn)("saves: could not read: " + err);
    removeRunDependency(dependency);
  });

  var busy = false, again = false;
  function flush() {
    if (busy) { again = true; return; }
    busy = true;
    FS.syncfs(false, function(err) {
      busy = false;
      if (err) (Module.printErr || console.warn)("saves: could not write: " + err);
      if (again) { again = false; flush(); }
    });
  }
  /* IDBFS compares timestamps, so a sync with nothing new costs little. */
  setInterval(flush, 5000);
  document.addEventListener("visibilitychange", function() {
    if (document.visibilityState === "hidden") flush();
  });
  window.addEventListener("pagehide", flush);
});
