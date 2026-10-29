/*
 * aaModSettings.js — "Android Auto Mod" screen in the OEM Settings app.
 *
 * Deploys to /jci/gui/apps/syssettings/aamod/ and is pulled in by one added
 * loadScript in syssettingsApp.appInit (see syssettingsApp.js.patch); everything
 * else is done by wrapping syssettingsApp.prototype from here, so that one
 * statement is the only change to the OEM file. UTF-8, hence the explicit charset
 * on the hook — over file:// there is no HTTP charset to inherit.
 *
 * Settings > System gains one row opening a list of the mod's libpatch.conf
 * switches plus Apply. Toggling only records a change; Apply writes them and asks
 * for a reboot. Leaving without applying discards them.
 *
 * The screen is not a new MMUI context — those are MMUI's to define, and a faked
 * ctxtChange desyncs its state machine. Instead the row sends the OEM's own
 * "SelectAbout" event and the About context's list is dressed for that visit, so
 * transitions, focus stack and goBack all stay the OEM's. The real About still
 * works: the skin is applied per click, not per context.
 *
 * Values come from the libdbapiaamod jciDbapi plugin — the GUI's only route to the
 * filesystem — via framework.sendRequestToDbapi(uiaId, cb, "aamod", ...).
 * GetConfig returns the schema too, so this file renders what it is handed and
 * never names a config key.
 */

log.addSrcFile("aaModSettings.js", "syssettings");

