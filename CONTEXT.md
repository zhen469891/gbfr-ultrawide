# GBFRUltrawide

In-memory patching plugin that makes Granblue Fantasy Relink render correctly on
non-16:9 displays: the 3D scene fills the whole window while the HUD stays readable and
correctly placed.

## Language

### UI layout

**Canvas**:
The virtual 3840×2160 coordinate space the game lays every UI element out in.
_Avoid_: screen space, UI space

**Canvas units**:
Positions and sizes measured in canvas coordinates (3840×2160), independent of the real
window resolution.

**Canvas manager**:
The single global object holding the canvas size and scale that world→screen projection
reads.

**Full-canvas parent**:
A parent container authored at exactly 3840×2160 — the structural cue that its children
are laid out against the whole screen.

**Fit-height / fill-width**:
The two ways a 16:9-authored canvas can map onto a wider window: fit-height centers it
(pillarboxed UI); fill-width scales it up until it spans the width (cropping/oversizing).

**Span**:
Enlarging an element's layout constraint so its edge-anchored children reach the true
screen edges. Spanning never distorts.
_Avoid_: stretch

**Anchor**:
The parent-relative reference point (0 = left, 0.5 = center, 1 = right, per axis) an
element is positioned from. An element's px position is **anchor-relative**, not
screen-center-relative.

**Screen-anchored element**:
UI positioned on the canvas itself (HUD bars, button prompts, menus).

**World-anchored element**:
UI projected each frame from a 3D world position (nameplates, damage numbers, lock-on
markers).

**Element id**:
The stable hash identifying a UI element; survives sessions and, usually, game builds.
_Avoid_: widget id

**menuTree**:
The transitively-marked set of element ids belonging to menu/story subtrees, which
spanning must leave at 16:9.

**Blocklist**:
The hand-curated element ids excluded from spanning, complementing menuTree.

**Combat prompts**:
The screen-edge button hints shown in battle; they are moved outward (recentered), not
widened.

**Nameplate**:
The floating name/speech label above a character — a world-anchored element with its own
projection path.

**Pillarbox**:
The empty side bars produced when 16:9 content is centered on a wider screen.

**Probe**:
The dev-build diagnostic that logs each unique element flowing through the layout hook,
used to discover element ids.

### Patching & porting

**Pattern**:
A byte sequence with wildcards used to locate a code site in the game executable.
_Avoid_: signature (used interchangeably upstream; prefer pattern here)

**HIT / MISS**:
A pattern scan found (or failed to find) its site at load time.

**FIRED**:
A hook actually executed in-game at least once. HIT does not imply FIRED — the project's
central diagnostic distinction.

**Pinned build**:
The one game build (module timestamp) the plugin targets; updates mean a re-port, not an
abstraction (ADR-0005).

**Re-port**:
The documented process of relocating every pattern after a game update
(`docs/PATTERNS.md`).

**Data-layer patch**:
Fixing shared data (tables, globals, struct fields) once, instead of hooking every code
path that consumes it (ADR-0003).

### Build flavors

**Release build**:
The default build and what CI ships to users; contains no debug machinery.

**Dev build**:
`build.ps1 -Dev`; additionally honors the `[Debug - Span HUD]` ini section (Probe and
per-element overrides). Identifies itself in the log (ADR-0010).
