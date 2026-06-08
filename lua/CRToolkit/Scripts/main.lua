--[[ ============================================================================
  CRToolkit — Cyber Rats reverse-engineering / introspection mod (Milestone M0)

  Zweck: das Spiel zur Laufzeit untersuchen, um die exakten UFunction- und
  Property-Namen fuer die Koop-Mod zu finden, und zu pruefen, ob die Maze-
  Generierung bei gleichem Seed deterministisch ist.

  KEINE Gameplay-Aenderung. Reine Diagnose.

  Bedienung (Keybinds, Ausgabe geht in die UE4SS GUI-Console + UE4SS.log):
    F5  : lokalen PlayerController + Pawn (BP_LabRat) dumpen
    F6  : Maze_Generator dumpen (Properties + Funktionen; Seed/Random hervorgehoben)
    F7  : GameInstance_LabRats + GameMode_LabRats dumpen
    F8  : Pickups (Chese_Pickup) + Cyborg-Spawner + BP_EnterExit dumpen
    F9  : Maze-Layout-Fingerprint berechnen (Determinismus-Test)
    F10 : Seed auf CONFIG.forcedSeed setzen (Prop-Name in CONFIG.mazeSeedProp)
    F11 : `Dumper_Usmap`-Hinweis + Klassenliste eines Suchbegriffs (CONFIG.searchTerm)

  Determinismus-Test (Ziel von M0):
    1) In ein Maze laufen, F6 druecken -> Seed-Property + Generate-Funktion ablesen,
       Namen in CONFIG unten eintragen.
    2) F10 (Seed forcieren) ist optional, falls neu generiert werden kann.
    3) F9 druecken -> Fingerprint A notieren. Run neu starten/Maze neu laden mit
       GLEICHEM Seed -> F9 -> Fingerprint B. A == B  => deterministisch (Seed-Sync
       reicht). A != B => Layout-Stream-Fallback noetig.
============================================================================ ]]

local UEHelpers = require("UEHelpers")

-- ---------------------------------------------------------------------------
-- CONFIG — beim Erkunden anpassen (Namen aus F6/F8-Dumps hier eintragen)
-- ---------------------------------------------------------------------------
local CONFIG = {
    -- Spielklassen (kurze Namen fuer FindFirstOf/FindAllOf; "_C" = Blueprint-Klasse)
    pawnClass        = "BP_LabRat_C",
    controllerClass  = "Player_Controller_CyberRats_C",
    gameInstanceClass= "GameInstance_LabRats_C",
    gameModeClass    = "GameMode_LabRats_C",
    mazeGenClass     = "Maze_Generator_C",

    -- Werden nach F6 bekannt — dann hier eintragen:
    mazeSeedProp     = "Seed",        -- Kandidat; F6 zeigt den echten Namen
    forcedSeed       = 1337,          -- fuer F10 / Determinismus-Test

    -- Klassen, deren Instanzen das Maze-Layout ausmachen (fuer F9-Fingerprint).
    -- Aus dem Log bekannt: BP_New_EndWall_C. Bei Bedarf erweitern.
    mazePieceClasses = {
        "BP_New_EndWall_C",
        "Master_Maze_Room_C",
        "BP_EnterExit_C",
    },

    -- Pickup / Gegner / Ziel — fuer F8.
    pickupClasses = { "Chese_Pickup_C", "Last_Chese_Pickup_C" },
    spawnerClasses = {
        "BP_Cyborg_Spawner_C",
        "BP_GatlingCyborg_Spawner_v2_C",
        "BP_PropellerCyborg_Spawner_v2_C",
        "BP_SpiderCyborg_Spawner_v2_C",
        "BP_TrapSpawner_OnBegin_C",
    },
    objectiveClasses = { "BP_EnterExit_C" },

    searchTerm = "LabRat",   -- fuer F11

    maxSuperDepth = 4,       -- wie weit die Vererbungskette hochgedumpt wird
}

