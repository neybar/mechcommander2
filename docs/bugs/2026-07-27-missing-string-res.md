# "3/3 missing string res" in the vehicle info tab (OPEN — not yet investigated)

**Found:** 2026-07-27, live play test at building 13 (Mission 1), on the
captured Fire Ant 30t 'Mech. **Not investigated — noted only.**

## Symptom

The in-mission vehicle info tab (the `i` panel) lists the mech's loadout as:

```
3/3  missing string res
3/3  MG Array
undefined
undefined
undefined
```

The second component resolves ("MG Array") but the first does not, and the
lower list is all `undefined`. jalance's note: it's a captured mech with battle
damage, and the stock Fire Ant loadout wasn't verified — so it is not yet known
*which* component the missing name belongs to.

## Where the strings come from (traced, ~10 min, no fix attempted)

- `"missing string res"` is a literal in `GameOS/gameos/gameos_res.cpp:107`,
  returned by `gos_GetResourceString()` when the replacement resource DLL has no
  entry for the requested id. **It also logs the failing id to stderr**:
  `"Requested string id: %d not found, return dummy string"` — so a live run with
  stderr captured names the missing ids exactly. That's the cheap first step.
- Call chain: `mclib/cmponent.cpp:100`
  `cLoadString(COMPONENT_NAME_START + masterID, ...)` →
  `mclib/utilities.h:118` `cLoadString()` → `gos_GetResourceString()`.
  `COMPONENT_NAME_START = 32000` (`cmponent.cpp:62`), abbreviations at 33000.
- The resource DLL (`libmc2res_64.dylib`) is our replacement for the Windows
  string resources: `res/libmain.cpp` builds an `id → const char*` map from a
  generated table, `test_scripts/res_conv/strings.res.cpp` (~3995 records) with
  symbolic ids in `strings.res.h`.
- The component range **does exist** in that table (e.g. `IDS_COMP40 = 32040`,
  `IDS_COMP92 = 32092`, `IDS_COMP_ABBR112 = 33112`), so this is **specific gaps,
  not a wholesale omission**. Whether the gaps are in the generated table or in
  the original extraction was not checked.

## Important: `undefined` is probably NOT a bug

The generated table itself contains `{ IDS_COMPENCYCLO40, "undefined" }` — the
string "undefined" is authentic original resource data, and
`mclib/cmponent.cpp:111` explicitly treats a component form of `"undefined"` as
a valid empty slot (`masterID = -1`). So the `undefined` lines are likely the
original game's own behavior for empty component slots, and per
[[feedback-original-engine-bugs]] would stay warts-and-all. Only the
`missing string res` line looks like a genuine port gap.

## Not yet checked

- **Does this reproduce on the GL build?** Both backends load the same resource
  DLL, so it almost certainly does — which would make it a port-wide issue, not
  a Vulkan one. Verify before assuming.
- Which ids actually fail (run and read stderr).
- Whether the gap is in `strings.res.cpp` or upstream in the extraction script.
- What the stock Fire Ant loadout should be, to confirm the missing name.
