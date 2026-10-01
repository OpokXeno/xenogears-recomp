# Making native C/C++ hook mods

A native mod adds your own C or C++ code to XenogearsRecomp. You build a shared
library, put it in a `.psxmod` package, and install that package through the
launcher.

A **hook** runs when the game enters a function you selected. Your code can
inspect the call, change arguments, run the original function, change its
result, or replace the function entirely.

One `.psxmod` can hook many functions. It can also replace selected instruction
ranges inside a function, while keeping the original code before and after each
range. Function hooks and partial hooks can share the same library and package.

The API is in [`mod_native_api.h`](psxrecomp/runtime/include/mod_native_api.h), and a complete
buildable example is in [`examples/native-hook`](examples/native-hook).
Optional C++17 helpers are in
[`mod_native_api.hpp`](psxrecomp/runtime/include/mod_native_api.hpp).
For a complete gameplay template combining these services, see
[Combat Lab](examples/native-combat-lab/README.md). Its addresses and game
structures are illustrative and must be mapped before activation.

## How a hook works

An ordinary call is:

```text
Game caller -> original game function -> game caller
```

With a hook, it becomes:

```text
Game caller -> your hook -> original game function -> your hook -> game caller
```

Your hook decides whether to run the original function.

| Approach | What your code does | Example use |
|---|---|---|
| Observe | Log the call, then continue normally. | Find when an encounter or menu function runs. |
| Extend | Edit arguments before the original, or results after it. | Adjust a reward returned by a known reward function. |
| Replace | Run your own logic and skip the original. | Replace a small calculation whose full behavior you understand. |
| Replace a range | Run custom code in place of selected instructions inside a function. | Change one calculation while keeping initialization and cleanup. |

You can also read and write game memory, use settings from the launcher, and
receive callbacks at VBlank or after a save state is restored.

This is a low-level API. It provides registers, memory addresses, and function
calls. To change HP, damage, items, or enemy behavior, for example, you must first identify
the relevant function or data structure. There are no built-in helpers such as
`set_hp()` or `add_item()` yet.

The runtime's **dispatcher** routes execution to guest functions. Hooks run at
those entry points.
Function hooks need those boundaries. For an instruction inside a function,
use a partial hook as explained below. Hooks are inactive during BIOS boot. Overlay
functions can be hooked when they reach these boundaries and their live bytes
match the package's guard.

## Step 1: choose where your code runs

Choose the function you want to hook, or the start and end of a range you want
to replace. Study its registers and memory effects with a disassembler,
debugger, or the recompiled source.

### Function names

You can identify a function by its name in [`annotations/`](annotations) or by
its guest address. For example, `WaitForVerticalRetrace` and `0x8004B54C`
identify the same resident function. Names are case-sensitive and do not include
the signature or description: `CommitGameStateTransition(unsigned state) — ...`
becomes `CommitGameStateTransition`. Existing underscores remain part of a name;
spaces and punctuation become underscores, such as `Kernel_MENU`.

Use an image prefix when you want to make the owner explicit:

| Reference | Meaning |
|---|---|
| `resident/WaitForVerticalRetrace` | A function in the main executable. |
| `battle-overlay/BattleMain` | A function in the Battle overlay. |
| `WaitForVerticalRetrace+0x20` | An instruction 32 bytes after that function's entry. |
| `0x8004B54C` | A guest address |

An unqualified name must identify one function. An ambiguous name is rejected,
even if only one of its overlays is currently loaded. Use the exact image ID
from [`annotations/overlays/index.toml`](annotations/overlays/index.toml).
`IMAGE/func_XXXXXXXX` is also available as an address-style annotation alias.
Only function entries are named functions; annotations for individual call,
return, or instruction sites are excluded. The native API still uses the same
2 MiB RAM bounds; developer diagnostics annotated outside those bounds are not
available through this catalog.

### Guest addresses

An address such as `0x80019524` belongs to PlayStation memory. Use the host
memory services to access guest data; do not dereference that address as a C/C++
pointer. Hook points must be instruction-aligned and map to physical game RAM
from `0x10000` through `0x1fffff`. KUSEG, KSEG0, and KSEG1 aliases identify the
same physical hook point.

For a first logging test, `0x80019524` is the game entry address in the USA game
configuration. It can run for the whole session, so code after its original call
may not run until the session ends. Leave replacement mode off for that test.

## Step 2: build and install the supplied example

You need CMake, a C compiler, and Python 3.11 or newer. Run these commands from the repository
root: the `XenogearsRecomp` folder containing `examples`, `game`, and
`CMakeLists.txt`.

### Build the example

The example builds independently of the game:

```sh
cmake -S examples/native-hook -B native-hook-build -DCMAKE_BUILD_TYPE=Release
cmake --build native-hook-build --config Release --parallel 2
```

The output depends on the platform and CMake generator:

| Build | Typical output |
|---|---|
| Linux | `native-hook-build/native_hook_example.so` |
| macOS | `native-hook-build/native_hook_example.dylib` |
| Windows, Visual Studio generator | `native-hook-build/Release/native_hook_example.dll` |

If a C++ compiler is available, the same build also produces
`native_hook_cpp_example` with the platform's library suffix. Its source,
[`native_hook_cpp.cpp`](examples/native-hook/native_hook_cpp.cpp), uses the C++
helpers. You can package either library with the commands below.
It also builds `native_hook_named_example`, described under "A complete named
example" below.

### The tool used to create the package

With the library built, use [`package.py`](examples/native-hook/package.py), a
Python script included in this repository. Run it in a terminal to create the
`.psxmod` archive that you install through the launcher.

The command has this structure:

```text
python3 examples/native-hook/package.py LIBRARY OUTPUT.psxmod [OPTIONS]
```

`LIBRARY` is the path to your compiled `.so`, `.dll`, or `.dylib` file.
`OUTPUT.psxmod` is the package file to create. Options such as `--exe`,
`--image`, `--hook`, and `--block` are arguments to this Python script.
The commands below show how to package the supplied example library.

Relative file paths are resolved from your terminal's current folder. From
your own mod project, use the full path to `package.py` and to the game files.
On Windows, use `python` instead of `python3` if that is the name of your
Python command.

To list all packaging options, run:

```sh
python3 examples/native-hook/package.py --help
```

To find function names without building a library or reading your game files:

```sh
python3 examples/native-hook/package.py --list-functions
python3 examples/native-hook/package.py --list-functions Battle
```

The optional filter searches qualified names. Output contains each name and its
hexadecimal address. The script finds this repository's annotations even when
you run it from your own project directory. An advanced `--annotations PATH`
option selects another annotation directory with the same main CSV and indexed
overlay layout.

