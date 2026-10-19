agentbar
========

A small always-on-top bar for Windows that keeps track of my terminal AI agent
sessions (omp, mostly; Claude Code and Codex work too) and starts new ones.

One C file, plain Win32 + GDI. ~140 KB exe, no runtime, ~2 MB of memory, and it
sits at roughly zero CPU when nothing is happening.


Why
---

I run a lot of agent sessions in parallel, often 4-10 at once across different
projects. Every new one used to be the same ritual: open a terminal, cd somewhere,
mkdir a scratch folder, start the agent, type the first prompt. Then later: which
window was that again, is it done yet, how much of my weekly limit is left.

This removes that. It's a personal tool and it does exactly what I need, nothing
more.


What it does
------------

    click a session      bring its terminal to the front (and the right tab)
    +                    new session: quick idea (~/new_ideas/ideaN), ephemeral
                         (%TEMP%), or any folder. optional first prompt, which is
                         also used to name the folder
    ctrl+click, Open N   tile the selected terminals in a grid (all if none selected)
    right-click session  rename / open folder / close
    right-click bar      dock top/left/right, start with Windows, quit
    drop a folder        start a session there
    Win+Alt+N / E / T    quick idea / ephemeral / tile
    Win+Alt+1..9         focus the Nth session

Each session shows its folder (or the name you gave it), the model it's on, and
how long it has been working. A thin green line under a session means it is
working; amber means it finished while you were looking at something else.

On the right: what's left of the Claude 7-day, Codex 7-day and Claude 5-hour
limits. Hover the bar for titles, paths and reset times.


Building
--------

You need a MinGW-w64 gcc on PATH. I use MSYS2 UCRT64.

    build.cmd

That's it. Run agentbar.exe. Only one copy runs at a time.

Needs Windows 10/11 x64 and Windows Terminal (the launch command is configurable).
Rounded corners need Windows 11; on 10 it's square, which is fine.


Config
------

agentbar.ini is created next to the exe on first run, with comments. It is
re-read within 2 s of saving it. Roughly:

    [bar]
    edge=top
    ideas_dir=%USERPROFILE%\new_ideas
    idea_prefix=idea
    ; %d = folder, %c = agent command
    terminal=wt.exe -w new new-tab -d "%d" %c

    [agents]
    ; name=command, the process looked for is <name>.exe
    omp=omp
    claude=claude
    codex=cmd /c codex

    [hotkeys]
    new_idea=Win+Alt+N
    ephemeral=Win+Alt+E
    tile=Win+Alt+T
    focus=Win+Alt

    [usage]
    Claude 7d=anthropic:7d
    Codex 7d=openai-codex:primary
    Claude 5h=anthropic:5h

Comments go on their own line. The Windows ini reader does not strip trailing
comments, so `edge=top ; foo` gives you the value "top ; foo".

agentbar.state.ini is the bar's own file: last edge, last agent, session names.
It never writes to agentbar.ini, except through the group commands below.

There is also folder grouping (`[group:<name>]` sections,
`agentbar.exe group <name> <folder>`, `agentbar.exe ungroup <folder>`). I don't
really use it; it's there.


Design notes
------------

Stuff I decided on and why, mostly so I remember.

* C and raw Win32 instead of Electron/Tauri/WinForms. Those idle at 40-170 MB
  for what is a strip of rectangles. This one is ~2 MB.

* Nothing is registered. Sessions are discovered: every ~2 s the bar diffs the
  process list against a cache, and a new agent process gets checked once. It
  attaches to the process's console, reads the tab title, follows the console's
  owner up to the hosting terminal window, and reads the working folder out of
  the process. So sessions I started by hand, or before the bar was running, show
  up too, and there's no state that can drift.

* EnumProcesses, not CreateToolhelp32Snapshot. Toolhelp cost ~14 ms per scan here
  (300+ processes); EnumProcesses is ~0.03 ms, and non-agent pids are remembered
  so they aren't looked at again.

* One terminal window per session. Tabs share a window, and you can't tile tabs.
  Focusing a tab inside a shared window still works (UI Automation, loaded only
  when actually needed).

* Model, usage and working state come from files omp already writes. The model
  is the last `model_change` line in the session's .jsonl, found by reading
  backwards from the end (a few MB per tick at most, so a 20 MB transcript never
  stalls the UI) and then only the appended bytes. Usage limits come from omp's
  agent.db, read through Windows' own winsqlite3.dll and only when the file
  changes. No network, no API keys, no credentials touched.

* "Working" = the agent has a Braille spinner in its title. Crude, free, works.

* The bar registers as a Windows app bar, so maximized windows stay out of its
  strip. The visible pill floats inside that strip and is exactly as long as
  its contents; hovering grows it over other windows without moving anything.

* Config is INI via GetPrivateProfileString. No JSON parser to carry around.

* Launch prompts go through two command-line parsers (wt.exe, then the agent),
  so they're escaped twice. Quotes, backslashes, `;`, unicode and emoji all
  arrive intact.


Limitations
-----------

* Primary monitor only.
* Model / usage / working state only for omp. Claude and Codex sessions show up
  and can be focused and tiled, but show the agent name instead of a model.
* Agents inside an editor's integrated terminal (VS Code etc.) aren't found:
  their console has no owning window.
* x64 only (the folder lookup reads the process environment block at x64
  offsets).
* Quick ideas are numbered N+1. That's the only naming rule.


License
-------

MIT. See LICENSE.
