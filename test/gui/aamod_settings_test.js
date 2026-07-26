// Host self-test for resources/gui/aaModSettings.js. Node, no dependencies:
//
//     node test/gui/aamod_settings_test.js        (from the repo root)
//
// The screen wraps OEM syssettingsApp methods and borrows the OEM About context,
// so most of what needs proving is that it does not disturb the app it lives in:
// the System tab list stays stable across visits, About stays last, the real About
// comes back unskinned, unrelated rows still reach the OEM handler. Plus both
// dbapi orderings (GetConfig answering before vs. after the template is ready),
// since only one of them happens on a warm unit.
//
// The stubs reproduce just enough of the framework and of the OEM SystemTab
// populate to make those properties testable off-car. Non-zero exit on failure.

var fails = 0, log_ = [];

function check(ok, what) { console.log((ok ? "PASS  " : "FAIL  ") + what); if (!ok) fails++; }

global.log = { addSrcFile: function(){}, debug: function(m){log_.push("D "+m)},
               info: function(m){log_.push("I "+m)}, warn: function(m){log_.push("W "+m)},
               error: function(m){log_.push("E "+m)}, writeError: function(){} };
global.document = { body: {}, createElement: function(){ return {classList:{add:function(){}}, style:{}, appendChild:function(){}}; } };

var dbapiCalls = [], mmuiEvents = [], controls = [];
var curLang = "LANGS_US_ENGLISH";
var curApp = null, curAppInstance = null;
var atSpeed = false;

// Timers fire only when a test says so, and never keep node alive.
var timers = [];
global.setTimeout   = function (fn, ms) { timers.push(fn); return timers.length; };
global.clearTimeout = function (id) { timers[id - 1] = null; };
function fireTimers() { var t = timers; timers = []; t.forEach(function (fn) { if (fn) fn(); }); }
var controllerEventsToList = [];
global.framework = {
    localize: {
        getCurrentLanguage: function () { return curLang; },
        // Real map from Localization.js; the framework stores a LOCALE here.
        LANG_CODES: { LANGS_US_ENGLISH: "en_US", LANGS_CZ_CZECH: "cs_CS",
                      LANGS_DE_GERMAN: "de_DE" }
    },
    common: {
        handleControllerEvent: function (eventId, firstEvent) {
            controllerEventsToList.push(eventId);      // i.e. reached the list
        },
        getAtSpeedValue: function () { return atSpeed; }
    },
    getCurrentApp: function () { return curApp; },
    getCurrAppInstance: function () { return curAppInstance; },
    sendRequestToDbapi: function (uiaId, cb, api, method, params) { dbapiCalls.push({api:api, method:method, params:params, cb:cb}); },
    sendEventToMmui: function (uiaId, ev, p) { mmuiEvents.push(ev); },
    instantiateControl: function (uiaId, parent, name, props) {
        var c = { _name:name, _props:props, divElt:{style:{}, classList:{add:function(){}}},
                  _ctrlEvents: [],
                  handleControllerEvent: function (eventId) {
                      this._ctrlEvents.push(eventId);
                      // Dialog3Ctrl fires the focused button on select/selectStart.
                      if ((eventId === "select" || eventId === "selectStart") &&
                          this._props.defaultSelectCallback) {
                          this._props.defaultSelectCallback(this, "AaModDialogOk", {});
                      }
                  } };
        controls.push(c); return c;
    },
    destroyControl: function (c) { var i = controls.indexOf(c); if (i>=0) controls.splice(i,1); }
};
global.utility = { loadScript: function(){} };

// --- fake List2 control ---------------------------------------------------
function List2() { this.dataList = null; this.repaints = [];
                   this.properties = {}; }   // real List2Ctrl keeps selectCallback here
List2.prototype.setDataList = function (dl) { this.dataList = dl; };
List2.prototype.updateItems = function (a, b) { this.repaints.push([a,b]); };

// --- fake syssettingsApp, with the OEM behaviour we depend on -------------
function syssettingsApp() {}
global.syssettingsApp = syssettingsApp;
var P = syssettingsApp.prototype;
P.uiaId = "syssettings";
P.oemPopulateCalls = 0;

// Reproduces the shape of the OEM SystemTab populate: assign rows BY INDEX,
// set itemCount, never shorten items[].
P._populateListCtrl = function (tmplt) {
    this.oemPopulateCalls++;
    if (this._currentContext.ctxtId === "SystemTab") {   // no About branch in OEM
        var l = this._SystemTabCtxtDataList, i = 0;
        var oem = ["SetToolTips","SelectChangeLanguage","SetUnitsTemperature","SetUnitsDistance",
                   "SelectMusicDatabaseUpdate","SelectFactoryReset","SelectAbout"];
        for (i = 0; i < oem.length; i++) l.items[i] = { appData: oem[i], text1Id: oem[i], itemStyle:"style01" };
        l.itemCount = oem.length;
        this._SystemTabHelperArray["factoryReset"].index = 5;
        this._SystemTabHelperArray["about"].index = 6;
        tmplt.list2Ctrl.setDataList(l);
    }
    // The OEM populate ends with the lockout for every context — i.e. BEFORE any
    // row a wrapper adds after calling it.
    this._EnableDisableControlItems();
};
// The OEM SystemTab and About branches: SystemTab locks rows up to Factory Reset
// plus items[itemCount - 1] (About); About sets every row to the bare speed flag.
P._EnableDisableControlItems = function () {
    this.oemEnableCalls = (this.oemEnableCalls||0)+1;
    this._disableSpeedRestricted = framework.common.getAtSpeedValue();
    var l = this._currentContextTemplate && this._currentContextTemplate.list2Ctrl;
    if (!l || !l.dataList || !l.dataList.items) return;
    var d = l.dataList, s = this._disableSpeedRestricted, i;
    if (this._currentContext.ctxtId === "SystemTab") {
        for (i = 0; i <= this._SystemTabHelperArray["factoryReset"].index; i++) d.items[i].disabled = s;
        d.items[d.itemCount - 1].disabled = s;
        l.updateItems(0, d.itemCount - 1);
    } else if (this._currentContext.ctxtId === "About") {
        for (i = 0; i < d.items.length; i++) d.items[i].disabled = s;
        l.updateItems(0, d.items.length - 1);
    }
};
P.oemSelectSeen = [];
P._menuItemSelectCallback = function (listCtrlObj, appData, params) { this.oemSelectSeen.push(appData); };