### Source files used for automatic checks

Pass `--exe game/slus_006.64` to `package.py` for functions in the USA main
executable. The script reads its load address, extracts the selected original
bytes, and records the executable's SHA-256 as a target revision check. The
source executable stays on your computer; it is not included in the package.

For an overlay, pass `--image ADDRESS:FILE` to the same script. `ADDRESS` is where
the raw file is loaded into guest RAM. You can supply several nonoverlapping
images and combine them with `--exe`. For a full RAM snapshot, use
`--image 0x0:ram.bin`. Only the guards and your own library enter the archive.
When an overlay is selected by name, the script checks the supplied source's
SHA-256 against the annotation index and chooses the matching image. This allows
several overlay files to share a load address. Use the original overlay file:
a live snapshot whose data has changed will not match its original file hash.

The helper samples 16 bytes for a function guard by default and uses the entire
selected range for a partial hook. If you need a different function sample, use the advanced
`--guard-bytes` option. Guards contain whole PS1 instructions and must fit in
guest RAM.

The runtime compares the generated guard with live guest memory before invoking
your callback. If it differs, the original code runs. This prevents hooking a
different game revision or an unrelated overlay that later occupies the same
address. The packaging tool creates this check automatically.

### Package the example

The command needs your library, output archive, local executable, and function
name or address. On Linux:

```sh
python3 examples/native-hook/package.py \
    native-hook-build/native_hook_example.so \
    native-hook-example-1.0.0.psxmod \
    --exe game/slus_006.64 \
    --hook ResidentEntryPoint
```

For macOS, use the `.dylib` path. A Windows PowerShell command is:

```powershell
python examples/native-hook/package.py `
    native-hook-build/Release/native_hook_example.dll `
    native-hook-example-1.0.0.psxmod `
    --exe game/slus_006.64 `
    --hook ResidentEntryPoint
```

The helper generates a manifest, hashes the library, and creates a deterministic
archive. It does not load the library. It detects the build machine's platform;
use `--platform`, such as `windows-x86_64`, when cross compiling for another one.

### Install the example

Install the archive through the launcher's package management view. Choose
**Trust and install**, enable **Native Function Hook**, and launch the game.
The example logs its first five hook calls to the process log. Its feature is
disabled by default, so installation alone does not activate it.

The example has two settings:

| Setting | Behavior |
|---|---|
| **Replace function (advanced)** | Off: run the original. On: skip it and return the configured value. |
| **Replacement return value** | The value written to register `v0` in replacement mode. |

Leave replacement off until you have studied a function that can safely be
replaced. Returning a constant does not reproduce memory writes or other effects.

The helper defaults to the example's identity and settings. For your own mod,
set `--id`, `--name`, `--author`, and `--version`; repeat `--hook` and `--block`
as needed. It generates the guards, library hash, and manifest. The advanced
manifest workflow below lets you customize settings and platform variants.

### A complete named example

[`native_hook_named.cpp`](examples/native-hook/native_hook_named.cpp) uses
`call.matches()` to distinguish two resident hooks and calls the verified
zero-argument getter `InspectPendingCallback` by name. It logs up to five calls
and supports the standard example's replacement controls. Leave replacement off
to keep normal callback setup and vertical-retrace behavior.

On Linux, after the example build above:

```sh
python3 examples/native-hook/package.py \
    native-hook-build/native_hook_named_example.so named-hooks.psxmod \
    --exe game/slus_006.64 \
    --hook ClearCallbackChain --hook WaitForVerticalRetrace
```

For other platforms, change the library path and suffix as shown above. This
example requires a runtime with the named-function services. Your library only
needs the two SDK headers; the launcher already contains the generated catalog.

Names are resolved to integer addresses during packaging. The resulting
manifest remains format 9 and contains the same addresses and guards as a
hexadecimal declaration. A library using only the original numeric API keeps
working with older runtimes; a library requiring name lookup must check that
service's availability in `start`.

## Step 3: write your own library

You only need the SDK header. Copy `mod_native_api.h` into an `sdk` directory in
your own project. Use a source layout like this:

```text
my-native-mod/
  sdk/mod_native_api.h
  hook.c
  CMakeLists.txt
```

Save this complete logging mod as `hook.c`:

```c
#include "mod_native_api.h"
#include <stdio.h>

static const PSXNativeHost* host;
static unsigned calls;

static int PSX_NATIVE_MOD_CALL start(void* userdata, const PSXNativeHost* api) {
    (void)userdata;
    if (api->abi_version != PSX_NATIVE_MOD_ABI || api->size < sizeof(*api))
        return 0;
    host = api;
    calls = 0;
    host->log(host->context, "My hook is ready");
    return 1;
}

static void PSX_NATIVE_MOD_CALL hook(
    void* userdata, PSXNativeCPU* cpu, const PSXNativeCall* call) {
    (void)userdata;
    if (++calls <= 5) {
        char message[96];
        snprintf(message, sizeof message, "Call %u at 0x%08x, a0 = 0x%08x",
                 calls, (unsigned)call->address, (unsigned)cpu->gpr[4]);
        host->log(host->context, message);
    }
    call->next(call->context, cpu);
}

static void PSX_NATIVE_MOD_CALL stop(void* userdata) {
    (void)userdata;
    if (host) host->log(host->context, "My hook stopped");
    host = NULL;
}

static const PSXNativeMod mod = {
    sizeof(PSXNativeMod), PSX_NATIVE_MOD_ABI, NULL,
    start, stop, hook, NULL, NULL, NULL
};

#ifdef __cplusplus
extern "C"
#endif
PSX_NATIVE_MOD_EXPORT const PSXNativeMod* PSX_NATIVE_MOD_CALL psx_native_mod_v1(void) {
    return &mod;
}
```

The runtime calls `psx_native_mod_v1()` to get this descriptor. Its version and
size identify the interface your library implements. Keep the descriptor and
any `userdata` alive until `stop` finishes; the example uses static storage.
`userdata` is an optional pointer to your state, passed back to every callback.

In C++, keep the entry point under `extern "C"`, as shown above. Use
`PSX_NATIVE_MOD_CALL` on callbacks and `PSX_NATIVE_MOD_EXPORT` on the entry point.
You do not link against the game or include its private `CPUState` definition.

Save this as `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_native_mod C)
add_library(my_native_hook SHARED hook.c)
target_include_directories(my_native_hook PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
set_target_properties(my_native_hook PROPERTIES PREFIX "" C_STANDARD 99)
if(MSVC)
    set_target_properties(my_native_hook PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
    target_link_options(my_native_hook PRIVATE -static-libgcc)
endif()
```

Build from `my-native-mod`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