(function ()
{
    if (typeof syssettingsApp === "undefined")
    {
        log.error("aaMod: syssettingsApp not defined — load order is wrong, screen not installed");
        return;
    }

    var P = syssettingsApp.prototype;

    if (P._aaModInstalled)
    {
        return;     // idempotent: a second load must not double-wrap the methods
    }
    P._aaModInstalled = true;

    /* ---------------------------------------------------------------- config */

    var API            = "aamod";          // == <plugin name> in /jci/dbapi/plugins.xml
    var ENTRY_APPDATA  = "SelectAaMod";    // our row in the System tab
    var ROW_APPDATA    = "SetAaMod";       // one of our setting rows
    var INERT_APPDATA  = "AaModInert";     // placeholder rows: selecting does nothing
    var APPLY_APPDATA  = "AaModApply";     // the Apply row at the bottom of the list

    /* ----------------------------------------------------------- translation */

    /*
     * We own our strings. The OEM way (text1Id + an entry in each
     * syssettingsAppDict_<locale>.js) means editing 42 dictionary files, which is
     * the wholesale change to JCI's code the single loadScript hook avoids. List
     * controls use a literal text1/label1/button1 when the *Id is absent, so this
     * works; every string is resolved at render time, so a language change needs
     * no invalidation.
     *
     * LANGS_US_ENGLISH is the base. A language may define a subset — each missing
     * string falls back to English on its own.
     *
     * TO ADD A LANGUAGE: copy the LANGS_CZ_CZECH block, change the key to its
     * LANG_CODES value, translate. Keep labels short; they render in a narrow row.
     * {0}, {1}, ... are substituted by fmt().
     */
    var BASE_LANG = "LANGS_US_ENGLISH";

    var STRINGS = {

        LANGS_US_ENGLISH : {
            "entry"                        : "Android Auto Mod",
            "title"                        : "Android Auto Mod",
            "loading"                      : "Reading libpatch.conf...",
            "empty"                        : "No settings available",
            "notice.reboot"                : "Saved to libpatch.conf. Reboot the CMU to apply: press and hold NAV + MUTE.",
            "apply"                        : "Apply",
            "apply.n"                      : "Apply ({0})",
            "err.applysome"                : "{0} of {1} settings could not be saved: {2}",
            "err.read"                     : "Could not read the mod configuration: {0}",
            "err.write"                    : "Could not save {0}: {1}",
            "err.noconf"                   : "libpatch.conf was not found at {0}. The values shown are the mod's built-in defaults; changing one creates the file.",

            // Row labels by libpatch.conf key. A key with no entry falls back to
            // the plugin's English label, so exposing a new setting stays a
            // plugin-only change.
            "key.touch"                    : "Touch passthrough",
            "key.hud"                      : "HUD guidance",
            "key.hud_transport"            : "HUD transport",
            "key.force_street_name"        : "HUD street name",
            "key.hud_fold_latin"           : "HUD accent folding",
            "key.use_protocol_v1_6"        : "AA protocol 1.6",
            "key.roundabout_prepend_exit_number"     : "Roundabout exit number",
            "key.compass_always_on"        : "Compass always on",
            "key.bt_pairing_bypass_all_devices"      : "1.6 bypass: all devices",
            "key.bt_pairing_show_device_notification" : "Show USB device name",
            "key.mute_pauses_phone"        : "Mute pauses phone",
            "key.unmute_starts_playback"   : "Unmute starts playback",
            "key.block_headunit_media_play": "Block unit play",
            "key.aa_audio_low_latency"     : "Low-latency audio",
            "key.hud_maneuver_max_distance_m" : "Show HUD maneuver from",

            // Read-only ("info") row values.
            "val.always"                   : "Always",
            "val.metres"                   : "{0} m",

            // Enum button labels would go here as "opt.<key>.<value>", same
            // fallback rule. hud_transport's are absent ON PURPOSE: "svcnavi"/"vbs"
            // are config tokens shown verbatim in every language, and the plugin
            // sends them as their own labels. That absence is what makes them
            // untranslatable — the string-id audit fails on any language defining an
            // id the base lacks. An enum whose labels are prose does belong here.
        },

        // Czech — DRAFT, not reviewed by a native speaker. Correct freely.
        LANGS_CZ_CZECH : {
            "entry"                        : "Android Auto Mod",
            "title"                        : "Android Auto Mod",
            "loading"                      : "Načítání...",
            "empty"                        : "Žádná nastavení",
            "notice.reboot"                : "Uloženo. Změna se projeví po restartu CMU: podržte NAV + MUTE.",
            "apply"                        : "Použít",
            "apply.n"                      : "Použít ({0})",
            "err.applysome"                : "{0} z {1} nastavení nelze uložit: {2}",
            "err.read"                     : "Nelze přečíst konfiguraci modu: {0}",
            "err.write"                    : "Nelze uložit {0}: {1}",
            "err.noconf"                   : "libpatch.conf nebyl nalezen v {0}. Zobrazené hodnoty jsou výchozí nastavení modu; první změnou se soubor vytvoří.",

            "key.touch"                    : "Ovládání dotykem",
            "key.hud"                      : "Navigace na HUD",
            "key.hud_transport"            : "Režim přenosu na HUD",
            "key.force_street_name"        : "Vynutit název ulice na HUD",
            "key.hud_fold_latin"           : "Rozšířená diakritika na HUD",
            "key.use_protocol_v1_6"        : "Protokol AA 1.6 (podpora pruhů)",
            "key.roundabout_prepend_exit_number"     : "Číslo výjezdu z kruhového objezdu",
            "key.compass_always_on"        : "Kompas vždy zapnutý",
            "key.bt_pairing_bypass_all_devices"      : "Obejít bezdrátové párování 1.6",
            "key.bt_pairing_show_device_notification" : "Zobrazit název bezdrát. USB zařízení",
            "key.mute_pauses_phone"        : "Ztlumení pozastaví média",
            "key.unmute_starts_playback"   : "Zrušení ztlumení vždy spustí média",
            "key.block_headunit_media_play": "Blokovat automatické přehrávání médií",
            "key.aa_audio_low_latency"     : "Zvuk s nízkou latencí",
            "key.hud_maneuver_max_distance_m" : "Zobrazit manévr na HUD od",

            "val.always"                   : "Vždy"

            // No opt.* here either — see the note in the base table.
        }
    };

    /*
     * getCurrentLanguage() is not reliably a LANG_CODES enum key: _currentLang is
     * written by both setLanguage() (documented enum) and setLang() (documented
     * locale), and the framework's own startup calls setLanguage("en_US") — a
     * locale. A Czech unit reports "cs_CS", so keying on LANGS_CZ_CZECH alone gave
     * English everywhere. Accept either form via LANG_CODES.
     */
    var loggedLang = false;

    function currentLangKey()
    {
        var L, raw;
        try
        {
            L   = framework.localize;
            raw = L.getCurrentLanguage();
        }
        catch (err)
        {
            return null;    // pre-init or a stubbed framework: English it is
        }
        if (!raw)
        {
            return null;
        }

        if (!loggedLang)
        {
            loggedLang = true;
            log.info("aaMod: language reported as \"" + raw + "\"");
        }

        if (STRINGS[raw])
        {
            return raw;                     // already the form the tables use
        }
        if (L.LANG_CODES)
        {
            // locale ("cs_CS") -> enum ("LANGS_CZ_CZECH")
            for (var k in L.LANG_CODES)
            {
                if (L.LANG_CODES[k] === raw && STRINGS[k])
                {
                    return k;
                }
            }
            // enum -> locale, for a table keyed the other way round
            if (L.LANG_CODES[raw] && STRINGS[L.LANG_CODES[raw]])
            {
                return L.LANG_CODES[raw];
            }
        }
        return raw;
    }

    // Current language, then English, then `deflt` or the id itself — a missing
    // string should be visible, not blank.
    function tr(id, deflt)
    {
        var lang = currentLangKey();

        if (lang && STRINGS[lang] && STRINGS[lang][id] !== undefined)
        {
            return STRINGS[lang][id];
        }
        if (STRINGS[BASE_LANG][id] !== undefined)
        {
            return STRINGS[BASE_LANG][id];
        }
        return (deflt !== undefined && deflt !== null) ? deflt : id;
    }

    // Positional substitution. Trivial on purpose: the OEM's subMap belongs to
    // getLocStr, which we do not go through.
    function fmt(id)
    {
        var out = tr(id);
        for (var i = 1; i < arguments.length; i++)
        {
            if (arguments[i] !== undefined)
            {
                out = out.replace("{" + (i - 1) + "}", arguments[i]);
            }
        }
        return out;
    }

    /* --------------------------------------------------------------- helpers */

    function currentCtxtId(app)
    {
        return app._currentContext ? app._currentContext.ctxtId : null;
    }

    // The List2 of the context on screen right now, or null.
    function currentList(app)
    {
        var t = app._currentContextTemplate;
        return (t && t.list2Ctrl) ? t.list2Ctrl : null;
    }

    function repaint(listCtrl)
    {
        // setDataList swaps the model; updateItems is what redraws the rows —
        // the OEM populate path relies on the same pair.
        var count = listCtrl.dataList ? listCtrl.dataList.items.length : 0;
        if (count > 0)
        {
            listCtrl.updateItems(0, count - 1);
        }
    }

    /* -------------------------------------------------- the setting row model */

    // What a row displays: the pending edit if any, else the file's value.
    P._aaModShownValue = function (item)
    {
        if (this._aaModPending && this._aaModPending.hasOwnProperty(item.key))
        {
            return this._aaModPending[item.key];
        }
        return item.value;
    };

    P._aaModPendingCount = function ()
    {
        var n = 0;
        for (var k in (this._aaModPending || {}))
        {
            ++n;
        }
        return n;
    };

    // 1-based index of the displayed value, as the List2 toggle styles want.
    P._aaModEnumIndex = function (item)
    {
        var shown = this._aaModShownValue(item);
        if (item.options)
        {
            for (var i = 0; i < item.options.length; i++)
            {
                if (item.options[i] === shown)
                {
                    return i + 1;
                }
            }
        }
        return 1;
    };

    // Our translation if we have one, else the plugin's English label.
    function optLabel(item, i)
    {
        return tr("opt." + item.key + "." + item.options[i], item.labels[i]);
    }

    P._aaModRow = function (item, index)
    {
        var row = {
            appData    : ROW_APPDATA,
            aaModIndex : index,          // our own field; List2 passes items through
            // Literal text, not text1Id — see the translation block above.
            text1      : tr("key." + item.key, item.label),
            hasCaret   : false
        };

        if (item.type === "enum" && item.options && item.labels)
        {
            // style10 = two buttons, style11 = three. More has no widget, so show
            // it read-only rather than guess.
            if (item.options.length === 2)
            {
                row.itemStyle = "style10";
                row.button1   = optLabel(item, 0);
                row.button2   = optLabel(item, 1);
                row.value     = this._aaModEnumIndex(item);
                return row;
            }
            if (item.options.length === 3)
            {
                row.itemStyle = "style11";
                row.button1   = optLabel(item, 0);
                row.button2   = optLabel(item, 1);
                row.button3   = optLabel(item, 2);
                row.value     = this._aaModEnumIndex(item);
                return row;
            }

            row.appData   = INERT_APPDATA;
            row.itemStyle = "style06";
            row.label1    = item.value;
            return row;
        }

        // Read-only number in metres; 0 = always shown. Selecting it does nothing.
        if (item.type === "info")
        {
            row.appData   = INERT_APPDATA;
            row.itemStyle = "style06";
            row.label1    = (item.value === "0") ? tr("val.always", "Always")
                                                 : fmt("val.metres", item.value);
            return row;
        }

        row.itemStyle = "styleOnOff";
        row.value     = (this._aaModShownValue(item) === "true") ? 1 : 2;  // 1 On, 2 Off
        return row;
    };

    P._aaModBuildDataList = function ()
    {
        var items = [];

        if (!this._aaModItems)
        {
            // GetConfig still in flight, or it failed and said so in a dialog.
            items.push({
                appData   : INERT_APPDATA,
                text1     : tr("loading"),
                itemStyle : "style01",
                hasCaret  : false,
                disabled  : true
            });
        }
        else if (this._aaModItems.length === 0)
        {
            items.push({
                appData   : INERT_APPDATA,
                text1     : tr("empty"),
                itemStyle : "style01",
                hasCaret  : false,
                disabled  : true
            });
        }
        else
        {
            for (var i = 0; i < this._aaModItems.length; i++)
            {
                items.push(this._aaModRow(this._aaModItems[i], i));
            }

            // Apply, last. Disabled until something is pending — a greyed row says
            // that without needing a message.
            var n = this._aaModPendingCount();
            items.push({
                appData   : APPLY_APPDATA,
                text1     : n > 0 ? fmt("apply.n", n) : tr("apply"),
                itemStyle : "style01",
                hasCaret  : false,
                disabled  : n === 0
            });
        }

        applySpeedLock(items, framework.common.getAtSpeedValue());
        return { itemCountKnown : true, itemCount : items.length, items : items };
    };

    // At speed every row is locked, as the OEM locks every row of the real About.
    // aaModDisabled keeps a row's own state (Apply with nothing pending, the
    // placeholders), so the lock lifting when the car stops does not enable them.
    function applySpeedLock(items, atSpeed)
    {
        for (var i = 0; i < items.length; i++)
        {
            if (items[i].aaModDisabled === undefined)
            {
                items[i].aaModDisabled = !!items[i].disabled;
            }
            items[i].disabled = items[i].aaModDisabled || !!atSpeed;
        }
    }

    /* ------------------------------------------------- borrowing About's list */

    // Dress/undress the About context. Must run BEFORE the transition: the
    // framework builds the template from controlProperties, then calls
    // readyFunction — changing them later is too late.
    P._aaModSkinAbout = function (active)
    {
        var props = this._contextTable.About.controlProperties.List2Ctrl;

        if (active)
        {
            if (!this._aaModAboutSaved)
            {
                this._aaModAboutSaved = {
                    title                  : props.title,
                    dataList               : props.dataList,
                    rotationIdleDetectTime : props.rotationIdleDetectTime,
                    toggleMinChangeInterval: props.toggleMinChangeInterval,
                    toggleSettleTime       : props.toggleSettleTime,
                    noLongerDisplayedFn    : this._contextTable.About.noLongerDisplayedFunction
                };
            }

            // titleStyle02 localises text1 only when text1Id is set.
            props.title    = { titleStyle : "style02", text1 : tr("title") };
            props.dataList = this._aaModBuildDataList();

            // About has no toggle rows, so none of the debounce properties items
            // inherit. Borrow the System tab's.
            props.rotationIdleDetectTime  = 1250;
            props.toggleMinChangeInterval = 1000;
            props.toggleSettleTime        = 1500;

            this._contextTable.About.noLongerDisplayedFunction =
                this._aaModAboutNoLongerDisplayed.bind(this);
        }
        else if (this._aaModAboutSaved)
        {
            props.title                   = this._aaModAboutSaved.title;
            props.dataList                = this._aaModAboutSaved.dataList;
            props.rotationIdleDetectTime  = this._aaModAboutSaved.rotationIdleDetectTime;
            props.toggleMinChangeInterval = this._aaModAboutSaved.toggleMinChangeInterval;
            props.toggleSettleTime        = this._aaModAboutSaved.toggleSettleTime;
            this._contextTable.About.noLongerDisplayedFunction =
                this._aaModAboutSaved.noLongerDisplayedFn;
            this._aaModAboutSaved = null;
        }

        this._aaModActive = active;
    };

    // Leaving: drop the skin so the real About row shows the real About.
    P._aaModAboutNoLongerDisplayed = function (params)
    {
        log.info("aaMod: leaving the mod screen");
        this._aaModHideDialog();
        var unapplied = this._aaModPendingCount();
        if (unapplied > 0)
        {
            log.warn("aaMod: left with " + unapplied
                     + " unapplied change(s) — discarded, nothing was written");
        }
        this._aaModPending     = {};
        this._aaModSkinAbout(false);
    };

    /* ----------------------------------------------------------- dbapi bridge */

    // The framework drops a dbapi callback that never fired after 60 s without
    // calling it, so a plugin that does not answer (not installed, failed to load)
    // would leave the screen on "Reading..." and Apply waiting for good. Answer
    // null ourselves first; a reply arriving after that is ignored.
    var REPLY_TIMEOUT_MS = 10000;

    function requestDbapi(app, method, params, cb)
    {
        var settled = false;
        var timer = setTimeout(function () {
            if (settled)
            {
                return;
            }
            settled = true;
            log.error("aaMod: " + method + ": no reply within " + REPLY_TIMEOUT_MS + " ms");
            cb(null);
        }, REPLY_TIMEOUT_MS);

        framework.sendRequestToDbapi(app.uiaId, function (msg) {
            if (settled)
            {
                log.warn("aaMod: " + method + ": reply after the timeout, ignored");
                return;
            }
            settled = true;
            clearTimeout(timer);
            cb(msg);
        }, API, method, params);
    }

    P._aaModRequestConfig = function ()
    {
        // Re-read every entry: the file is editable over ssh too, and unapplied
        // edits are dropped with it — nothing was written.
        this._aaModItems   = null;
        this._aaModPending = {};
        requestDbapi(this, "GetConfig", {}, this._aaModGetConfigCb.bind(this));
    };

    P._aaModGetConfigCb = function (msg)
    {
        var params = (msg && msg.msgContent) ? msg.msgContent.params : null;

        if (!params || params.result !== 0 || !params.items)
        {
            var why = (params && params.error) ? params.error
                                               : "no answer from the aamod dbapi plugin";
            log.error("aaMod: GetConfig failed: " + why);
            this._aaModItems = [];
            this._aaModRefresh();
            this._aaModShowDialog(fmt("err.read", why));
            return;
        }

        this._aaModItems = params.items;
        log.info("aaMod: GetConfig gave " + params.items.length + " items from "
                 + params.path + (params.present ? "" : " (absent — showing defaults)"));

        this._aaModRefresh();

        if (!params.present)
        {
            this._aaModShowDialog(fmt("err.noconf", params.path));
        }
    };

    // Rebuild if our screen is the one on display.
    P._aaModRefresh = function ()
    {
        if (!this._aaModActive || currentCtxtId(this) !== "About")
        {
            return;     // answer outran the transition, or we already left
        }
        var list = currentList(this);
        if (!list)
        {
            return;
        }
        var dataList = this._aaModBuildDataList();
        this._contextTable.About.controlProperties.List2Ctrl.dataList = dataList;
        list.setDataList(dataList);
        repaint(list);
    };




    /* ------------------------------------------------------------- the dialog */

    // A dialog is normally its own MMUI context, which we cannot reach — but a
    // control can be instantiated straight onto document.body, as Common.js does
    // for the VUI help overlay. Both controls are in the list Common.js registers
    // at startup, so neither can be missing.
    P._aaModShowDialog = function (text)
    {
        this._aaModHideDialog();

        this._aaModDialog = framework.instantiateControl(
            this.uiaId, document.body, "Dialog3Ctrl", {
                contentStyle          : "style02",   // one text block, wraps to 5 lines
                fullScreen            : true,
                buttonCount           : 1,
                buttonConfig          : { button1 : { labelId : "common.Ok",
                                                      appData : "AaModDialogOk" } },
                defaultSelectCallback : this._aaModDialogSelect.bind(this),
                text1                 : text
            });

        if (this._aaModDialog)
        {
            // Being outside the template system, we get no commander events:
            // framework.common.handleControllerEvent routes them to the current
            // template, i.e. the list underneath. So borrow that routing while we
            // are up — Dialog3Ctrl handles cw/ccw/select itself.
            this._aaModGrabController();
        }

        if (this._aaModDialog && this._aaModDialog.divElt)
        {
            // Same lift Common.js gives its template-less dialog.
            this._aaModDialog.divElt.style.zIndex = 13;
            return;
        }

        // Should not happen, but the message matters more than its shape.
        log.warn("aaMod: Dialog3Ctrl unavailable — falling back to a wink");
        this._aaModDialog = null;
        this._aaModWink = framework.instantiateControl(
            this.uiaId, document.body, "WinkCtrl", {
                style            : "style04",
                winkTimeout      : 6000,
                text1            : tr("title"),
                text2            : text,
                completeCallback : this._aaModWinkComplete.bind(this)
            });
    };

    // Route commander events to the dialog while it is up.
    P._aaModGrabController = function ()
    {
        var common = framework.common;
        if (this._aaModCtrlSaved || !common ||
            typeof common.handleControllerEvent !== "function")
        {
            return;
        }

        var self = this;
        this._aaModCtrlSaved = common.handleControllerEvent;

        common.handleControllerEvent = function (eventId, firstEvent) {
            if (self._aaModDialog)
            {
                // cw/ccw move between buttons; select fires the focused one,
                // which closes us.
                try
                {
                    self._aaModInRoutedEvent = true;
                    self._aaModDialog.handleControllerEvent(eventId);
                    self._aaModInRoutedEvent = false;
                }
                catch (err)
                {
                    self._aaModInRoutedEvent = false;
                    log.error("aaMod: dialog controller event failed");
                    log.writeError(err);
                    self._aaModHideDialog();
                }
                return;                     // never reaches the list underneath
            }

            // Dismissed during this press. Dialog3Ctrl fires on both selectStart
            // and select, so swallow the rest — the trailing select would activate
            // whatever row the list has focused.
            if (eventId === "select" || eventId === "selectStart")
            {
                return;
            }

            var saved = self._aaModCtrlSaved;
            self._aaModReleaseController();
            return saved.call(common, eventId, firstEvent);
        };
    };

    P._aaModReleaseController = function ()
    {
        if (!this._aaModCtrlSaved)
        {
            return;
        }
        if (framework.common)
        {
            framework.common.handleControllerEvent = this._aaModCtrlSaved;
        }
        this._aaModCtrlSaved = null;
    };

    P._aaModHideDialog = function ()
    {
        // Inside a routed event the wrapper releases it once the press finishes;
        // releasing here would hand the trailing select to the list.
        if (!this._aaModInRoutedEvent)
        {
            this._aaModReleaseController();
        }

        if (this._aaModDialog)
        {
            framework.destroyControl(this._aaModDialog);
            this._aaModDialog = null;
        }
        if (this._aaModWink)
        {
            framework.destroyControl(this._aaModWink);
            this._aaModWink = null;
        }
    };

    P._aaModDialogSelect = function (dialogCtrl, appData, params)
    {
        this._aaModHideDialog();
    };

    P._aaModWinkComplete = function ()
    {
        if (this._aaModWink)
        {
            framework.destroyControl(this._aaModWink);
            this._aaModWink = null;
        }
    };

    /* ------------------------------------------ wrap: the System tab list row */

    var origPopulateListCtrl = P._populateListCtrl;

    P._populateListCtrl = function (tmplt)
    {
        var ctxtId = currentCtxtId(this);

        // The OEM sets _isListChanged by comparing the item count before/after.
        // Our extra row would make that differ every visit and defeat focus
        // restore, so settle the flag ourselves once the row is in.
        var countBefore = (ctxtId === "SystemTab" && tmplt.list2Ctrl.dataList)
                            ? tmplt.list2Ctrl.dataList.itemCount : -1;

        origPopulateListCtrl.call(this, tmplt);

        if (ctxtId === "SystemTab")
        {
            this._aaModAddSystemTabRow(tmplt, countBefore);
        }
        else if (ctxtId === "About" && this._aaModActive)
        {
            // Not redundant with the skin's dataList: the control rendered from
            // those properties at construction, so if GetConfig answered before
            // the template was ready the rows are still the placeholder.
            var dataList = this._aaModBuildDataList();
            this._contextTable.About.controlProperties.List2Ctrl.dataList = dataList;
            tmplt.list2Ctrl.setDataList(dataList);
            repaint(tmplt.list2Ctrl);
        }
    };

    P._aaModAddSystemTabRow = function (tmplt, countBefore)
    {
        var list = this._SystemTabCtxtDataList;
        if (!list || !list.items)
        {
            return;
        }

        // The OEM populate assigns by index and sets itemCount without shortening
        // items[], so itemCount is authoritative. Our splice lengthens the array,
        // so without this the row from last visit survives past itemCount and the
        // splice below adds a duplicate About on every visit.
        list.items.length = list.itemCount;

        // Before the OEM About row, not appended: _EnableDisableControlItems
        // treats items[itemCount - 1] as About for the at-speed lockout.
        var at = -1;
        for (var i = 0; i < list.itemCount; i++)
        {
            if (list.items[i] && list.items[i].appData === "SelectAbout")
            {
                at = i;
                break;
            }
        }
        if (at < 0)
        {
            at = list.itemCount;    // no About row in this build — go last
        }

        // Locked here, not only in the EnableDisable wrapper: the OEM populate runs
        // its lockout before returning, i.e. before this row exists.
        list.items.splice(at, 0, {
            appData   : ENTRY_APPDATA,
            text1     : tr("entry"),
            itemStyle : "style01",
            hasCaret  : false,
            disabled  : !!framework.common.getAtSpeedValue()
        });
        list.itemCount = list.items.length;
        this._aaModSystemTabIndex = at;

        // Keep the OEM's index bookkeeping truthful, About having moved down.
        if (this._SystemTabHelperArray && this._SystemTabHelperArray["about"] &&
            at <= this._SystemTabHelperArray["about"].index)
        {
            this._SystemTabHelperArray["about"].index += 1;
        }

        // Both calls are needed. setDataList() recomputes the scroller geometry
        // for the new itemCount but also calls _emptyScroller(), returning every
        // rendered row to the pool; updateItems() is the only thing that builds row
        // DOM. The OEM gets away with setDataList alone because the control's
        // constructor already ran init() -> updateItems(). Omit the rebuild and the
        // list stays empty until a scroll, with no row DOM to tap.
        tmplt.list2Ctrl.setDataList(list);
        if (list.itemCount > 0)
        {
            tmplt.list2Ctrl.updateItems(0, list.itemCount - 1);
        }
        log.info("aaMod: System tab row at index " + at + " of " + list.itemCount
                 + " (rows rebuilt)");

        if (countBefore >= 0)
        {
            this._isListChanged = (countBefore !== list.itemCount);
        }
    };

    /* --------------------------------- wrap: at-speed lockout for our own row */

    var origEnableDisable = P._EnableDisableControlItems;

    P._EnableDisableControlItems = function ()
    {
        origEnableDisable.apply(this, arguments);

        var list = currentList(this);

        // The OEM About branch sets every row to the bare speed flag, which on
        // stopping would enable Apply with nothing pending. Re-derive each row.
        if (currentCtxtId(this) === "About" && this._aaModActive)
        {
            if (list && list.dataList && list.dataList.items &&
                list.dataList.items.length)
            {
                applySpeedLock(list.dataList.items, this._disableSpeedRestricted);
                list.updateItems(0, list.dataList.items.length - 1);
            }
            return;
        }

        // The OEM SystemTab branch locks only the rows it knows; opt ours in too.
        if (currentCtxtId(this) !== "SystemTab")
        {
            return;
        }
        var at   = this._aaModSystemTabIndex;
        if (!list || !list.dataList || at === undefined || !list.dataList.items[at] ||
            list.dataList.items[at].appData !== ENTRY_APPDATA)
        {
            return;
        }
        list.dataList.items[at].disabled = this._disableSpeedRestricted;
        list.updateItems(at, at);
    };

    /* --------------------------------------------------- wrap: item selection */

    var origMenuItemSelect = P._menuItemSelectCallback;

    P._menuItemSelectCallback = function (listCtrlObj, appData, params)
    {
        if (appData === ENTRY_APPDATA)
        {
            log.info("aaMod: entry row selected");
            // Dress About, ask for values, then let MMUI do its ordinary
            // SystemTab -> About transition. "SelectAbout" is the event the OEM row
            // sends, so MMUI sees nothing unusual.
            log.info("aaMod: opening the mod screen");
            this._aaModSkinAbout(true);
            this._aaModRequestConfig();
            framework.sendEventToMmui(this.uiaId, "SelectAbout");
            return;
        }

        if (appData === ROW_APPDATA)
        {
            log.info("aaMod: setting row " + params.itemIndex + " selected, choice "
                     + params.additionalData);
            this._aaModOnRowSelect(listCtrlObj, params);
            return;
        }

        if (appData === APPLY_APPDATA)
        {
            log.info("aaMod: Apply selected");
            this._aaModApply();
            return;
        }

        if (appData === INERT_APPDATA)
        {
            return;     // placeholder / read-only row
        }

        if (appData === "SelectAbout")
        {
            // The genuine About row: undress first.
            this._aaModSkinAbout(false);
        }

        return origMenuItemSelect.call(this, listCtrlObj, appData, params);
    };

    P._aaModOnRowSelect = function (listCtrlObj, params)
    {
        var itemIndex = params.itemIndex;
        var row = (listCtrlObj && listCtrlObj.dataList)
                    ? listCtrlObj.dataList.items[itemIndex] : null;
        if (!row || row.aaModIndex === undefined || !this._aaModItems)
        {
            log.warn("aaMod: row select with no backing item at index " + itemIndex);
            return;
        }

        var item = this._aaModItems[row.aaModIndex];
        if (!item)
        {
            return;
        }

        // additionalData is the 1-based toggle position: styleOnOff 1 = On,
        // 2 = Off; style10/11 the button number.
        var choice   = params.additionalData;
        var oldValue = item.value;
        var newValue;

        if (item.type === "enum")
        {
            if (!item.options || choice < 1 || choice > item.options.length)
            {
                log.warn("aaMod: " + item.key + " toggled to out-of-range " + choice);
                return;
            }
            newValue = item.options[choice - 1];
        }
        else if (choice === 1 || choice === 2)
        {
            newValue = (choice === 1) ? "true" : "false";
        }
        else
        {
            log.warn("aaMod: " + item.key + " toggled to unexpected " + choice);
            return;
        }

        // Record only; Apply writes. Selecting the file's current value clears any
        // pending edit rather than queueing a no-op.
        if (!this._aaModPending)
        {
            this._aaModPending = {};
        }
        if (newValue === item.value)
        {
            delete this._aaModPending[item.key];
        }
        else
        {
            this._aaModPending[item.key] = newValue;
        }

        log.info("aaMod: " + item.key + " -> " + newValue + " (pending "
                 + this._aaModPendingCount() + ")");

        // The row already shows the new position; Apply's label/state must catch up.
        this._aaModRefreshApplyRow();
    };

    // Repaint just the Apply row (always last).
    P._aaModRefreshApplyRow = function ()
    {
        var list = currentList(this);
        if (!list || !list.dataList || !list.dataList.items.length)
        {
            return;
        }
        var at  = list.dataList.items.length - 1;
        var row = list.dataList.items[at];
        if (!row || row.appData !== APPLY_APPDATA)
        {
            return;
        }
        var n = this._aaModPendingCount();
        row.text1         = n > 0 ? fmt("apply.n", n) : tr("apply");
        row.aaModDisabled = (n === 0);
        row.disabled      = row.aaModDisabled || !!framework.common.getAtSpeedValue();
        list.updateItems(at, at);
    };

    /* --------------------------------------------------------------- applying */

    P._aaModApply = function ()
    {
        var keys = [];
        for (var k in (this._aaModPending || {}))
        {
            keys.push(k);
        }
        if (!keys.length)
        {
            return;     // the row is disabled in this state anyway
        }

        log.info("aaMod: applying " + keys.length + " change(s)");

        // One SetConfig per key; the plugin serialises them on its single thread.
        // Wait for every reply before saying anything.
        var self      = this;
        var total     = keys.length;
        var outstanding = total;
        var failures  = [];

        var done = function () {
            if (--outstanding > 0)
            {
                return;
            }
            self._aaModRefresh();           // rebuild rows from the new values
            self._aaModRefreshApplyRow();
            if (failures.length === 0)
            {
                log.info("aaMod: applied " + total + " change(s)");
                self._aaModShowDialog(tr("notice.reboot"));
            }
            else
            {
                log.error("aaMod: " + failures.length + " of " + total
                          + " change(s) failed: " + failures.join("; "));
                self._aaModShowDialog(fmt("err.applysome", failures.length, total,
                                          failures.join("; ")));
            }
        };

        for (var i = 0; i < keys.length; i++)
        {
            (function (key) {
                var value = self._aaModPending[key];
                var item  = null;
                for (var j = 0; j < (self._aaModItems || []).length; j++)
                {
                    if (self._aaModItems[j].key === key) { item = self._aaModItems[j]; }
                }
                requestDbapi(self, "SetConfig", { key : key, value : value }, function (msg) {
                    var params = (msg && msg.msgContent) ? msg.msgContent.params : null;
                    if (params && params.result === 0)
                    {
                        if (item) { item.value = value; }
                        delete self._aaModPending[key];
                        log.info("aaMod: wrote " + key + " = " + value);
                    }
                    else
                    {
                        var why = (params && params.error) ? params.error
                                                           : "no answer from the aamod plugin";
                        // Stays pending: the file did not change — or, after a
                        // timeout, its state is unknown; re-entering re-reads it.
                        failures.push((item ? tr("key." + key, item.label) : key) + ": " + why);
                    }
                    done();
                });
            })(keys[i]);
        }
    };

    /* --------------------------------------- re-bind the pre-bound callback */

    /*
     * Wrapping the prototype is not enough for list clicks. appInit binds once and
     * passes the bound value around —
     *
     *     this.listItemClick = this._menuItemSelectCallback.bind(this);
     *     ... { "List2Ctrl": { selectCallback : this.listItemClick } }
     *
     * — and appInit is where our loadScript sits, so that bind captured the
     * ORIGINAL method and every list context copied it. The OEM handler kept the
     * clicks and forwarded appData "SelectAaMod" to MMUI as an unknown event.
     *
     * The other wrappers are fine: readyFunctions look up _populateListCtrl and
     * _EnableDisableControlItems at call time. Only pre-bound values need this.
     */
    P._aaModRebindCallbacks = function ()
    {
        var stale   = this.listItemClick;
        var wrapped = this._menuItemSelectCallback.bind(this);  // now the wrapper

        this.listItemClick = wrapped;

        // Contexts captured the old value when appInit built the table.
        var fixed = 0;
        for (var ctxtId in this._contextTable)
        {
            var props = this._contextTable[ctxtId].controlProperties;
            if (props && props.List2Ctrl && props.List2Ctrl.selectCallback === stale)
            {
                props.List2Ctrl.selectCallback = wrapped;
                ++fixed;
            }
        }

        // A control already on screen holds it in its own properties.
        var live = currentList(this);
        if (live && live.properties && live.properties.selectCallback === stale)
        {
            live.properties.selectCallback = wrapped;
            ++fixed;
        }

        log.info("aaMod: rebound list select callback (" + fixed + " captured copies fixed)");
    };

    // Future instances: appInit builds the table and does the binding, so redo ours
    // right after it.
    var origAppInit = P.appInit;
    P.appInit = function ()
    {
        origAppInit.apply(this, arguments);
        this._aaModRebindCallbacks();
    };

    /* ------------------------------------------------ late-load self-repair */

    /*
     * loadScript appends a <script> tag, but _appFinishedLoading() runs
     *
     *     this._initApp(uiaId)          // -> appInit() -> our loadScript
     *     this._callHandleContext(msg)  // -> template -> readyFunction -> populate
     *
     * in one synchronous turn, and an appended script cannot execute until that turn
     * yields. Apps load lazily, so the first Settings screen after a GUI start can
     * render before this file runs — missing our row until re-entered. Whether it
     * happens depends on the template also needing a fetch, so repair it rather
     * than rely on that.
     */
    P._aaModSelfHeal = function ()
    {
        var ctxtId = currentCtxtId(this);
        if (ctxtId !== "SystemTab")
        {
            return false;   // About cannot be skinned yet
        }
        var list = currentList(this);
        if (!list)
        {
            return false;
        }

        log.info("aaMod: SystemTab rendered before this file loaded — repopulating");
        this._populateListCtrl(this._currentContextTemplate);
        repaint(list);

        // Re-apply the at-speed lockout; it ran once on a list without our row.
        if (typeof this._EnableDisableControlItems === "function")
        {
            this._EnableDisableControlItems();
        }
        return true;
    };

    (function repairExistingInstance() {
        // The instance that pulled us in has already bound, so the wrapped appInit
        // above never runs for it.
        var app = null;
        try
        {
            app = framework.getAppInstance("syssettings");
        }
        catch (err)
        {
            app = null;     // framework not far enough along
        }
        if (!app || typeof app._aaModRebindCallbacks !== "function")
        {
            return;
        }

        app._aaModRebindCallbacks();
        app._aaModSelfHeal();
    })();

    log.info("aaMod: settings screen installed");
})();