function newApp() {
    var a = new syssettingsApp();
    // Reproduce appInit's pre-binding: bind once, copy the bound value into the
    // context table. Without this the pre-bound bug is invisible here.
    a.listItemClick = ORIG_SELECT.bind(a);
    a._SystemTabCtxtDataList = { itemCountKnown:true, itemCount:0, items:[] };
    a._SystemTabHelperArray = { factoryReset:{index:0}, about:{index:0} };
    a._contextTable = {
        About: {
            controlProperties: { List2Ctrl: { dataList: "OEM_ABOUT_LIST",
                                              title: { titleStyle:"style02", text1Id:"About" },
                                              selectCallback: a.listItemClick } },
            readyFunction: function(){}
        },
        SystemTab: {
            controlProperties: { List2Ctrl: { dataList: a._SystemTabCtxtDataList,
                                              selectCallback: a.listItemClick } },
            readyFunction: function(){}
        }
    };
    a._disableSpeedRestricted = false;
    a._currentContext = { ctxtId: null };
    a._currentContextTemplate = { list2Ctrl: new List2() };
    a._currentContextTemplate.list2Ctrl.properties.selectCallback = a.listItemClick;
    return a;
}

// appInit binds this before our file loads; binding the wrapped method instead
// would make the pre-bound test vacuous.
var ORIG_SELECT = P._menuItemSelectCallback;

require("../../resources/gui/aaModSettings.js");

// ===================== 1. System tab row, repeated visits ================
var app = newApp();
app._currentContext.ctxtId = "SystemTab";
var counts = [];
for (var visit = 0; visit < 4; visit++) {
    app._populateListCtrl(app._currentContextTemplate);
    var it = app._SystemTabCtxtDataList;
    var abouts = it.items.filter(function(r){return r && r.appData==="SelectAbout";}).length;
    var mine   = it.items.filter(function(r){return r && r.appData==="SelectAaMod";}).length;
    counts.push(it.itemCount + "/" + it.items.length + " about=" + abouts + " mine=" + mine);
}
console.log("     visits: " + counts.join(" | "));
check(counts.every(function(c){return c === counts[0];}), "System tab list is stable across repeated visits");
check(/about=1 mine=1$/.test(counts[3]), "exactly one About row and one mod row after 4 visits");
var items = app._SystemTabCtxtDataList.items;
check(items[items.length-1].appData === "SelectAbout", "About stays LAST (EnableDisable indexes items[count-1] as About)");
check(items[items.length-2].appData === "SelectAaMod", "mod row sits immediately before About");
check(app._SystemTabHelperArray["about"].index === items.length-1, "OEM about helper index still points at About");
check(app._SystemTabHelperArray["factoryReset"].index === 5 &&
      items[5].appData === "SelectFactoryReset", "OEM factoryReset helper index still correct");

// A changed item count means the rows must be rebuilt: setDataList() empties the
// scroller and only updateItems() builds row DOM. Without it the tab stays blank
// until a scroll, and an untapped row has no DOM to hit.
(function () {
    var t = newApp();
    t._currentContext.ctxtId = "SystemTab";
    t._currentContextTemplate.list2Ctrl.repaints = [];
    t._populateListCtrl(t._currentContextTemplate);
    var n = t._SystemTabCtxtDataList.itemCount;
    var full = t._currentContextTemplate.list2Ctrl.repaints.filter(function (r) {
        return r[0] === 0 && r[1] === n - 1;
    });
    check(full.length > 0,
          "System tab populate rebuilds ALL rows (updateItems(0, " + (n - 1) + "))");
})();

// at-speed lockout reaches our row
atSpeed = true;
app._EnableDisableControlItems();
check(items[items.length-2].disabled === true, "mod row picks up the at-speed lockout");
atSpeed = false;
app._EnableDisableControlItems();
check(items[items.length-2].disabled === false, "lockout released when stopped");

// Opening Settings while ALREADY moving: no speed change follows, so the only
// lockout pass is the one inside the OEM populate, before our row exists.
(function () {
    var t = newApp();
    t._currentContext.ctxtId = "SystemTab";
    atSpeed = true;
    t._populateListCtrl(t._currentContextTemplate);
    var r = t._SystemTabCtxtDataList.items[t._aaModSystemTabIndex];
    check(r.appData === "SelectAaMod" && r.disabled === true,
          "mod row is locked when the System tab opens at speed");
    atSpeed = false;
})();

// ===================== 2. opening the screen ============================
dbapiCalls = []; mmuiEvents = [];
app._menuItemSelectCallback(app._currentContextTemplate.list2Ctrl, "SelectAaMod", {itemIndex: items.length-2});
check(mmuiEvents.length === 1 && mmuiEvents[0] === "SelectAbout", "entry sends the OEM SelectAbout event");
check(dbapiCalls.length === 1 && dbapiCalls[0].api === "aamod" && dbapiCalls[0].method === "GetConfig",
      "entry asks the aamod plugin for GetConfig");
