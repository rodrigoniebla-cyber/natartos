# RE: Menu Injection — reference

## Two-layer architecture

| Layer | Classes | Role |
|-------|---------|------|
| **Definition** | `UavMenuTree` subclasses (UavWa520, UavZa530...) | Declares menu items (tree of definitions) |
| **Rendering** | `DuThemedControlsMoreMenu` | Draws items on screen (widgets, layout) |

Only the **rendering layer** works for injecting into More menu.

## Adding items to Settings root

Hook slot 11 (DefineMenu) on `UavMenuTree` VMTs. After `original(tree)`:
```c
void *root_menu = *(void **)((char *)tree + 0x80);
void *item = MxMenuDefinitionMenu_AddItem2(root_menu, title, id);
*(int *)((char *)item + 0x44) = 0;  // bypass data source lookup
```
Only `id=1` is safe at root level; others cause SIGSEGV.

## Adding items to More Menu (working approach)

PLT-interpose `MxBindableAbstractMenu_Reload`:
1. Filter by `*(void**)self == __vmt_DuThemedControlsMoreMenu`
2. Call original Reload, then:
   - `menudata = self[73]` (offset 0x248)
   - `md_vmt[11]` = count, `md_vmt[12]` = get_item(index)
3. Clone 256-byte template from last stock item
4. Patch: `d[4]=action, d[5]=icon, d[16]=title, d[17]=subtitle, d[18]=type(31)`
5. `MxBindableAbstractMenu_AddItemFromData(self, item_data)`
6. `CoreVerticalList_OnSetNoOfItems(self, 0)` + `vmt[58](self)` (Postreload)

## VMT names

UavMenuTree subclasses (10 total):
`__vmt_UavInvalidUavMenuTree`, `__vmt_UavWa020UavMenuTree`, `__vmt_UavWa140UavMenuTree`,
`__vmt_UavWa233UavMenuTree`, `__vmt_UavWa520UavMenuTree`, `__vmt_UavWa521UavMenuTree`,
`__vmt_UavWa530UavMenuTree`, `__vmt_UavWm169unitUavMenuTree`, `__vmt_UavZa530UavMenuTree`,
`__vmt_UavZa5305UavMenuTree`

Rendering: `__vmt_DuThemedControlsMoreMenu`, `__vmt_DuThemedControlsMainMenu`

## Item data layout (unsigned long[])

| Offset | Field |
|--------|-------|
| `[4]`  | `MxBindableMenuItemSubTitleActionValue` (action object, 64B) |
| `[5]`  | `ResourcesBitmap` icon |
| `[16]` | title (XString) |
| `[17]` | accessibility subtitle (XString) |
| `[18]` | type (set to 31 for indexed-image-action) |

Action value layout (at `[4]`):
- `+0x20` (32B): slot `{void *object, void *method}` — action dispatch target
- `[6]`: subtitle XString (displayed in UI)

## Critical: do NOT export EwSignal

EwSignal PLT interposition causes SIGSEGV after 30-40s. Keep signal-related
functions `static`. Action dispatch uses marker-based slot patching on f[4]+0x20.

## Device picker ("Computer" entry) — recon in progress

Goal: add a **Computer** entry to the goggles' device picker (next to the air
units / Avata 2) that starts PC Stream, so a Mac/PC running toolya-display (or
the web PC Stream) shows up as a "device".

Unknown yet: which class draws the picker, and whether it is an
`MxBindableAbstractMenu` (the same injection path as the More menu) or a
different list type.

Recon (read-only):
1. `touch /data/local/tmp/natartos_menu_spy`, then restart mods.
2. Open the device picker on the goggles (and a couple of other menus, for contrast).
3. `grep "spy:" /data/local/tmp/natartos.log`: each list class is logged once
   on Reload (`__vmt_...` name via dladdr).
4. `sh scripts/dump_device_picker_symbols.sh`: vtables/functions matching
   Device|Product|Aircraft|Uav|Switch|Pair|Bind|Link.
5. `rm /data/local/tmp/natartos_menu_spy` when done.

If the picker appears in the spy log, it's an `MxBindableAbstractMenu` and the
More-menu technique (clone a stock item, patch title/action, AddItemFromData)
should carry over. If it doesn't appear, the picker uses another list type and
needs its own hook.