-- ---------------------------------------------------------------------------
-- Logging-Helfer
-- ---------------------------------------------------------------------------
-- Dedizierte Dump-Datei (sauberer als die laute UE4SS.log). io kann gesandboxt sein -> pcall.
local DUMP_FILE = "F:\\Projects\\Mods\\CyberRats\\extracted\\crtoolkit.log"
local function fileWrite(s)
    pcall(function()
        local fh = io.open(DUMP_FILE, "a")
        if fh then fh:write(s); fh:write("\n"); fh:close() end
    end)
end

local function log(fmt, ...)
    local ok, s = pcall(string.format, fmt, ...)
    if not ok then s = tostring(fmt) end
    print("[CRToolkit] " .. s .. "\n")
    fileWrite("[CRToolkit] " .. s)
end

local function rule(title)
    print("\n[CRToolkit] ========== " .. tostring(title) .. " ==========\n")
    fileWrite("\n[CRToolkit] ========== " .. tostring(title) .. " ==========")
end

-- pcall-Wrapper, gibt nil zurueck statt zu crashen
local function try(fn)
    local ok, res = pcall(fn)
    if ok then return res end
    return nil, res
end

-- Ist ein UObject gueltig?
local function valid(o)
    if o == nil then return false end
    local ok, v = pcall(function() return o:IsValid() end)
    return ok and v == true
end

local function fnameToStr(fn)
    return try(function() return fn:ToString() end) or "<fname?>"
end

local function fullName(o)
    return try(function() return o:GetFullName() end) or "<nofullname>"
end

local function className(o)
    return try(function() return o:GetClass():GetFName():ToString() end) or "<noclass>"
end

-- ---------------------------------------------------------------------------
-- Wertformatierung fuer Property-Dumps
-- ---------------------------------------------------------------------------
local function formatValue(v)
    local t = type(v)
    if t == "number" or t == "boolean" or t == "string" then
        return tostring(v)
    elseif t == "nil" then
        return "nil"
    elseif t == "userdata" then
        -- UObject? FName? FStruct? -> defensiv versuchen
        local fn = try(function() return v:GetFullName() end)
        if fn then return "[obj] " .. fn end
        local s = try(function() return v:ToString() end)
        if s then return "[str] " .. s end
        return "<userdata>"
    elseif t == "table" then
        return "<table>"
    end
    return "<" .. t .. ">"
end

-- ---------------------------------------------------------------------------
-- Reflection: Properties + Funktionen einer Klasse + Vererbungskette dumpen
-- ---------------------------------------------------------------------------
local function dumpStructProperties(struct, obj, label)
    local printedAny = false
    local ok = pcall(function()
        struct:ForEachProperty(function(Prop)
            local pn = fnameToStr(Prop:GetFName())
            local pt = try(function() return Prop:GetClass():GetFName():ToString() end) or "?"
            local valStr = "<n/a>"
            if obj ~= nil then
                local v = try(function() return obj[pn] end)
                valStr = formatValue(v)
            end
            log("  %-34s : %-22s = %s", pn, pt, valStr)
            printedAny = true
        end)
    end)
    if not ok then
        log("  (ForEachProperty nicht verfuegbar fuer %s)", tostring(label))
    elseif not printedAny then
        log("  (keine eigenen Properties auf %s)", tostring(label))
    end
end