check(typeof dbapiCalls[0].params === "object" && dbapiCalls[0].params !== null,
      "GetConfig params is an OBJECT (host rejects a string)");
var aboutProps = app._contextTable.About.controlProperties.List2Ctrl;
check(aboutProps.title.text1 === "Android Auto Mod" && !aboutProps.title.text1Id,
      "About retitled with literal text (no text1Id, or it would be localised over)");
check(aboutProps.dataList.items[0].text1.indexOf("Reading") === 0, "placeholder row until GetConfig answers");
check(typeof app._contextTable.About.noLongerDisplayedFunction === "function", "leave-hook installed");

// transition happens: About becomes current with a fresh control
var getCb = dbapiCalls[0].cb;
app._currentContext.ctxtId = "About";
app._currentContextTemplate = { list2Ctrl: new List2() };
app._currentContextTemplate.list2Ctrl.setDataList(aboutProps.dataList);   // control renders from properties

// ...and only THEN does GetConfig answer (the framework queues dbapi msgs during transitions)
getCb({ msgContent: { api:"aamod", methodName:"GetConfig", reqId:1, params: {
    result: 0, path: "/data_persist/oem-aa-mod/libpatch.conf", present: true,
    items: [
        { key:"touch", label:"Touch passthrough", type:"bool", value:"true" },
        { key:"unmute_starts_playback", label:"Unmute starts playback", type:"bool", value:"false" },
        { key:"hud_transport", label:"HUD transport", type:"enum", value:"vbs",
          options:["svcnavi","vbs"], labels:["svcnavi","vbs"] },
        { key:"hud_fold_latin", label:"HUD accent folding", type:"bool", value:"false" }
    ] } } });
var L = app._currentContextTemplate.list2Ctrl;
check(L.dataList.items.length === 5, "four setting rows rendered, plus Apply");
check(L.repaints.length > 0, "list repainted after the late GetConfig answer");
check(L.dataList.items[0].itemStyle === "styleOnOff" && L.dataList.items[0].value === 1, "touch=true -> On");
check(L.dataList.items[1].value === 2, "unmute_starts_playback=false -> Off");
check(L.dataList.items[2].itemStyle === "style10" && L.dataList.items[2].value === 2 &&
      L.dataList.items[2].button1 === "svcnavi", "hud_transport=vbs -> 2-button row, 2nd selected");
check(L.dataList.items[3].itemStyle === "styleOnOff" && L.dataList.items[3].value === 2,
      "hud_fold_latin=false -> Off");

// At speed the mod screen locks like the real About; stopping must not enable
// Apply with nothing pending (the OEM About branch sets every row to the bare flag).
atSpeed = true;
app._EnableDisableControlItems();
check(L.dataList.items.every(function (r) { return r.disabled === true; }),
      "at speed every mod-screen row is locked");
atSpeed = false;
app._EnableDisableControlItems();
check(L.dataList.items[0].disabled === false, "stopping unlocks the setting rows");
check(L.dataList.items[L.dataList.items.length - 1].disabled === true,
      "...but Apply stays disabled with nothing pending");
atSpeed = true;
check(app._aaModBuildDataList().items.every(function (r) { return r.disabled === true; }),
      "rows rebuilt at speed (refresh, Apply) come out locked");
atSpeed = false;

// the reverse ordering: answer arrives BEFORE the template is ready
var app2 = newApp();
app2._currentContext.ctxtId = "SystemTab";
app2._populateListCtrl(app2._currentContextTemplate);
dbapiCalls = [];
app2._menuItemSelectCallback(app2._currentContextTemplate.list2Ctrl, "SelectAaMod", {itemIndex:0});
dbapiCalls[0].cb({ msgContent:{ params:{ result:0, path:"p", present:true,
    items:[{key:"touch",label:"Touch",type:"bool",value:"false"}] } } });
app2._currentContext.ctxtId = "About";
app2._currentContextTemplate = { list2Ctrl: new List2() };
app2._currentContextTemplate.list2Ctrl.setDataList(app2._contextTable.About.controlProperties.List2Ctrl.dataList);
app2._populateListCtrl(app2._currentContextTemplate);   // the ready pass
// "Touch passthrough", not the fixture's "Touch": a translated label outranks
// the plugin's English fallback, which is the intended precedence.
check(app2._currentContextTemplate.list2Ctrl.dataList.items[0].text1 === "Touch passthrough",
      "answer-before-ready still renders real rows (not the placeholder)");
check(app2._currentContextTemplate.list2Ctrl.repaints.length > 0, "...and repaints them");

// ===================== 3. toggling + Apply =============================
// Toggling records; Apply writes. The notice belongs to Apply, so flipping several
// switches is not interrupted.
controls = []; dbapiCalls = [];
var applyAt = L.dataList.items.length - 1;
check(L.dataList.items[applyAt].appData === "AaModApply", "Apply row is last");
check(L.dataList.items[applyAt].disabled === true, "Apply is disabled with nothing pending");

app._menuItemSelectCallback(L, "SetAaMod", { itemIndex: 1, additionalData: 1 });
check(dbapiCalls.length === 0, "toggling writes NOTHING on its own");
check(controls.length === 0, "...and shows no dialog");
check(L.dataList.items[applyAt].disabled === false, "Apply became enabled");
check(L.dataList.items[applyAt].text1.indexOf("1") >= 0, "Apply shows the pending count");

// A second change to a different key.
app._menuItemSelectCallback(L, "SetAaMod", { itemIndex: 2, additionalData: 1 });
check(L.dataList.items[applyAt].text1.indexOf("2") >= 0, "two pending changes counted");

// Toggling back to the file's value cancels that pending change.
app._menuItemSelectCallback(L, "SetAaMod", { itemIndex: 1, additionalData: 2 });
check(L.dataList.items[applyAt].text1.indexOf("1") >= 0, "reverting a row drops its pending change");

