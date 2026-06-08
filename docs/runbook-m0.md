# Runbook — M0 verification (du fuehrst das aus)

UE4SS + CRToolkit sind bereits ins Spiel installiert. Diese Schritte verifizieren die Injektion
und sammeln die echten Klassen-/Funktionsnamen, mit denen die Koop-Mod weitergebaut wird.

## 1. Spiel starten

- **Einfachste Variante:** Steam → Cyber Rats → **Spielen**. Der `Cyber Rats.exe`-Wrapper startet
  die Shipping-Exe; die `dwmapi.dll` daneben laedt UE4SS automatisch mit.
- **Falls UE4SS nicht laedt** (kein Konsolenfenster): in Steam → Cyber Rats → Eigenschaften →
  **Startoptionen**:
  ```
  "F:\Launcher\SteamLibrary\steamapps\common\Cyber Rats\Engine\Binaries\Win64\UnrealGame-Win64-Shipping.exe" %command%
  ```

## 2. Injektion pruefen

Erwartet: ein separates **UE4SS GUI-Konsolenfenster** oeffnet sich beim Start. Darin sollte stehen,
dass Mods gescannt/geladen werden, inkl. `CRToolkit`. Im Spiel-Log/Console erscheint:
```
[CRToolkit] ========== CRToolkit geladen ==========
```
Falls nichts: Datei `…\Win64\ue4ss\UE4SS.log` pruefen und mir den Inhalt schicken (Anfang + Fehler).

## 3. Mappings.usmap erzeugen

In der UE4SS GUI-Konsole eintippen und Enter:
```
Dumper_Usmap
```
Dann (im Projekt):
```
pwsh F:\Projects\Mods\CyberRats\scripts\pull_usmap.ps1
```

## 4. Introspektion sammeln

Ins Spiel: neues Spiel starten, Ratte waehlen, **ins Labyrinth** laufen. Dann diese Tasten druecken
(Ausgabe landet in der GUI-Konsole **und** in `…\Win64\ue4ss\UE4SS.log`):

| Taste | Was |
|---|---|
| F7 | GameInstance + GameMode |
| F5 | PlayerController + Pawn (Ratte) |
| F6 | **Maze_Generator** (Seed-Property + Generate-Funktion!) |
| F8 | Pickups (Chese) + Cyborg-Spawner + Exit |
| F9 | Maze-Layout-Fingerprint (Hash notieren) |

## 5. Determinismus-Test (entscheidet die Maze-Sync-Strategie)

1. Im Labyrinth **F9** → Hash A notieren.
2. Aus F6 den Seed-Property-Namen ablesen; in
   `F:\Projects\Mods\CyberRats\lua\CRToolkit\Scripts\main.lua` `CONFIG.mazeSeedProp` setzen,
   mir den Namen aber auch einfach schicken — ich passe es an.
3. Run neu starten / Maze neu laden (gleicher Seed) → **F9** → Hash B.
   - Hash A == B → Maze ist deterministisch (Seed-Sync reicht). 👍
   - Hash A != B → wir brauchen den Layout-Stream-Fallback.

## 6. Mir schicken

Kopiere mir aus `…\Win64\ue4ss\UE4SS.log` die Bloecke ab `[CRToolkit]` (F5–F9). Damit fuelle ich
`docs/hooks.md` mit den echten Namen und baue M1 (Transport) + die Maze-/Puppet-Hooks.

> Kurz reicht auch: der **F6-Block** (Maze_Generator-Properties/Funktionen) + die **zwei F9-Hashes**
> sind die wichtigsten. Der Rest macht es vollstaendiger.