local function dumpStructFunctions(struct, label)
    local names = {}
    local ok = pcall(function()
        struct:ForEachFunction(function(Func)
            names[#names + 1] = fnameToStr(Func:GetFName())
        end)
    end)
    if not ok then
        log("  (ForEachFunction nicht verfuegbar fuer %s)", tostring(label))
        return
    end
    if #names == 0 then
        log("  (keine eigenen Funktionen auf %s)", tostring(label))
        return
    end
    table.sort(names)
    for _, n in ipairs(names) do log("  fn  %s", n) end
end

-- Vollstaendiger Objekt-Dump inkl. Supers
local function dumpObject(obj, opts)
    opts = opts or {}
    if not valid(obj) then log("  <ungueltiges Objekt>"); return end
    log("Objekt : %s", fullName(obj))
    local struct = try(function() return obj:GetClass() end)
    local depth = 0
    while struct ~= nil and depth < CONFIG.maxSuperDepth do
        local sname = fnameToStr(try(function() return struct:GetFName() end) or "?")
        local sfull = fullName(struct)
        log("--- Klasse [%d]: %s  (%s)", depth, sname, sfull)
        dumpStructProperties(struct, obj, sname)
        if not opts.noFunctions then dumpStructFunctions(struct, sname) end
        -- bei nativen /Script/ Basisklassen aufhoeren (zu viel Rauschen)
        if sfull:find("/Script/Engine", 1, true) or sfull:find("/Script/CoreUObject", 1, true) then
            break
        end
        struct = try(function() return struct:GetSuperStruct() end)
        depth = depth + 1
    end
end

-- ---------------------------------------------------------------------------
-- Suchhelfer
-- ---------------------------------------------------------------------------
local function firstOf(cls)
    local o = try(function() return FindFirstOf(cls) end)
    if valid(o) then return o end
    return nil
end

local function countOf(cls)
    local arr = try(function() return FindAllOf(cls) end)
    if arr == nil then return 0, nil end
    local n = 0
    pcall(function() arr:ForEach(function() n = n + 1 end) end)
    return n, arr
end

-- ---------------------------------------------------------------------------
-- F5 — lokaler PlayerController + Pawn
-- ---------------------------------------------------------------------------
local function dumpLocalPlayer()
    rule("F5  PlayerController + Pawn")
    ExecuteInGameThread(function()
        local pc = try(function() return UEHelpers.GetPlayerController() end)
        if valid(pc) then
            log("PlayerController-Klasse: %s", className(pc))
            dumpObject(pc, { noFunctions = false })
            local pawn = try(function() return pc.Pawn end) or try(function() return pc:K2_GetPawn() end)
            if valid(pawn) then
                rule("Pawn (gesteuerte Ratte)")
                log("Pawn-Klasse: %s", className(pawn))
                dumpObject(pawn)
            else
                log("Kein Pawn am PlayerController (noch nicht im Maze?).")
                local anyPawn = firstOf(CONFIG.pawnClass)
                if anyPawn then log("Aber FindFirstOf(%s) -> %s", CONFIG.pawnClass, fullName(anyPawn)) end
            end
        else
            log("Kein PlayerController gefunden.")
        end
    end)
end

-- ---------------------------------------------------------------------------
-- F6 — Maze_Generator (Seed-Suche!)
-- ---------------------------------------------------------------------------
local function dumpMazeGenerator()
    rule("F6  Maze_Generator")
    ExecuteInGameThread(function()
        local gen = firstOf(CONFIG.mazeGenClass)
        if not gen then
            log("Keine Instanz von %s gefunden (nur im Maze_LVL vorhanden).", CONFIG.mazeGenClass)
            return
        end
        log("Generator: %s", fullName(gen))
        dumpObject(gen)
        rule("F6  Heuristik: Seed/Random/Grid-Kandidaten")
        local gen2 = gen
        local struct = try(function() return gen2:GetClass() end)
        local depth = 0
        while struct ~= nil and depth < CONFIG.maxSuperDepth do
            pcall(function()
                struct:ForEachProperty(function(Prop)
                    local pn = fnameToStr(Prop:GetFName())
                    local low = pn:lower()
                    if low:find("seed") or low:find("random") or low:find("stream")
                       or low:find("width") or low:find("height") or low:find("rows")
                       or low:find("cols") or low:find("grid") or low:find("cell")
                       or low:find("size") or low:find("biome") then
                        local v = try(function() return gen2[pn] end)
                        log("  >> %-30s = %s", pn, formatValue(v))
                    end
                end)
            end)
            local sfull = fullName(struct)
            if sfull:find("/Script/", 1, true) then break end
            struct = try(function() return struct:GetSuperStruct() end)
            depth = depth + 1
        end
        log("Hinweis: Generate-Funktion in der obigen 'fn'-Liste suchen")
        log("(Kandidaten: Generate / BuildMaze / ConstructMaze / GenerateMaze).")
    end)
end

-- ---------------------------------------------------------------------------
-- F7 — GameInstance + GameMode
-- ---------------------------------------------------------------------------
local function dumpGameGlobals()
    rule("F7  GameInstance + GameMode")
    ExecuteInGameThread(function()
        local gi = try(function() return UEHelpers.GetGameInstance() end) or firstOf(CONFIG.gameInstanceClass)
        if valid(gi) then
            log("GameInstance-Klasse: %s", className(gi))
            dumpObject(gi)
        else
            log("Keine GameInstance gefunden.")
        end
        local gm = firstOf(CONFIG.gameModeClass)
        if valid(gm) then
            rule("GameMode")
            log("GameMode-Klasse: %s", className(gm))
            dumpObject(gm)
        else
            log("Kein %s gefunden (nur waehrend Run aktiv).", CONFIG.gameModeClass)
        end
    end)
end

-- ---------------------------------------------------------------------------
-- F8 — Pickups + Spawner + Ziel
-- ---------------------------------------------------------------------------
local function dumpInteractables()
    rule("F8  Pickups / Spawner / Ziel")
    ExecuteInGameThread(function()
        local function dumpFirstAndCount(list, header)
            log("--- %s ---", header)
            for _, cls in ipairs(list) do
                local n, _ = countOf(cls)
                log("  %-34s : %d Instanz(en)", cls, n)
            end
            -- erste gefundene Instanz der Liste detailliert dumpen
            for _, cls in ipairs(list) do
                local o = firstOf(cls)
                if o then
                    rule("Detail: " .. cls)
                    dumpObject(o)
                    break
                end
            end
        end
        dumpFirstAndCount(CONFIG.pickupClasses, "Pickups")
        dumpFirstAndCount(CONFIG.spawnerClasses, "Spawner")
        dumpFirstAndCount(CONFIG.objectiveClasses, "Ziel/Exit")
    end)
end

-- ---------------------------------------------------------------------------
-- F9 — Maze-Layout-Fingerprint (Determinismus-Test)
-- ---------------------------------------------------------------------------
-- FNV-1a 32-bit Hash ueber einen String
local function fnv1a(str)
    local h = 2166136261
    for i = 1, #str do
        h = h ~ str:byte(i)
        -- h = h * 16777619 (mod 2^32) ohne Overflow
        h = (h * 16777619) & 0xFFFFFFFF
    end
    return h
end

local function mazeFingerprint()
    rule("F9  Maze-Layout-Fingerprint")
    ExecuteInGameThread(function()
        local positions = {}
        local total = 0
        for _, cls in ipairs(CONFIG.mazePieceClasses) do
            local _, arr = countOf(cls)
            if arr then
                pcall(function()
                    arr:ForEach(function(_, item)
                        local a = item:get()
                        local loc = try(function() return a:K2_GetActorLocation() end)
                        if loc then
                            local x = math.floor((try(function() return loc.X end) or 0) + 0.5)
                            local y = math.floor((try(function() return loc.Y end) or 0) + 0.5)
                            local z = math.floor((try(function() return loc.Z end) or 0) + 0.5)
                            positions[#positions + 1] = string.format("%d,%d,%d", x, y, z)
                            total = total + 1
                        end
                    end)
                end)
            end
        end
        table.sort(positions)
        local blob = table.concat(positions, ";")
        local hash = fnv1a(blob)
        log("Maze-Teile gezaehlt: %d", total)
        log("Layout-Hash (FNV-1a): 0x%08X", hash)
        log("=> Run mit GLEICHEM Seed neu laden und F9 erneut druecken; Hashes vergleichen.")
        if total == 0 then
            log("WARNUNG: 0 Teile. CONFIG.mazePieceClasses anpassen (F6/Live View zeigt echte Wand-Klassen).")
        end
    end)
end

-- ---------------------------------------------------------------------------
-- F10 — Seed forcieren
-- ---------------------------------------------------------------------------
local function forceSeed()
    rule("F10  Seed forcieren = " .. tostring(CONFIG.forcedSeed))
    ExecuteInGameThread(function()
        local gen = firstOf(CONFIG.mazeGenClass)
        if not gen then log("Kein Maze_Generator gefunden."); return end
        local ok, err = pcall(function() gen[CONFIG.mazeSeedProp] = CONFIG.forcedSeed end)
        if ok then
            local rb = try(function() return gen[CONFIG.mazeSeedProp] end)
            log("Seed-Property '%s' gesetzt. Rueckgelesen: %s", CONFIG.mazeSeedProp, formatValue(rb))
            log("Hinweis: Wirkt nur, wenn vor der Generierung gesetzt; ggf. Generate-Fn erneut aufrufen.")
        else
            log("Konnte '%s' nicht setzen: %s (Prop-Name aus F6 pruefen)", CONFIG.mazeSeedProp, tostring(err))
        end
    end)
end

-- ---------------------------------------------------------------------------
-- F11 — Suche + usmap-Hinweis
-- ---------------------------------------------------------------------------
local function searchClasses()
    rule("F11  Suche '" .. CONFIG.searchTerm .. "'")
    log("Tipp: In der UE4SS GUI-Console `Dumper_Usmap` ausfuehren -> erzeugt Mappings.usmap")
    log("(noetig fuer lesbare 5.6 Property-Namen). Danach UObjects/CXX dumpen.")
    ExecuteInGameThread(function()
        -- bekannte Kandidaten zaehlen, damit man sieht was geladen ist
        local probes = {
            CONFIG.pawnClass, CONFIG.controllerClass, CONFIG.gameInstanceClass,
            CONFIG.gameModeClass, CONFIG.mazeGenClass,
            "BP_TeamRat_C", "DAVE_C", "Krueger_Claw_Tentacle_C", "BP_Dead_LabRat_C",
        }
        for _, cls in ipairs(probes) do
            local n = countOf(cls)
            log("  %-34s : %d", cls, n)
        end
    end)
end

-- ---------------------------------------------------------------------------
-- Lifecycle-Logging (Timing der Klassen-Erstellung verstehen)
-- ---------------------------------------------------------------------------
pcall(function()
    RegisterLoadMapPostCallback(function(Engine, World)
        local wn = try(function() return World:GetFullName() end) or "?"
        log("MAP geladen: %s", wn)
    end)
end)

-- Auto-Dump wird ueber NotifyOnNewObject + ExecuteWithDelay getrieben (beides funktioniert in
-- diesem UE4SS-Build; RegisterLoadMapPostCallback und Keybinds feuern hier NICHT zuverlaessig).
local g_mazeDumped = false
local g_pawnDumped = false

-- Maze_Generator: nach Erzeugung kurz warten (Generierung abschliessen lassen), dann dumpen.
pcall(function()
    NotifyOnNewObject("/Game/Procedural_Maze/Maze_Generator.Maze_Generator_C", function(Obj)
        log("NEU: Maze_Generator erzeugt -> %s", fullName(Obj))
        if not g_mazeDumped then
            g_mazeDumped = true
            ExecuteWithDelay(5000, function()
                dumpMazeGenerator()
                dumpInteractables()
                mazeFingerprint()
            end)
        end
    end)
end)

-- Spieler-Pawn: nach Erzeugung dumpen (Pawn + GameInstance/GameMode).
pcall(function()
    NotifyOnNewObject("/Game/Characters/Lab_Rat/BP_LabRat.BP_LabRat_C", function(Obj)
        log("NEU: BP_LabRat erzeugt -> %s", fullName(Obj))
        if not g_pawnDumped then
            g_pawnDumped = true
            ExecuteWithDelay(5500, function()
                dumpGameGlobals()
                dumpLocalPlayer()
            end)
        end
    end)
end)

-- ---------------------------------------------------------------------------
-- AUTONOMER MODUS — Auto-Dump ohne Tastendruck
-- ---------------------------------------------------------------------------
-- Konsolenbefehl ausfuehren (z.B. "open Maze_LVL")
local function execConsole(cmd)
    ExecuteInGameThread(function()
        local ok = pcall(function()
            local ksl = UEHelpers.GetKismetSystemLibrary()
            local world = UEHelpers.GetWorld()
            local pc = UEHelpers.GetPlayerController()
            ksl:ExecuteConsoleCommand(world, cmd, pc)
        end)
        if ok then log("Konsole: %s", cmd) else log("Konsole FEHLER: %s", cmd) end
    end)
end

-- Vollstaendiger Dump-Durchlauf
local function autoDumpAll(tag)
    rule("AUTO-DUMP (" .. tostring(tag) .. ")")
    dumpGameGlobals()
    dumpLocalPlayer()
    dumpMazeGenerator()
    dumpInteractables()
    mazeFingerprint()
    searchClasses()
end

-- Bei jedem Map-Load: nach kurzer Verzoegerung (Aktoren spawnen lassen) automatisch dumpen.
pcall(function()
    RegisterLoadMapPostCallback(function(Engine, World)
        local wn = try(function() return World:GetFullName() end) or "?"
        log("MAP-POST: %s", wn)
        local isMaze = wn:lower():find("maze") ~= nil
        ExecuteWithDelay(4000, function()
            dumpGameGlobals()
            dumpLocalPlayer()
            if isMaze then
                dumpMazeGenerator()
                dumpInteractables()
                mazeFingerprint()
            end
        end)
    end)
end)

-- Konsolenbefehle (in der UE4SS GUI-Console nutzbar: z.B. "cr_dump")
pcall(function()
    RegisterConsoleCommandHandler("cr_dump", function(Cmd, Params, Ar) autoDumpAll("console"); return true end)
    RegisterConsoleCommandHandler("cr_maze", function(Cmd, Params, Ar) dumpMazeGenerator(); mazeFingerprint(); return true end)
    RegisterConsoleCommandHandler("cr_openmaze", function(Cmd, Params, Ar) execConsole("open Maze_LVL"); return true end)
    RegisterConsoleCommandHandler("cr_seed", function(Cmd, Params, Ar) forceSeed(); return true end)
    RegisterConsoleCommandHandler("cr_fp", function(Cmd, Params, Ar) mazeFingerprint(); return true end)
end)

-- Autonom: nach kurzer Wartezeit automatisch ins Maze springen (umgeht Menue-Navigation).
-- Aktivieren via CONFIG.autoOpenMaze; Verzoegerung gibt dem Menue/Engine Zeit.
CONFIG.autoOpenMaze = true
CONFIG.autoOpenDelayMs = 14000
local g_autoOpenDone = false
if CONFIG.autoOpenMaze then
    pcall(function()
        ExecuteWithDelay(CONFIG.autoOpenDelayMs, function()
            if not g_autoOpenDone then
                g_autoOpenDone = true
                log("AUTO-OPEN: oeffne Maze_LVL ...")
                execConsole("open Maze_LVL")
            end
        end)
    end)
end

-- ---------------------------------------------------------------------------
-- Keybinds
-- ---------------------------------------------------------------------------
RegisterKeyBind(Key.F5,  dumpLocalPlayer)
RegisterKeyBind(Key.F6,  dumpMazeGenerator)
RegisterKeyBind(Key.F7,  dumpGameGlobals)
RegisterKeyBind(Key.F8,  dumpInteractables)
RegisterKeyBind(Key.F9,  mazeFingerprint)
RegisterKeyBind(Key.F10, forceSeed)
RegisterKeyBind(Key.F11, searchClasses)

rule("CRToolkit geladen")
log("Keybinds: F5 Player | F6 Maze | F7 Game | F8 Pickups | F9 Fingerprint | F10 Seed | F11 Suche")
log("Erst `Dumper_Usmap` in der GUI-Console ausfuehren, dann ins Maze laufen und F5/F6 druecken.")