// Apply writes every pending key, then reports once.
dbapiCalls = []; controls = [];
app._menuItemSelectCallback(L, "AaModApply", { itemIndex: applyAt });
check(dbapiCalls.length === 1 && dbapiCalls[0].method === "SetConfig" &&
      dbapiCalls[0].params.key === "hud_transport",
      "Apply writes the one remaining pending key");
check(controls.length === 0, "no dialog until every write has answered");
dbapiCalls[0].cb({ msgContent:{ params:{ result:0 } } });
check(controls.length === 1 && controls[0]._props.text1.indexOf("NAV + MUTE") > 0,
      "reboot notice appears once, after Apply completes");
check(L.dataList.items[L.dataList.items.length-1].disabled === true,
      "Apply is disabled again once everything is written");
controls[0]._props.defaultSelectCallback(controls[0], "ok", {});

// A failed write stays pending and is named in the message.
dbapiCalls = []; controls = [];
app._menuItemSelectCallback(L, "SetAaMod", { itemIndex: 0, additionalData: 2 });
app._menuItemSelectCallback(L, "AaModApply", { itemIndex: applyAt });
check(dbapiCalls.length === 1, "Apply issues the write");
dbapiCalls[0].cb({ msgContent:{ params:{ result:1, error:"disk full" } } });
check(controls.length === 1 && controls[0]._props.text1.indexOf("disk full") > 0,
      "a failed Apply reports the reason");
check(app._aaModPendingCount() === 1, "...and the change stays pending (file unchanged)");
controls[0]._props.defaultSelectCallback(controls[0], "ok", {});
app._aaModPending = {};

// Apply with nothing pending does nothing at all.
dbapiCalls = []; controls = [];
app._menuItemSelectCallback(L, "AaModApply", { itemIndex: applyAt });
check(dbapiCalls.length === 0 && controls.length === 0, "Apply with nothing pending is inert");

// inert rows and bogus toggle indices still write nothing
dbapiCalls = [];
app._menuItemSelectCallback(L, "SetAaMod", { itemIndex: 2, additionalData: 9 });
app._menuItemSelectCallback(L, "AaModInert", { itemIndex: 0 });
check(dbapiCalls.length === 0, "bogus toggle index and inert rows write nothing");
app._aaModPending = {};

// A plugin that never answers (not installed, failed Init): the framework drops
// the callback after 60 s without calling it, so the screen must time out itself.
timers = []; dbapiCalls = []; controls = [];
app._menuItemSelectCallback(L, "SetAaMod", { itemIndex: 0, additionalData: 2 });
app._menuItemSelectCallback(L, "AaModApply", { itemIndex: applyAt });
check(dbapiCalls.length === 1 && controls.length === 0, "Apply sent, waiting for the reply");
fireTimers();
check(controls.length === 1 && controls[0]._props.text1.indexOf("no answer") > 0,
      "an unanswered SetConfig times out into the failure dialog");
dbapiCalls[0].cb({ msgContent:{ params:{ result:0 } } });
check(controls.length === 1, "...and a reply after the timeout is ignored");
controls[0]._props.defaultSelectCallback(controls[0], "ok", {});
app._aaModPending = {};

(function () {
    var t = newApp();
    t._currentContext.ctxtId = "SystemTab";
    t._populateListCtrl(t._currentContextTemplate);
    timers = []; dbapiCalls = []; controls = [];
    t._menuItemSelectCallback(t._currentContextTemplate.list2Ctrl, "SelectAaMod", { itemIndex: 0 });
    t._currentContext.ctxtId = "About";
    t._currentContextTemplate = { list2Ctrl: new List2() };
    t._currentContextTemplate.list2Ctrl.setDataList(
        t._contextTable.About.controlProperties.List2Ctrl.dataList);
    fireTimers();
    check(controls.length === 1 && controls[0]._props.text1.indexOf("Could not read") === 0,
          "an unanswered GetConfig times out into the read-error dialog");
    check(t._currentContextTemplate.list2Ctrl.dataList.items[0].text1.indexOf("Reading") !== 0,
          "...and the screen leaves the Reading placeholder");
    controls[0]._props.defaultSelectCallback(controls[0], "ok", {});
})();

// ===================== 4. leaving, and the real About ===================
app._contextTable.About.noLongerDisplayedFunction(app._currentContextTemplate, true);
check(aboutProps.dataList === "OEM_ABOUT_LIST", "leaving restores the OEM About list");
check(aboutProps.title.text1Id === "About", "leaving restores the OEM About title");
check(app._contextTable.About.noLongerDisplayedFunction === undefined, "leave-hook removed again");
check(controls.length === 0, "no dialog left behind");

app.oemSelectSeen = [];
app._menuItemSelectCallback(L, "SelectAbout", { itemIndex: 0 });
check(app.oemSelectSeen.length === 1 && app.oemSelectSeen[0] === "SelectAbout",
      "the real About row still reaches the OEM handler");
check(aboutProps.dataList === "OEM_ABOUT_LIST", "...unskinned");

// unrelated appData is passed straight through
app.oemSelectSeen = [];
app._menuItemSelectCallback(L, "SetToolTips", { itemIndex: 0, additionalData: 1 });
check(app.oemSelectSeen[0] === "SetToolTips", "unrelated rows still go to the OEM handler");

// ===================== 5. double load ==================================
var before = P._menuItemSelectCallback;
delete require.cache[require.resolve("../../resources/gui/aaModSettings.js")];
require("../../resources/gui/aaModSettings.js");
check(P._menuItemSelectCallback === before, "loading the file twice does not double-wrap");