The resulting library is ready for the packaging helper. If using a custom
manifest, copy it into `package/native`. For C++, use `hook.cpp`,
change the project language to `CXX`, and replace `C_STANDARD 99` with
`CXX_STANDARD 17`. With MinGW C++, also add `-static-libstdc++` to the link
options so the library does not depend on an extra C++ runtime DLL.

## Step 4: package your own library

For a basic mod, use the same helper with your own library and metadata:

```sh
python3 /path/to/XenogearsRecomp/examples/native-hook/package.py \
    build/my_native_hook.so my-hook-1.0.0.psxmod \
    --exe /path/to/XenogearsRecomp/game/slus_006.64 \
    --hook 0x80019524 \
    --id yourname.xenogears.my-hook \
    --name "My Function Hook" --author "Your Name" --version 1.0.0
```

The helper handles platform detection, compatibility guards, hashes, and the
format 9 manifest. Rebuild and package again when your code changes. Update the
package version when publishing. You can then install as described in Step 5.

### Advanced: write a custom manifest

Use a custom manifest when adding your own settings, several platform variants,
or other mod operations. `expected` and `sha256` are generated fields in the
normal helper workflow, so manual editing is not required for a basic mod.

This custom manifest targets Linux. Replace the metadata, payload path,
platform, hash, address, and guard as needed. Placeholder values must be filled
in before installation:

```toml
format_version = 9
id = "yourname.xenogears.my-hook"
version = "1.0.0"
name = "My Function Hook"
author = "Your Name"
description = "Logs calls to a verified game function."

[[target]]
game_id = "SLUS-00664"

[[feature]]
id = "hook"
name = "Function Hook"
description = "Observe the selected function."
default_enabled = false

[[native_module]]
id = "main"
feature = "hook"
platform = "linux-x86_64"
file = "native/my_native_hook.so"
sha256 = "PASTE_THE_LIBRARY_SHA256_HERE"

[[native_module.hook]]
address = 0x80019524
expected = "PASTE_THE_ORIGINAL_INSTRUCTION_BYTES_HERE"
```

This uses the same boot tracing address as the first example. Replace it with
your verified function address when making a different mod.

| Field | Meaning |
|---|---|
| `format_version` | Use `9` for all native modules, including function hooks and partial hooks. |
| Package `id` | A stable, unique name for your mod. Keep it across releases. |
| `version` | A package version such as `1.0.0`. Increase it when publishing changes. |
| `target.game_id` | The supported game identifier. |
| Feature `id` | The launcher toggle that owns the module and its options. |
| Module `id` | Your library's identity within that feature, such as `main`. |
| `platform` | The OS and CPU targeted by the library. |
| `file` | A path relative to the package root. It must stay inside that root. |
| `sha256` | The lowercase SHA-256 of the compiled library. |
| Hook `address` | A guest function entry, or the first instruction of a partial range. |
| Hook `expected` | Original bytes to check: a function guard or the entire partial range. |
| Hook `resume_address` | Optional. Selects a partial hook and names the first instruction kept after the range. |

Compute the hash without loading the library:

```sh
python3 -c "import hashlib; from pathlib import Path; print(hashlib.sha256(Path('package/native/my_native_hook.so').read_bytes()).hexdigest())"
```

