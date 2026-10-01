# Combat Lab: a complete native mod template

This example demonstrates the current native SDK in one C++17 library. It is
a hypothetical gameplay mod, not a working Xenogears combat patch. Its function
addresses, calling conventions, Player fields, and cycle costs must be verified
for the game before use. Addresses default to zero, so `start` rejects activation
until you complete the mapping.

The complete source is [combat_lab.cpp](combat_lab.cpp).

| Capability | Where it is demonstrated |
|---|---|
| Several function hooks in one library | `hook` routes damage and reward calls by physical address. |
| Arguments before the original, results after it | Damage `extend` mode scales `a2`, calls `next`, then adds a bonus to `v0`. |
| Whole-function replacement | Damage `replace` mode and optional reward replacement skip `next`. |
| Partial replacement | `block` edits only `v0` for a selected critical-damage range. |
| Calling another guest function | Replacement damage can call a verified formula with six arguments, including stack arguments, and read `v0`/`v1`. |
| C++ helpers | Named registers, `Host`, `Call`, `Block`, typed options, and `descriptor`. |
| Boolean, choice, and integer options | `start` reads the declared settings using their stable IDs. |
| Guest memory | Regeneration uses byte flags and halfword HP; rewards read/write word-sized XP. |
| Guest clock | Native replacements charge illustrative costs with `advance_cycles`. Guest calls retain their own runtime timing. |
| VBlank and game-start status | Regeneration runs after a configurable number of guest VBlanks. |
| Savestate restoration | `restored` resets host counters and the regeneration timer. Player memory comes from the restored guest state. |
| Start, stop, logging, and userdata | A descriptor-owned `State` stores settings and counters and logs a summary at shutdown. |

## Build the source

From the XenogearsRecomp root:

```sh
cmake -S examples/native-combat-lab -B combat-lab-build -DCMAKE_BUILD_TYPE=Release
cmake --build combat-lab-build --config Release --parallel 2
```

On Linux the library is `combat-lab-build/combat_lab.so`; use `.dylib` on macOS,
or `combat-lab-build/Release/combat_lab.dll` with the usual Visual Studio layout.
You can copy this project elsewhere and set `PSX_NATIVE_SDK_INCLUDE` to a folder
containing just `mod_native_api.h` and `mod_native_api.hpp`.

## Map it to verified game code

Edit `Game` in the source and rebuild. These are the example's assumed contracts;
change the source to match the real routines rather than assigning unrelated
addresses to them.

| Mapping | Assumed behavior |
|---|---|
| `damage_function` | Takes attacker pointer, defender pointer, base damage, and element in `a0..a3`; returns damage in `v0`. |
| `reward_function` | Takes Player pointer in `a0` and XP amount in `a1`; updates that Player's XP and returns the awarded amount. |
| `critical_begin`, `critical_end` | A separate function's range whose only original register effect is `v0 = t0 + t1`. The range starts outside branch/load delay slots. |
| `formula_function` | Optional independent, normally returning guest formula. Its six word arguments are documented in `Game`. It must not route back into this mod's damage hook recursively. |
| `player_pointer` | Address of a guest word containing the current Player pointer. |
| Player offsets and flags | Illustrative fields are flags, defense, HP, maximum HP, and XP. Verify their widths, offsets, alignment, lifetimes, and meanings. |

Do not place a whole-function hook entry inside the partial range. For example,
the critical range can belong to a separate routine called by the damage
function. Extension and partial replacement can then compose without scaling
the same operation twice. Replace the demonstration cycle charges with costs
appropriate to your implementation.

The settings are committed at launch. The current SDK resets host-owned state
after a savestate load; it does not serialize that state. In this example,
regeneration's countdown restarts after restoration. All guest memory addresses
are read again when needed, so no stale Player pointer is cached across loads.

## Package it

The repository's `examples/native-hook/package.py` generates the revision and
library hashes, instruction guards, and format-9 manifest. After completing the
mapping, invoke it with your verified function and range addresses:

```text
python3 examples/native-hook/package.py LIBRARY combat-lab.psxmod \
    --exe game/slus_006.64 \
    --hook DAMAGE_FUNCTION --hook REWARD_FUNCTION \
    --block CRITICAL_BEGIN:CRITICAL_END \
    --id yourname.combat-lab --name "Combat Lab" --author "Your Name"
```

Uppercase values are placeholders. Use `--image LOAD_ADDRESS:overlay.bin` for
code from a raw overlay, as described in the [native mod guide](../../MOD_NATIVE_HOOKS.md).
No original executable or overlay enters the package.

To provide the complete set of launcher controls, extract the generated archive
into a `package` folder, replace its example `[[option]]` tables with the contents
of [options.toml](options.toml), and put them before `[[native_module]]`. Preserve
the generated hooks and hashes. Repack using:

```sh
python3 psxrecomp/tools/psxmod_pack.py package combat-lab.psxmod
```

The feature ID in both the helper output and `options.toml` is `hook`. Without
the custom option tables the source uses its fallback settings; regeneration
and guest-formula use remain off. Guest-formula use applies to replacement
damage mode. Install the final archive through the launcher, accept the native
code trust prompt, and enable its feature.