// ===================== 6. translation ===================================
// Strings resolve at render time, so a language change needs only a repaint.
function rowsFor(lang, cfgItems) {
    curLang = lang;
    var t = newApp();
    t._currentContext.ctxtId = "SystemTab";
    t._populateListCtrl(t._currentContextTemplate);
    var entry = t._SystemTabCtxtDataList.items.filter(function(r){return r.appData==="SelectAaMod";})[0];
    dbapiCalls = [];
    t._menuItemSelectCallback(t._currentContextTemplate.list2Ctrl, "SelectAaMod", {itemIndex:0});
    t._currentContext.ctxtId = "About";
    t._currentContextTemplate = { list2Ctrl: new List2() };
    dbapiCalls[0].cb({ msgContent:{ params:{ result:0, path:"p", present:true, items: cfgItems } } });
    t._populateListCtrl(t._currentContextTemplate);
    return { app:t, entry:entry,
             title:t._contextTable.About.controlProperties.List2Ctrl.title.text1,
             rows:t._currentContextTemplate.list2Ctrl.dataList.items };
}
var CFG = [
    { key:"touch", label:"Touch passthrough", type:"bool", value:"true" },
    { key:"hud_transport", label:"HUD transport", type:"enum", value:"svcnavi",
      options:["svcnavi","vbs"], labels:["svcnavi","vbs"] },
    { key:"some_future_key", label:"Plugin English Label", type:"bool", value:"false" }
];

var en = rowsFor("LANGS_US_ENGLISH", CFG);
check(en.entry.text1 === "Android Auto Mod", "en: entry row label");
check(en.rows[0].text1 === "Touch passthrough", "en: row label from the table");
check(en.rows[1].button1 === "svcnavi" && en.rows[1].button2 === "vbs",
      "en: hud_transport buttons are the raw config values, not prose");

var cs = rowsFor("LANGS_CZ_CZECH", CFG);
check(cs.title === "Android Auto Mod", "cs: title");
check(cs.rows[0].text1 === "Ovl\u00e1d\u00e1n\u00ed dotykem", "cs: row label is Czech");
// hud_transport's options are config tokens, shown verbatim in every language.
check(cs.rows[1].button1 === "svcnavi" && cs.rows[1].button2 === "vbs",
      "cs: hud_transport buttons are the raw values too (never translated)");
check(cs.rows[1].button1 === en.rows[1].button1 && cs.rows[1].button2 === en.rows[1].button2,
      "cs: ...rendered identically to English");
check(cs.rows[0].text1 !== en.rows[0].text1, "cs differs from en (the table is actually consulted)");

// a key the translation table does not know falls back to the plugin's label
check(en.rows[2].text1 === "Plugin English Label", "unknown key falls back to the plugin label (en)");
check(cs.rows[2].text1 === "Plugin English Label", "unknown key falls back to the plugin label (cs)");

// no table at all: falls back to English, string by string
var de = rowsFor("LANGS_DE_GERMAN", CFG);
check(de.rows[0].text1 === "Touch passthrough", "untranslated language falls back to English");

// The framework stores a LOCALE in _currentLang (its startup calls
// setLanguage("en_US")), not the enum the tables are keyed on, so a Czech unit
// reported "cs_CS" and everything fell back to English. Either form must work.
var csLocale = rowsFor("cs_CS", CFG);
check(csLocale.rows[0].text1 === "Ovl\u00e1d\u00e1n\u00ed dotykem",
      "locale form \"cs_CS\" resolves to the Czech table (the form the unit stores)");
check(csLocale.entry.text1 === cs.entry.text1,
      "locale and enum forms give identical results");

// dialogs are translated and interpolated
curLang = "LANGS_CZ_CZECH";
controls = []; dbapiCalls = [];
var L2 = cs.app._currentContextTemplate.list2Ctrl;
var applyIdx = L2.dataList.items.length - 1;
cs.app._menuItemSelectCallback(L2, "SetAaMod", { itemIndex: 0, additionalData: 2 });
check(L2.dataList.items[applyIdx].text1.indexOf("Pou\u017e\u00edt") === 0,
      "cs: the Apply row is Czech");
cs.app._menuItemSelectCallback(L2, "AaModApply", { itemIndex: applyIdx });
check(dbapiCalls.length === 1, "cs: Apply issues the write");
dbapiCalls[0].cb({ msgContent:{ params:{ result:0 } } });
check(controls.length === 1 && controls[0]._props.text1.indexOf("NAV + MUTE") > 0 &&
      controls[0]._props.text1.indexOf("restartu CMU") > 0,
      "cs: reboot notice is Czech and keeps NAV + MUTE");
controls[0]._props.defaultSelectCallback(controls[0], "ok", {});

dbapiCalls = []; controls = [];
cs.app._menuItemSelectCallback(L2, "SetAaMod", { itemIndex: 0, additionalData: 1 });
cs.app._menuItemSelectCallback(L2, "AaModApply", { itemIndex: applyIdx });
dbapiCalls[0].cb({ msgContent:{ params:{ result:1, error:"disk full" } } });
var msg = controls[0]._props.text1;
check(msg.indexOf("disk full") > 0 && msg.indexOf("{0}") < 0 &&
      msg.indexOf("{1}") < 0 && msg.indexOf("{2}") < 0,
      "cs: failure message interpolates every {n}");
check(msg.indexOf("Ovl\u00e1d\u00e1n\u00ed dotykem") > 0,
      "cs: failure names the setting by its translated label");
controls[0]._props.defaultSelectCallback(controls[0], "ok", {});
cs.app._aaModPending = {};
curLang = "LANGS_US_ENGLISH";