Use the corresponding library path on Windows or macOS. You can also add
`exe_sha256` or `disc_sha256` to `[[target]]` to restrict support to a tested
revision. The disc value is the runtime's canonical mounted-disc digest, not the
hash of a CUE, BIN, or CHD container. See the
[target and revision guide](MOD_AUTHORING.md#targets-and-revision-guards).

Dependencies, conflicts, and save compatibility declarations work as they do
for other packages. See [`MOD_AUTHORING.md`](MOD_AUTHORING.md) for these fields
and for combining native code with declarative operations.

## Step 5: install and iterate

If you used the native packaging helper, your `.psxmod` is ready. Install it
through the launcher's Mods page, accept the trust prompt, enable the feature,
and launch. Confirm that the hook runs and that normal behavior is preserved.

When changing code, rebuild the library and rerun the same helper command. It
updates the library hash and compatibility checks automatically. Increase
`--version` when publishing a new release. A native package copied directly
into the installed catalog does not gain trust; an edited package with the same
version must be removed and reinstalled through the install dialog.

### Advanced: package a custom manifest

Keep `manifest.toml` at the archive root. Put the manifest, libraries, README,
and license in `package`. Keep source, build output, original game files, and
test data outside that directory. Update the custom manifest's library hash
when rebuilding.

From `my-native-mod`, use the repository's general packer:

```sh
python3 /path/to/XenogearsRecomp/psxrecomp/tools/psxmod_pack.py \
    package my-native-mod-1.0.0.psxmod
```

The packer creates a deterministic ZIP archive without executing the library.
The launcher validates package content; the runtime checks the library's
interface when loading it for a game session. Install and test as above.

## Change arguments, results, or the whole function

The snippets below replace the `hook` callback from the complete example.

### Registers you will use

`PSXNativeCPU` contains PlayStation registers, not your computer's registers.
For a function using the usual PS1 C calling convention:

| Field | Common meaning |
|---|---|
| `gpr[4]` through `gpr[7]` | First four arguments: `a0` through `a3`. |
| `gpr[2]`, `gpr[3]` | Return registers: `v0`, `v1`. |
| `gpr[29]` | Stack pointer, `sp`. |
| `gpr[31]` | Return address register, `ra`. |
| `hi`, `lo` | Multiplication/division result registers. |
| `pc` | Next guest address to execute. |

The runtime keeps register zero fixed at zero. It owns the remaining private
CPU state, including COP0, GTE, and timing state.

### Edit an argument before calling the original

Suppose you verified that `a0` is a small unsigned quantity:

```c
static void PSX_NATIVE_MOD_CALL hook(
    void* userdata, PSXNativeCPU* cpu, const PSXNativeCall* call) {
    (void)userdata;
    if (cpu->gpr[4] < 10) cpu->gpr[4] = 10;
    call->next(call->context, cpu);
}
```

The original receives the edited value. If `a0` is actually a pointer or an
identifier, this edit would be incorrect.

### Edit a result after the original returns

Suppose a verified reward function returns an unsigned value in `v0`:

```c
static void PSX_NATIVE_MOD_CALL hook(
    void* userdata, PSXNativeCPU* cpu, const PSXNativeCall* call) {
    (void)userdata;
    if (!call->next(call->context, cpu)) return;
    if (cpu->pc != call->return_address) return;
    cpu->gpr[2] /= 2;
}
```

After a normal return, `cpu->pc` holds `call->return_address`. The check avoids
editing a result when guest execution continues somewhere else. A guest
exception or nonlocal exit can also prevent `next()` from returning to the hook.

### Replace a function

Suppose the complete behavior of a verified function is to return a value,
and you have determined an appropriate guest cycle charge:

```c
static uint32_t replacement_cycles; /* Set to your measured guest cycle cost. */

static void PSX_NATIVE_MOD_CALL hook(
    void* userdata, PSXNativeCPU* cpu, const PSXNativeCall* call) {
    (void)userdata;
    cpu->gpr[2] = 10;
    cpu->pc = call->return_address;
    host->advance_cycles(replacement_cycles);
}
```

This hook skips `next()`, writes its result, and publishes the continuation.
Preserve callee-saved registers and stack discipline, and reproduce any required
memory effects. The skipped original's cycles are not charged automatically.
The supplied example uses a one-cycle demonstration charge; that is not a timing
model for arbitrary functions.

### Rules for next()

Call `next()` synchronously, at most once per hook call, with exactly the CPU
pointer you received. Do not pass a copied structure. It returns zero for an
invalid pointer, repeated use, or an escaped nested guest call. Return from the
callback immediately when a nested call escapes. One means the next stage ran, not that the
guest necessarily returned normally.

The original uses the existing guest dispatcher, including timing, dirty-code
handling, interrupts, and return checks. Nested calls and recursion can encounter
their own hooks.

## Use the C++ helpers

Copy both `mod_native_api.h` and `mod_native_api.hpp` into your project's `sdk`
directory. Include `mod_native_api.hpp` and compile as C++17. These helpers are
header-only: your library still exports the same C entry point and uses format 9.

| Helper | Purpose |
|---|---|
| `psx::mod::CPU` | Named register access: `a0()`, `a1()`, `v0()`, `sp()`, `ra()`, `hi()`, and others. `return_value()` is another name for `v0()`. |
| `psx::mod::Host` | Host services, compatibility checks, logging, memory access, and `options()`. |
| `psx::mod::Function` | A resolved address plus found/unknown/ambiguous status; returned by `host.find_function()`. |
| `psx::mod::Options` | `get_bool`, `get_int`, and `get_string`, with a fallback for missing or invalid values. |
| `psx::mod::Call` | The CPU view, `next()`, `return_value(v0, v1)`, and calls to other guest functions. |
| `psx::mod::Block` | The CPU view, range information, and calls to other guest functions from a partial hook. |
| `psx::mod::descriptor()` | A zero-initialized descriptor with its size and ABI version already set. |

For example, this hook adjusts the first argument, runs the original, and edits
the return value. It uses the host saved by your `start` callback:

```cpp
#include "mod_native_api.hpp"

static psx::mod::Host host;
static uint32_t bonus;

static int PSX_NATIVE_MOD_CALL start(void*, const PSXNativeHost* api) {
    host = psx::mod::Host(*api);
    if (!host.compatible()) return 0;
    auto options = host.options();
    bonus = options.get_bool("enabled", true)
        ? static_cast<uint32_t>(options.get_int("bonus", 10)) : 0;
    return 1;
}

static void PSX_NATIVE_MOD_CALL hook(
    void*, PSXNativeCPU* registers, const PSXNativeCall* native_call) {
    psx::mod::Call call(host, *registers, *native_call);
    call.cpu().a0() += bonus;
    if (!call.next()) return;
    if (call.cpu().pc() != call.return_address()) return;
    call.cpu().return_value() += bonus;
}
```

Declare `enabled` and `bonus` in your manifest if you want launcher controls;
otherwise the example uses their fallback values. `get_int()` returns a signed
64-bit integer and rejects overflow or trailing text. `get_string()` accepts a
fallback and an optional buffer capacity, which defaults to 4096 bytes.

For complete descriptor and entry-point code, use the supplied C++ example.
`CPU`, `Call`, and `Block` are views of the current callback. Do not keep them
after it returns. A `Host` can be kept until `stop` finishes. The wrappers use
the same callback and thread rules as the C API.

## Call another game function

Use `call_guest()` when your hook needs to execute a different guest function,
such as a verified calculation, sound routine, or inventory routine. It uses
the game's normal dispatcher, so interpreted code, compiled code, timing, and
other mods' hooks remain part of the call.

First identify the function's guest address and calling convention. Make sure
its code is loaded and its required game state is ready. The runtime checks
address alignment and stack space; it cannot determine whether an arbitrary
address is the function you intended to call.

### Call by name

Named calls resolve the function and check its current code identity on every
call. For example, `InspectPendingCallback` is a resident getter taking no
arguments:

```cpp
psx::mod::Call call(host, *registers, *native_call);
const auto pending = call.call_guest("InspectPendingCallback");
if (pending.nonlocal()) return;
if (pending) {
    // pending.v0 is the getter's result. Reading it does not change our registers.
}
call.next();
```

The same overload is available on `Block`, and both accept an initializer list
or a pointer and count of argument words. Qualified names work too, such as
`"resident/InspectPendingCallback"`. You still need to understand a function's
arguments and required game state: the name does not infer its C types.

Use `host.find_function("Name")` to inspect metadata, including from `start`:

```cpp
const auto function = host.find_function("WaitForVerticalRetrace");
if (!function) {
    host.log(function.ambiguous() ? "Choose an image-qualified name" : "Function not found");
    return 0;
}
// function.address == 0x8004B54C
```

`call.matches("Name")` compares that resolved address with the current hook,
including RAM aliases. `block.matches("Name+0x20")` can distinguish a partial
hook. Resolution and matching describe addresses; they do not prove that an
overlay is loaded. Keep using the named `call_guest` overload for execution.
Caching an address and calling the numeric overload later skips the named
identity check.

Overlay calls check the function's authenticated code ranges. Identical code
shared at the same address by different images also needs the owning image's
code fingerprint. Replaced, absent, or modified code rejects the call. Code
writes, overlay changes and save-state restores invalidate the runtime's identity
cache. This check can reject a function after another mod patches its guest code.
It does not check gameplay prerequisites or load an overlay for you.

| Named-call status | Meaning |
|---|---|
| `PSX_NATIVE_GUEST_UNKNOWN_FUNCTION` | No matching name, or an invalid reference. |
| `PSX_NATIVE_GUEST_AMBIGUOUS_FUNCTION` | More than one function has that name. Use `IMAGE/NAME`. |
| `PSX_NATIVE_GUEST_FUNCTION_NOT_LOADED` | Current code does not match the selected function's identity. |
| `PSX_NATIVE_GUEST_UNAVAILABLE` | Missing host service or an invalid callback, stack, argument list or output. |

All four reject without executing guest code. Normal returns and nonlocal exits
use the same result statuses as numeric calls. `host.supports_named_functions()`
checks that the optional services exist. Require it in `start` if your mod
depends on them. Named calls target function entries; use name offsets for hook
ranges and `find_function`, and use numeric addresses for deliberate interior
guest calls whose calling contract you have verified.

From C, size-check the appended `call_guest_named` field, then call:

```c
PSXNativeGuestResult result = {sizeof(result), 0, 0};
int status = host->call_guest_named(call->context, cpu,
    "InspectPendingCallback", NULL, 0, &result);
```

`find_function(host->context, name, &address)` returns
`PSX_NATIVE_FUNCTION_FOUND`, `PSX_NATIVE_FUNCTION_UNKNOWN`, or
`PSX_NATIVE_FUNCTION_AMBIGUOUS`. It leaves the output unchanged on failure.

### Call by hexadecimal address

In C++, the call looks like this. The address is illustrative: replace it with
a function you have verified that accepts two 32-bit arguments:

```cpp
psx::mod::Call call(host, *registers, *native_call);
const auto result = call.call_guest(0x80012340, {call.cpu().a0(), 5});
if (result.nonlocal()) return;
if (!result) {
    host.log("Guest call rejected");
    call.next();
    return;
}
// The caller's registers are unchanged. Apply the result explicitly.
call.cpu().return_value() = result.v0;
call.cpu().pc() = call.return_address();
```

For a partial hook, construct `psx::mod::Block(host, *registers, *range)` and
use its `call_guest()` method in the same way. After a normal callback return,
the host still resumes at the declared end of the partial range.

### Arguments, results, and preserved state

Pass 32-bit words in argument order. The first four go in `a0` through `a3`;
the remaining words go on the guest stack. The runtime reserves the 16-byte
argument home area and aligns the temporary stack frame to 8 bytes. Omitted
register arguments are zero. You can pass more than four arguments; the limit
is available guest stack space. Pass guest addresses for strings and structures,
using host memory services to prepare their contents. Host pointers are not
guest pointers. For 64-bit values, respect the function's word order and argument
alignment; the API does not infer C types.

On a normal return, `result.v0` and `result.v1` contain the function's results.
The runtime restores your callback's general registers, HI, LO, PC, and the
temporary argument area. Changes to other guest memory, COP0/GTE state, and
elapsed cycles remain. The callee may use memory below its stack frame as usual.
Guest execution is already charged by the runtime; do not charge those cycles
again with `advance_cycles()`.

You may make several calls during one hook. `next()` remains a separate,
single-use operation for the original hook chain. Calling a hooked function
also runs its hooks: calling your own entry can recurse. Use `next()` to continue
your current chain rather than calling the same entry to bypass it.

`call_guest()` needs the live context of a function or partial hook. It is
unavailable in `start`, `stop`, `vblank`, and `savestate_loaded`. The game must
have started and the current guest stack must be initialized in RAM or scratchpad.

### Rejected calls and nonlocal exits

The C++ result converts to true only after a normal return. A rejected call
does not execute guest code. If `result.nonlocal()` is true, guest control has
left the call instead of returning normally. Return your callback immediately;
do not call `next()`, make another guest call, or edit the result. The runtime
propagates that guest continuation through the enclosing hooks. A partial hook
also propagates the exit instead of forcing its ordinary suffix. A guest
exception can unwind out of the callback without returning a result at all.

### The same operation from C

The host service is appended to ABI v1. Check its presence before accessing it:

```c
#include <stddef.h>

if (host->size < offsetof(PSXNativeHost, call_guest) + sizeof(host->call_guest)
    || !host->call_guest) {
    call->next(call->context, cpu);
    return;
}

uint32_t arguments[] = {cpu->gpr[4], 5};
PSXNativeGuestResult result = {sizeof(result), 0, 0};
int status = host->call_guest(call->context, cpu, 0x80012340,
                              arguments, 2, &result);
if (status == PSX_NATIVE_GUEST_NONLOCAL) return;
if (status != PSX_NATIVE_GUEST_RETURNED) {
    call->next(call->context, cpu);
    return;
}
cpu->gpr[2] = result.v0;
cpu->pc = call->return_address;
```

Pass the exact `cpu` pointer received by the callback, and `call->context` or
`range->context`. Initialize `result.size`. Status is `PSX_NATIVE_GUEST_RETURNED`
for success, `PSX_NATIVE_GUEST_UNAVAILABLE` for rejection, and
`PSX_NATIVE_GUEST_NONLOCAL` for a guest escape. Existing libraries keep working;
the C++ helpers also recognize an older host without this service. A mod that
requires guest calls should check `host.supports_guest_calls()` in `start` and
reject activation if it is false.

The supplied C++ example optionally reads a `guest_function` integer setting.
Zero disables it. With a custom manifest, declare this setting and select a
verified one-argument function returning one word. The example calls it to
transform `a0` before continuing or replacing the hooked function.

## Add settings in the launcher

A **feature** is an on/off row. An **option** is a setting inside that feature.
Declare options in the manifest and read their validated values from the host.
The examples below belong to the `hook` feature. The logging callback does not
automatically implement them; you must read and use them in your code.

### Boolean: turn logging on or off

```toml
[[option]]
feature = "hook"
id = "log_calls"
label = "Log function calls"
type = "boolean"
default = "true"
```

Boolean defaults are strings: `"true"` or `"false"`.

### Choice: select behavior

```toml
[[option]]
feature = "hook"
id = "mode"
label = "Function behavior"
type = "choice"
default = "original"
choice = [
    { value = "original", label = "Keep original behavior" },
    { value = "half", label = "Halve the returned value" },
    { value = "fixed", label = "Return a fixed value" }
]
```

Your code reads the stable `value`, such as `"half"`. The launcher displays the
human-readable `label`.

### Integer: choose a result

```toml
[[option]]
feature = "hook"
id = "fixed_result"
label = "Fixed return value"
type = "integer"
default = 10
min = 0
max = 9999
step = 1
```

The default must fit the bounds and step. The host delivers it as decimal text,
such as `"10"`. Options can also have `description` and `group` fields to explain
and organize controls in the launcher.

### Read and use those settings

Place these declarations and helper before `start` in your source, and include
`<stdlib.h>` and `<string.h>`:

```c
static int log_calls = 1;
static int mode; /* 0 = original, 1 = half, 2 = fixed */
static uint32_t fixed_result = 10;

static void read_settings(void) {
    char text[32];
    if (host->option(host->context, "log_calls", text, sizeof text))
        log_calls = strcmp(text, "true") == 0;
    if (host->option(host->context, "mode", text, sizeof text)) {
        mode = strcmp(text, "half") == 0 ? 1 :
               strcmp(text, "fixed") == 0 ? 2 : 0;
    }
    if (host->option(host->context, "fixed_result", text, sizeof text))
        fixed_result = (uint32_t)strtoul(text, NULL, 10);
}
```

Call `read_settings()` from `start`, after storing `host`. Change the logging
and behavior in `hook` to use those settings:

```c
static void PSX_NATIVE_MOD_CALL hook(
    void* userdata, PSXNativeCPU* cpu, const PSXNativeCall* call) {
    (void)userdata;
    if (log_calls && ++calls <= 5)
        host->log(host->context, "Configured hook called");

    if (mode == 2) {
        cpu->gpr[2] = fixed_result;
        cpu->pc = call->return_address;
        host->advance_cycles(1); /* Demonstration cost; use your verified cost. */
        return;
    }

    if (!call->next(call->context, cpu)) return;
    if (mode == 1 && cpu->pc == call->return_address)
        cpu->gpr[2] /= 2;
}
```

Only expose result changes for a function whose return value you have verified.
Replace the demonstration cycle charge with an appropriate cost for that
function before using fixed-result mode. The boot tracing example should stay
in `original` mode.

`option()` reads options from this module's feature. It copies a NUL-terminated
string and returns zero for an unknown option or an insufficient buffer. These
are committed launch settings; the API does not promise live updates when a
player moves a launcher control.

### Disable a control when another setting overrides it

`disabled_by` names a boolean option in the same feature. For example, add a
`use_original` boolean and put `disabled_by = "use_original"` on `mode` and
`fixed_result`. When it is true, the launcher greys out those controls and the
native host supplies their declared defaults. Your hook must also honor
`use_original` and run the original function.

### Load a module only for a selected value

Put `when` inside `[[native_module]]`, before its nested hook tables:

Replace your existing module declaration with this version; do not add a second
declaration with the same feature, module ID, and platform.

```toml
[[native_module]]
id = "main"
feature = "hook"
platform = "linux-x86_64"
file = "native/my_native_hook.so"
sha256 = "PASTE_THE_LIBRARY_SHA256_HERE"
when = { mode = "half" }

[[native_module.hook]]
address = 0x80019524
expected = "PASTE_THE_ORIGINAL_INSTRUCTION_BYTES_HERE"
```

This module only loads when the feature is enabled and `mode` is `"half"`.
All entries in `when` must match. Conditions use options in the same feature
and text values, including `"true"` and decimal strings for integers. Use them
to select modules; use hook code for decisions that depend on current game state.

## Read game memory and handle lifecycle events

### Read and write guest data

The host provides 8-bit, 16-bit, and 32-bit memory services:

```c
static void update_u16(uint32_t guest_address, uint16_t new_value) {
    uint16_t old_value = host->read_half(guest_address);
    if (old_value != new_value)
        host->write_half(guest_address, new_value);
}
```

Only call this with a verified data address. For a structure pointer passed in
`a0`, read fields with services such as
`host->read_word(cpu->gpr[4] + verified_offset)`. Do not cast a guest address into
a host pointer. Use the correct field width and alignment.

These are data services. For instruction changes, use the package system's
guarded executable patches instead of ordinary memory writes.

### Run periodic code at VBlank

Add this callback to your source:

```c
static unsigned vblanks;

static void PSX_NATIVE_MOD_CALL on_vblank(void* userdata) {
    (void)userdata;
    if (++vblanks == 60) {
        host->log(host->context, "60 guest VBlanks have passed");
        vblanks = 0;
    }
}
```

Put `on_vblank` in the descriptor's `vblank` slot. It runs at the emulation
thread's VBlank boundary after the game starts. This follows guest timing, not
host rendering frequency. Do not assume 60 callbacks always equal one second
for every region or configuration.

### Reset caches after loading a save state

Your C/C++ state is not automatically included in guest save states. Reset or
rebuild counters, caches, and references in a restoration callback:

```c
static void PSX_NATIVE_MOD_CALL on_restore(void* userdata) {
    (void)userdata;
    calls = 0;
    vblanks = 0;
}

static const PSXNativeMod mod = {
    sizeof(PSXNativeMod), PSX_NATIVE_MOD_ABI, NULL,
    start, stop, hook, on_vblank, on_restore, NULL
};
```

Replace the earlier descriptor; do not define a second `mod`. The callback runs
after declarative executable patches have been reapplied. Rebuild cached game
data from restored guest memory as needed.

A module can provide VBlank or lifecycle callbacks without hook tables. It still
needs a native declaration, an enabled feature, and player trust.

## Replace part of a function

A partial hook replaces a contiguous range of guest instructions. Execution is:

```text
Original function prefix -> your block callback -> original function suffix
```

For example, keep a function's checks, replace its damage calculation, and keep
its existing animation, memory writes, and cleanup. You do not need to rewrite
the whole function or modify the recompiler project.

### 1. Choose the range and study its live registers

Record the first instruction to replace and the first instruction to keep after
it. Both addresses must be aligned to four bytes. `resume_address` is exclusive:
instructions at that address still run.

This **hypothetical** disassembly replaces two instructions:

```text
0x8002001c  ...                  ; existing prefix
0x80020020  addiu v0, a0, 7       ; first replaced instruction
0x80020024  sll   v0, v0, 1       ; second replaced instruction
0x80020028  ...                  ; existing suffix starts here
```

The original expression is `v0 = (a0 + 7) * 2`. Suppose your replacement should
calculate `v0 = (a0 + 20) * 3`. Identify every register and memory value used by
the suffix, and preserve all values except the ones your new calculation is
meant to change. A point inside a function does not have a fresh C calling
convention: `a0` may already hold a temporary rather than the original argument.

### 2. Declare the whole range

Use the helper with your original code image and the two addresses:

```sh
python3 examples/native-hook/package.py build/hook.so partial-hook.psxmod \
    --exe game/slus_006.64 --block 0x80020020:0x80020028
```

These hypothetical addresses illustrate the syntax; replace them with a range
you studied in your own game code. The helper reads and records every original
byte in that range. For an overlay, use `--image LOAD_ADDRESS:overlay.bin`.

You can express both endpoints relative to an annotated function. This syntax
example names a range inside `WaitForVerticalRetrace`; study its actual live
registers and instruction boundaries before using that range:

```sh
python3 examples/native-hook/package.py build/hook.so partial-hook.psxmod \
    --exe game/slus_006.64 \
    --block WaitForVerticalRetrace+0x20:WaitForVerticalRetrace+0x28
```

Offsets are nonnegative multiples of four. Names and hexadecimal endpoints can
be mixed. Named endpoints from different images are rejected.

The generated format 9 manifest has this shape:

```toml
[[native_module.hook]]
address = 0x80020020
resume_address = 0x80020028
expected = "0700822440100200"
```

These bytes encode the two hypothetical instructions above, in memory order.
They are an API example, not a Xenogears damage mod. Use your own verified
addresses and bytes.

The helper fills `expected` with every original byte from `address` up to
`resume_address`; you do not type them. The complete range is checked before
replacement. It must fit in game RAM and stay in the same address segment.
Neither a function guard nor a partial guard has a 64-byte maximum, and their
length never limits the amount of C/C++ code in your callback.

Without `resume_address`, the declaration remains a normal function hook.
Both kinds of hook belong to format 9 and can appear in the same module. The
runtime rejects unknown native module and hook fields rather than ignoring
them. Partial hooks need a runtime that implements `resume_address` and the
extended `block` callback; format 9 alone does not make earlier builds support
those features. Existing function-only libraries remain compatible.

### 3. Add the block callback

Add this to your library, using the `host`, `start`, and `stop` from the complete
example:

```c
static void PSX_NATIVE_MOD_CALL block(
    void* userdata, PSXNativeCPU* cpu, const PSXNativeBlock* range) {
    (void)userdata;
    (void)range;
    cpu->gpr[2] = (cpu->gpr[4] + 20u) * 3u;
    host->advance_cycles(2);
}

static const PSXNativeMod mod = {
    sizeof(PSXNativeMod), PSX_NATIVE_MOD_ABI, NULL,
    start, stop, hook, NULL, NULL, block
};
```

Replace the earlier descriptor instead of defining a second `mod`. Set its
`hook` field to `NULL` if the library only has partial hooks. Rebuild and rerun
the helper with `--block`, then install as described in Step 5.

The callback receives:

| Field | Meaning |
|---|---|
| `range->address` | First replaced instruction, using the current guest address alias. |
| `range->resume_address` | First original instruction that runs after the callback. |
| `range->byte_count` | Number of replaced instruction bytes. |
| `range->return_address` | Current guest `ra`; it is separate from the range continuation. |
| `cpu` | Registers at the replacement point; pending load writeback is committed before entry. |

You can use host memory services and launcher options just as in a function
hook. Charge appropriate guest cycles for your replacement. The example's two
cycles are illustrative, not a general timing rule for damage calculations.

There is no `next()` in a block callback. It replaces that range; it does not
call the whole function from the middle. You can call another verified guest
function with `call_guest()` using `range->context`. After a normal callback
return, the host sets the continuation to `range->resume_address`, even if you
edit `cpu->pc`. A nonlocal exit from an injected guest call propagates instead.
Preserve live `sp`, `ra`, HI, LO, and other registers that the suffix needs.

### Several ranges, function hooks, and practical limits

Repeat the hook table for each range. One library can replace several ranges in
one function and ranges in many different functions. All use `block`; branch
on `range->address & 0x1fffffff` to select the appropriate implementation.

Partial ranges must not overlap, including ranges from different enabled mods.
A function hook entry cannot lie inside a partial range. A function hook at an
earlier entry may wrap a function containing partial hooks: its `next()` runs
the original prefix, partial callbacks, and original suffix normally.

Select a range with clear entry and exit behavior. An external branch into its
middle does not trigger the callback at its start; those original instructions
can still execute. If replacing branching code, account for all paths, memory
effects, and delay-slot instructions. A normally returning callback takes the
declared continuation.

Do not start a range at a branch delay slot or an instruction executing in a
load delay slot. The runtime keeps the original instruction in those cases to
preserve MIPS pipeline behavior. Addresses physically following a branch,
jump, or load are conservatively excluded even if another edge reaches them directly.
Move the start to a safe instruction boundary. The native CPU view does not
expose direct COP0/GTE register editing. Keep guest instructions that perform
those effects, or call a verified guest routine that implements them.

The runtime interprets resident game code whose compiled instruction footprint
intersects a partial range. This prevents compiled or inlined instructions from
silently bypassing the replacement; unrelated resident functions remain
compiled. While partial hooks are active, overlay code uses the interpreter too,
because linked overlay bodies can call other bodies directly within a module.
This can reduce performance. Disabling the partial hooks restores normal
compiled execution. No guest instruction bytes are patched for this mechanism.

## Hook several functions or combine mods

Repeat `--hook ADDRESS` for each selected function. The helper adds one
`[[native_module.hook]]` table for each. There is no fixed hook-count limit: a library may handle hundreds of functions in one `.psxmod`.
Normal archive size and game RAM limits still apply. All function hooks use the
same callback; inspect `call->address` to choose a
behavior. That is the actual dispatch address, so normalize aliases before
comparing addresses:

```c
uint32_t physical = call->address & 0x1fffffff;
```

For example, a module with two hook entries has this shape:

```toml
[[native_module.hook]]
address = 0x80019524
expected = "PASTE_THE_FIRST_FUNCTION_GUARD_HERE"

[[native_module.hook]]
address = 0x8004b740
expected = "PASTE_THE_SECOND_FUNCTION_GUARD_HERE"
```

Put these under one module declaration, replacing its earlier single-hook table.
The second address illustrates another entry in the USA executable; study and
verify it before using it. These addresses do not identify two reward functions.
The logging callback can handle both without changes. For different behavior,
branch on the normalized address and always continue any call you intend to
leave unchanged.

The example packaging helper also accepts repeated entries:

```sh
python3 examples/native-hook/package.py build/hook.so many-hooks.psxmod \
    --exe game/slus_006.64 \
    --hook 0x80019524 \
    --hook 0x8004b740
```

Add `--block ADDRESS:RESUME` for each partial range. The helper generates all
guards from the supplied images and always uses format 9. You can mix both
kinds of hook. The repository example's block callback writes its
`return_value` setting to `v0`, so use it for a range where that is the intended
effect.

Explicit bytes remain an advanced alternative: `--hook ADDRESS:HEX_BYTES`,
`--block ADDRESS:RESUME:HEX_BYTES`, or `--address` with `--expected`. Use this
when you already have a verified guard. The usual workflow does not need it.

Different modules can hook the same address. They chain in resolved package and
dependency order, then module declaration order:

```text
Caller -> mod A -> mod B -> original -> mod B -> mod A -> caller
```

`next()` advances to the next hook. A replacement that omits it also skips all
later hooks and the original. There is no native-module `order` field.

Hooks at the same physical address need compatible guards. Contradictory guards
fail plan resolution. An enabled executable patch that changes a guarded byte
to a different value also conflicts with the hook. Module order, hashes,
platform, hooks, and options contribute to the resolved mod plan fingerprint.

## Distribute one package for several platforms

Native libraries target an OS and CPU. The author builds each supported variant;
the player installs a prebuilt package. One `.psxmod` can contain all variants,
and the runtime selects the matching one.

| OS | Platform names | Usual library suffix |
|---|---|---|
| Linux | `linux-x86_64`, `linux-aarch64` | `.so` |
| Windows | `windows-x86_64`, `windows-aarch64` | `.dll` |
| macOS | `macos-x86_64`, `macos-aarch64` | `.dylib` |

Repeat `[[native_module]]` with the same feature and module ID, but different
platforms, paths, and hashes. Each variant needs its own nested hook tables.
For example, add this Windows variant to the custom manifest:

```toml
[[native_module]]
id = "main"
feature = "hook"
platform = "windows-x86_64"
file = "native/windows-x86_64/my_native_hook.dll"
sha256 = "PASTE_THE_WINDOWS_LIBRARY_SHA256_HERE"

[[native_module.hook]]
address = 0x80019524
expected = "PASTE_THE_ORIGINAL_INSTRUCTION_BYTES_HERE"
```

Keep guards and conditions consistent for the same intended behavior. A package
supports up to 64 native module variants. An enabled module with no matching
platform variant stops launch with a diagnostic.

Link dependencies statically except for system libraries. Libraries load from
an authenticated snapshot, not the mutable installed file. Windows resolves
imports through System32; POSIX uses eager loading with local symbols. There is
no package-specific dependency search path, and extra game C++ symbols are not
part of the supported API. Shipping a dependency beside the library does not
make it automatically loadable.

## API reference and callback rules

| API type | Purpose |
|---|---|
| `PSXNativeMod` | Versioned descriptor, your `userdata`, and callback pointers. |
| `PSXNativeCPU` | Editable guest general registers, HI, LO, and next PC. |
| `PSXNativeCall` | Struct size, dispatch address, return address, opaque context, and `next()`. |
| `PSXNativeBlock` | Struct size, range address, resume address, byte count, current guest `ra`, and callback context. |
| `PSXNativeGuestResult` | Struct size and the called function's `v0`/`v1` results. |
| `PSXNativeHost` | Struct size, ABI version, opaque context, package/feature IDs, and host services. |

| Callback | When it runs |
|---|---|
| `start(userdata, host)` | While preparing an enabled library. Return nonzero on success. |
| `stop(userdata)` | Before unloading, including cleanup after a failed `start`. |
| `hook(userdata, cpu, call)` | At a selected dispatch entry when its live-byte guard matches. |
| `block(userdata, cpu, range)` | Instead of a selected instruction range when its entire guard matches. |
| `vblank(userdata)` | At guest VBlank after game start. Optional. |
| `savestate_loaded(userdata)` | After restoring guest state and reapplying declarative executable patches. Optional. |

A module with function hooks must provide `hook`; one with partial hooks must
provide `block`. Other callbacks may be `NULL`. `block` is appended to the v1
descriptor, so existing v1 libraries with the original descriptor still load
for function hooks. New builds should initialize its last field, using `NULL`
if they do not need partial hooks.
The runtime rejects incompatible ABI versions, short descriptors, missing entry
points, and missing required hook callbacks.

| Host service | Purpose |
|---|---|
| `read_byte`, `read_half`, `read_word` | Read 8, 16, or 32 bits from guest memory. |
| `write_byte`, `write_half`, `write_word` | Write 8, 16, or 32 bits of guest data. |
| `option(context, id, buffer, capacity)` | Copy a committed option from this feature as text. |
| `log(context, message)` | Write a message tagged with the package and module. |
| `advance_cycles(cycles)` | Advance the authoritative guest clock for replacement work. |
| `game_started()` | Report whether the runtime has entered the game. |
| `call_guest(context, cpu, address, arguments, count, result)` | Synchronously call another guest function from a hook; optional appended ABI v1 service. |
| `find_function(host_context, name, address)` | Resolve function metadata by name or hexadecimal string, optionally with a name offset; optional appended ABI v1 service. |
| `call_guest_named(context, cpu, name, arguments, count, result)` | Resolve a function, verify live code identity, then call it; optional appended ABI v1 service. |

Pass `host->context` to host services that need a context, and `call->context`
to `next()`. These are internal handles: pass them back unchanged, without
reading or editing their contents. `call_guest()` and `call_guest_named()` take the callback context,
not `host->context`. The appended `PSXNativeBlock.context` also requires a
struct-size check when using it with an older runtime.

Initialize host resources and read settings in `start`. Guest gameplay memory
is not ready at that point. The host table lives until `stop`; use its services
only from callbacks on the emulation thread.

Do not retain `cpu`, `call`, or `next` for later use. Do not throw C++ exceptions
or use `longjmp` across the API boundary. Background threads must not use guest
services. Keep callbacks short enough to preserve game responsiveness.

An unchanged configuration keeps its library and state across disc swaps.
After a plan change or reset, new dispatches use the new configuration. Old
libraries with pending callbacks stay loaded until those callbacks return. A
nonlocal guest exit may keep a retired library mapped until process exit.
Native mods are excluded from netplay.

## Troubleshoot and test before release

| Symptom | Check |
|---|---|
| Installed, but no hook logs | Enable the feature. Check the dispatch boundary and live guard. |
| Startup log appears, but hook log does not | `start` ran, but the hook has not fired. Check the address and current overlay. |
| Code after `next()` never runs | The original may run for the whole session, or guest flow may exit nonlocally. |
| Native content changed or is untrusted | Publish a new version, or remove the edited version and reinstall through the launcher. |
| Checksum failure | Hash the final rebuilt library after copying it into the package. |
| Module unavailable for this platform | Supply a binary and declaration for the game's OS/CPU. |
| Cannot load library | Check architecture and runtime dependencies. Prefer static non-system dependencies. |
| Missing `psx_native_mod_v1` | Check C linkage, export visibility, and the exact entry point name. |
| Incompatible ABI/descriptor | Use the SDK ABI constant and `sizeof(PSXNativeMod)`; provide required callbacks. |
| Startup rejected | `start` returned zero. Log the reason before returning. |
| Guard conflicts with another mod | Compare hook guards and executable patches at that physical address. |
| Crash or wrong result after returning | Check register meanings, stack discipline, guest pointers, side effects, and timing. |

Before publishing, test disabled behavior, logging-only behavior, every option,
replacement behavior, and combinations with other mods. Test save state restore,
disc swaps where relevant, wrong guards, every distributed platform variant,
and uninstall/reinstall. Check that declining trust leaves the package
uninstalled.