// A translated id the English base does not define is invisible: it just falls back
// to English forever. STRINGS lives in the file's closure, so lift the literal out
// of the source and eval it — pure data, no calls.
(function auditStringIds() {
    var src = require("fs").readFileSync("resources/gui/aaModSettings.js", "utf8");
    var at  = src.indexOf("var STRINGS = {");
    if (at < 0) { check(false, "STRINGS table found in the source"); return; }
    var open = src.indexOf("{", at), depth = 0, end = -1;
    for (var k = open; k < src.length; k++) {
        if (src[k] === "{") depth++;
        else if (src[k] === "}") { depth--; if (depth === 0) { end = k; break; } }
    }
    if (end < 0) { check(false, "STRINGS table is a balanced literal"); return; }
    var table = eval("(" + src.slice(open, end + 1) + ")");

    var langs = Object.keys(table);
    check(langs.length >= 2 && langs.indexOf("LANGS_US_ENGLISH") === 0,
          "STRINGS parsed; English is the first/base table (" + langs.length + " languages)");

    var baseIds = Object.keys(table.LANGS_US_ENGLISH);

    // hud_transport's option labels must NOT be in the base: their absence is what
    // makes them untranslatable, given the audit below.
    check(baseIds.indexOf("opt.hud_transport.svcnavi") < 0 &&
          baseIds.indexOf("opt.hud_transport.vbs") < 0,
          "hud_transport option labels are absent from the base (untranslatable by construction)");
    var bad = [];
    langs.forEach(function (lang) {
        if (lang === "LANGS_US_ENGLISH") return;
        Object.keys(table[lang]).forEach(function (id) {
            if (baseIds.indexOf(id) < 0) bad.push(lang + ":" + id);
        });
    });
    check(bad.length === 0, "no translated id is missing from the English base" +
          (bad.length ? " (" + bad.join(", ") + ")" : ""));

    // Placeholders must survive translation, or fmt() leaves a hole.
    var holes = [];
    langs.forEach(function (lang) {
        baseIds.forEach(function (id) {
            var b = table.LANGS_US_ENGLISH[id], t = table[lang][id];
            if (t === undefined) return;
            ["{0}", "{1}", "{2}", "{3}"].forEach(function (ph) {
                if ((b.indexOf(ph) >= 0) !== (t.indexOf(ph) >= 0)) holes.push(lang + ":" + id + ph);
            });
        });
    });
    check(holes.length === 0, "every translation keeps its {n} placeholders" +
          (holes.length ? " (" + holes.join(", ") + ")" : ""));

    // Untranslated ids are legal, but worth reporting.
    langs.forEach(function (lang) {
        if (lang === "LANGS_US_ENGLISH") return;
        var missing = baseIds.filter(function (id) { return table[lang][id] === undefined; });
        console.log("     " + lang + ": " + (baseIds.length - missing.length) + "/" +
                    baseIds.length + " strings translated" +
                    (missing.length ? " (falls back: " + missing.join(", ") + ")" : ""));
    });
})();

// ===================== 7. schema drift =================================
// Four schemas in four files must agree, and a rebase onto a branch with different
// config keys desyncs them silently — the plugin still compiles and the screen
// still opens, it just stops offering, or mislabels, settings.
//
//   mazda/patches/common/config.h  — the keys the shims parse (truth)
//   mazda/dbapi/aamod.cpp          — the keys the screen exposes
//   resources/libpatch.conf        — the shipped sample
//   this file's STRINGS base       — the English labels
(function auditSchema() {
    var fs = require("fs"), re, m;

    var configH = fs.readFileSync("mazda/patches/common/config.h", "utf8");
    var parsed = [];
    re = /strcasecmp\(key,\s*"([a-z_0-9]+)"\)/g;
    while ((m = re.exec(configH)) !== null) if (parsed.indexOf(m[1]) < 0) parsed.push(m[1]);

    var aamod = fs.readFileSync("mazda/dbapi/aamod.cpp", "utf8");
    var schemaAt = aamod.indexOf("const Item kSchema[]");
    var schemaEnd = aamod.indexOf("\n};", schemaAt);
    var exposed = [];
    re = /\{\s*"([a-z_0-9]+)",\s*"/g;
    var body = aamod.slice(schemaAt, schemaEnd);
    while ((m = re.exec(body)) !== null) if (exposed.indexOf(m[1]) < 0) exposed.push(m[1]);

    var conf = fs.readFileSync("resources/libpatch.conf", "utf8");
    var shipped = [];
    re = /^([a-z_0-9]+)\s*=/gm;
    while ((m = re.exec(conf)) !== null) if (shipped.indexOf(m[1]) < 0) shipped.push(m[1]);

    check(parsed.length > 0 && exposed.length > 0 && shipped.length > 0,
          "all three schemas parsed (config.h " + parsed.length + ", plugin " +
          exposed.length + ", libpatch.conf " + shipped.length + ")");

    // An exposed key the shims do not parse is a toggle that writes nothing.
    var ghosts = exposed.filter(function (k) { return parsed.indexOf(k) < 0; });
    check(ghosts.length === 0,
          "every exposed key is one config.h parses" +
          (ghosts.length ? " (dead keys: " + ghosts.join(", ") + ")" : ""));

    // The reverse is a judgement call — free-text keys are deliberately file-only.
    // List them so a new key gets noticed.
    var notExposed = parsed.filter(function (k) { return exposed.indexOf(k) < 0; });
    console.log("     config.h parses " + parsed.length + " keys; screen exposes " +
                exposed.length +
                (notExposed.length ? "; file-only: " + notExposed.join(", ") : ""));

    // Without an English label the row falls back to the plugin's and no
    // translation can reach it.
    var src = fs.readFileSync("resources/gui/aaModSettings.js", "utf8");
    var at = src.indexOf("var STRINGS = {"), open = src.indexOf("{", at), depth = 0, end = -1;
    for (var k = open; k < src.length; k++) {
        if (src[k] === "{") depth++;
        else if (src[k] === "}") { depth--; if (depth === 0) { end = k; break; } }
    }
    var table = eval("(" + src.slice(open, end + 1) + ")");
    var unlabelled = exposed.filter(function (key) {
        return table.LANGS_US_ENGLISH["key." + key] === undefined;
    });
    check(unlabelled.length === 0,
          "every exposed key has an English label" +
          (unlabelled.length ? " (missing: " + unlabelled.join(", ") + ")" : ""));

    // And no label for a key that no longer exists.
    var stale = Object.keys(table.LANGS_US_ENGLISH)
                  .filter(function (id) { return id.indexOf("key.") === 0; })
                  .map(function (id) { return id.slice(4); })
                  .filter(function (key) { return exposed.indexOf(key) < 0; });
    check(stale.length === 0,
          "no label for a key the screen no longer exposes" +
          (stale.length ? " (stale: " + stale.join(", ") + ")" : ""));

    // The sample should set every key config.h knows, so a fresh install
    // documents them all.
    var undocumented = parsed.filter(function (k) { return shipped.indexOf(k) < 0; });
    check(undocumented.length === 0,
          "libpatch.conf sets every key config.h parses" +
          (undocumented.length ? " (absent: " + undocumented.join(", ") + ")" : ""));
})();

// ===================== 8. late load ====================================
// _appFinishedLoading runs _initApp and _callHandleContext in one synchronous turn,
// so the first Settings screen after a GUI start can render before this file does.
// Simulate that: a SystemTab painted by the OEM populate alone.
(function lateLoad() {
    var t = newApp();
    t._currentContext.ctxtId = "SystemTab";

    // What the OEM populate leaves on its own: rows by index + itemCount.
    var oem = ["SetToolTips","SelectChangeLanguage","SetUnitsTemperature","SetUnitsDistance",
               "SelectMusicDatabaseUpdate","SelectFactoryReset","SelectAbout"];
    t._SystemTabCtxtDataList.items = oem.map(function (a) {
        return { appData: a, text1Id: a, itemStyle: "style01" };
    });
    t._SystemTabCtxtDataList.itemCount = oem.length;
    t._SystemTabHelperArray["factoryReset"].index = 5;
    t._SystemTabHelperArray["about"].index = 6;
    t._currentContextTemplate.list2Ctrl.setDataList(t._SystemTabCtxtDataList);
    t._currentContextTemplate.list2Ctrl.repaints = [];

    var before = t._SystemTabCtxtDataList.items.filter(function (r) {
        return r.appData === "SelectAaMod"; }).length;
    check(before === 0, "late load: screen starts with no mod row (as the OEM left it)");

    var healed = t._aaModSelfHeal();
    var items = t._currentContextTemplate.list2Ctrl.dataList.items;
    check(healed === true, "late load: self-heal reports it repaired the screen");
    check(items.filter(function (r) { return r.appData === "SelectAaMod"; }).length === 1,
          "late load: the mod row is now present without a second visit");
    check(items[items.length - 1].appData === "SelectAbout", "late load: About still last");
    check(t._currentContextTemplate.list2Ctrl.repaints.length > 0, "late load: and it repainted");

    // Idempotent on an already-correct screen.
    t._aaModSelfHeal();
    check(t._currentContextTemplate.list2Ctrl.dataList.items
            .filter(function (r) { return r.appData === "SelectAaMod"; }).length === 1,
          "late load: repeating the repair does not duplicate the row");

    // No-op on any other context.
    var u = newApp();
    u._currentContext.ctxtId = "ClockTab";
    check(u._aaModSelfHeal() === false, "late load: no-op on a context we do not touch");
})();

// ===================== 9. the shipped patches ==========================
// These are applied by BusyBox 1.19.2's patch applet, on the CMU, in the file's own
// directory — much stricter than GNU patch:
//
//   * it does not strip a trailing CR from the ---/+++ filename. syssettingsApp.js
//     is CRLF, so a diff of it has CR on every payload line; a tool that also
//     CR-terminates the HEADERS makes patch open "syssettingsApp.js\r" and fail.
//   * no fuzz, and -l does not rescue a line-ending mismatch.
//
// So headers are LF-only, payload keeps the target's endings, and paths are
// basenames so `cd <dir> && patch -p1` works in place.
(function auditPatches() {
    var fs = require("fs");
    // [patch, target, target is CRLF]
    [["resources/gui/syssettingsApp.js.patch", "syssettingsApp.js", true],
     ["resources/dbapi/plugins.xml.patch",     "plugins.xml",       false]].forEach(function (pair) {
        var file = pair[0], target = pair[1], crlf = pair[2];
        var buf = fs.readFileSync(file), text = buf.toString("binary");
        var lines = text.split("\n");

        var headers = lines.filter(function (l) {
            return l.indexOf("--- ") === 0 || l.indexOf("+++ ") === 0 || l.indexOf("@@") === 0;
        });
        check(headers.length >= 3, file + ": has ---/+++/@@ headers");

        var crHeaders = headers.filter(function (l) { return l.indexOf("\r") >= 0; });
        check(crHeaders.length === 0,
              file + ": no CR in any header line (BusyBox would open '" + target + "\\r')");

        check(lines[0] === "--- a/" + target && lines[1] === "+++ b/" + target,
              file + ": paths are basenames, so -p1 works in the file's own directory");

        // Payload must carry the target's line endings — git's autocrlf strips CRs
        // silently unless .gitattributes marks the patch -text.
        var payload = lines.slice(0, -1).filter(function (l) {
            return headers.indexOf(l) < 0 && l.charAt(0) !== "\\";
        });
        var wrong = payload.filter(function (l) { return (l.slice(-1) === "\r") !== crlf; });
        check(payload.length > 0 && wrong.length === 0,
              file + ": every payload line ends in " + (crlf ? "CRLF" : "LF") +
              ", as the target does" + (wrong.length ? " (" + wrong.length + " do not)" : ""));
    });
})();

// ===================== 10. the pre-bound callback ======================
// appInit binds before this file loads, so the bound reference — and every copy of
// it in the context table — points at the original method, which wrapping the
// prototype cannot reach. Symptom was MMUI reporting an unknown event for the
// appData the OEM handler forwarded.
(function preBound() {
    var t = newApp();
    t._currentContext.ctxtId = "SystemTab";
    t._populateListCtrl(t._currentContextTemplate);
    var row = t._SystemTabCtxtDataList.items.filter(function (r) {
        return r.appData === "SelectAaMod"; })[0];
    check(!!row, "pre-bound: the mod row exists");

    // First reproduce it: click through the pre-bound value, not the prototype.
    // This must reach the OEM handler.
    t.oemSelectSeen = []; mmuiEvents = []; dbapiCalls = [];
    t.listItemClick(t._currentContextTemplate.list2Ctrl, "SelectAaMod", { itemIndex: 6 });
    check(t.oemSelectSeen.indexOf("SelectAaMod") >= 0 && mmuiEvents.length === 0,
          "pre-bound: WITHOUT the rebind the OEM handler takes the click");

    // Now the fix.
    t.oemSelectSeen = []; mmuiEvents = []; dbapiCalls = [];
    t._aaModRebindCallbacks();
    t.listItemClick(t._currentContextTemplate.list2Ctrl, "SelectAaMod", { itemIndex: 6 });

    check(t.oemSelectSeen.indexOf("SelectAaMod") < 0,
          "pre-bound: the OEM handler no longer receives SelectAaMod (no unknown event to MMUI)");
    check(mmuiEvents.length === 1 && mmuiEvents[0] === "SelectAbout",
          "pre-bound: clicking through listItemClick sends SelectAbout");
    check(dbapiCalls.length === 1 && dbapiCalls[0].method === "GetConfig",
          "pre-bound: ...and asks the plugin for the config");

    // Every captured copy must be repaired, not just the property.
    check(t._contextTable.SystemTab.controlProperties.List2Ctrl.selectCallback === t.listItemClick,
          "pre-bound: SystemTab's captured selectCallback was repaired");
    check(t._contextTable.About.controlProperties.List2Ctrl.selectCallback === t.listItemClick,
          "pre-bound: About's captured selectCallback was repaired");
    check(t._currentContextTemplate.list2Ctrl.properties.selectCallback === t.listItemClick,
          "pre-bound: the live control's selectCallback was repaired");

    // Unrelated rows still reach the OEM handler through the same path.
    t.oemSelectSeen = [];
    t.listItemClick(t._currentContextTemplate.list2Ctrl, "SetToolTips", { itemIndex: 0, additionalData: 1 });
    check(t.oemSelectSeen[0] === "SetToolTips", "pre-bound: OEM rows still reach the OEM handler");

    // Future instances get it via the wrapped appInit.
    var u = new syssettingsApp();
    u.appInitRan = false;
    check(typeof u.appInit === "function", "pre-bound: appInit is wrapped for future instances");
})();

// ===================== 11. commander on the dialog =====================
// The dialog sits on document.body, outside the template/focus system, so knob
// events go to framework.common and on to the current template — the list behind
// it. While the dialog is up we borrow that routing and hand events to
// Dialog3Ctrl's own handleControllerEvent.
(function commander() {
    var t = newApp();
    t._currentContext.ctxtId = "About";
    t._aaModActive = true;
    t._currentContextTemplate = { list2Ctrl: new List2() };

    var original = framework.common.handleControllerEvent;
    controls = []; controllerEventsToList = [];

    t._aaModShowDialog("test notice");
    check(controls.length === 1, "commander: dialog is up");
    check(framework.common.handleControllerEvent !== original,
          "commander: routing is borrowed while the dialog is up");

    // A knob turn must reach the dialog, never the list.
    framework.common.handleControllerEvent("cw", false);
    check(controls[0]._ctrlEvents.indexOf("cw") >= 0, "commander: cw reaches the dialog");
    check(controllerEventsToList.length === 0, "commander: ...and not the list underneath");

    // Dialog3Ctrl fires the focused button on press, which closes us; the trailing
    // event must be swallowed or the list activates a row.
    framework.common.handleControllerEvent("selectStart", false);
    check(controls.length === 0, "commander: select dismisses the dialog");
    framework.common.handleControllerEvent("select", false);
    check(controllerEventsToList.length === 0,
          "commander: the trailing select of the press is swallowed");

    // Routing then goes back and the next event reaches the list.
    framework.common.handleControllerEvent("cw", false);
    check(controllerEventsToList.indexOf("cw") >= 0, "commander: the list gets events again");
    check(framework.common.handleControllerEvent === original,
          "commander: routing was handed back");

    // Touch dismissal hands it back immediately.
    controllerEventsToList = [];
    t._aaModShowDialog("second notice");
    check(framework.common.handleControllerEvent !== original, "commander: borrowed again");
    controls[0]._props.defaultSelectCallback(controls[0], "AaModDialogOk", {});
    check(framework.common.handleControllerEvent === original,
          "commander: touch dismissal hands routing back at once");
    check(controls.length === 0, "commander: and the dialog is gone");
})();

console.log("\n" + (fails ? "FAILURES" : "all good") + " (" + fails + " failures)");
process.exit(fails ? 1 : 0);
