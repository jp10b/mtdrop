# MT7961 firmware: source, licence, fetch procedure

mtdrop needs two MediaTek firmware images at run time. They are **never committed to this repository and never redistributed by it** (`CLAUDE.md` §9). This document says where to get them, under what licence, and how to fetch and check them.

Status legend: FACT = read from the cited source on 2026-11-11; TODO-VERIFY = not verified.

## 1. Files and upstream location

| Role | File (in linux-firmware) | Size at tag 20231211 | Size at tag 20261019 |
|---|---|---|---|
| ROM patch | `mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin` | 92192 B | 92192 B |
| RAM firmware | `mediatek/WIFI_RAM_CODE_MT7961_1.bin` | 797300 B | 792036 B |

Sizes: HTTP `Content-Length` of the URLs below (FACT).

Official repositories (the `README.md` of linux-firmware states: "The upstream repository is located at <https://gitlab.com/kernel-firmware/linux-firmware.git>"):

- GitLab (upstream): <https://gitlab.com/kernel-firmware/linux-firmware>
- git.kernel.org: <https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git> (also lists `kernel.googlesource.com` as a clone URL)
- GitHub mirror: TODO-VERIFY. `github.com/{kernel-firmware,linux-firmware,torvalds}/linux-firmware` returned 404; none is used here.

Raw download URL forms (all returned HTTP 200 for both files; FACT). `<TAG>` is a tag such as `20231211`; `<FILE>` is the file name without `mediatek/`.

| Pin | URL |
|---|---|
| Tag, git.kernel.org | `https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/<FILE>?h=<TAG>` |
| Commit, git.kernel.org | `https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/<FILE>?id=<COMMIT>` |
| Tag, GitLab | `https://gitlab.com/kernel-firmware/linux-firmware/-/raw/<TAG>/mediatek/<FILE>` |
| Main branch (do not pin to this) | `https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/mediatek/<FILE>` |

Note: the WebFetch tool received HTTP 403 from git.kernel.org while `curl` received 200. Automated clients may be throttled; keep GitLab as the fallback (TODO-VERIFY: rate limits).

## 2. Version history relevant to us

`WHENCE` (main, 2026-11-11) lists under `Driver: mt7921 - MediaTek MT7921 Wireless MACs`: `mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin` Version `20260224110909a`; `mediatek/WIFI_RAM_CODE_MT7961_1.bin` Version `20260224110949`. History of the RAM file (git.kernel.org Atom feed of the path; commit dates; FACT):

| Commit | Date | RAM build | Patch build | First tag containing it |
|---|---|---|---|---|
| `0a18a7292a66` | 2023-11-16 | 20231109190959 | 20231109190918a | `20231211` (tagged 2023-12-11; tag `20231111` is named before the commit date, its date not fetched) |
| `e83ad4497f19` | 2024-02-23 | 20240219111038 | 20240219110958a | not looked up |
| `ced02591a802` | 2026-03-13 | 20260224110949 | 20260224110909a | `20260410` (2026-04-10; `20260309` predates it) |

Between those rows there are further updates (2024-07, 2024-08, 2024-11, 2025-06, 2025-07, 2026-01); see the Atom feed. The commit message of `ced02591a802` names the patch file `..._1_1_hdr.bin` while `WHENCE` and the tree use `..._1_2_hdr.bin`; the file at that path is what we use.

**Important gap.** Project finding F-007 tested the linux-firmware copy with RAM build **20231109** (`____010000`, CNM flag set). By the history above that corresponds to tag `20231211` (inferred from commit dates; TODO-VERIFY with the tool in §4). The current upstream build (20260224110949) has **not** been tested by the project: whether it still reports `____010000` and the CNM flag, and whether it works on the chip, is TODO-VERIFY. Until then, pin to `20231211`.

## 3. Licence

`WHENCE` entries for both files say: "Licence: Redistributable. See LICENCE.mediatek for details." and "SPDX-License-Identifier: LicenseRef-LICENCE.mediatek". The text is `LICENSES/LICENCE.mediatek` in the repository (<https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/LICENSES/LICENCE.mediatek>). Full text, two paragraphs. Key sentences:

> "MediaTek Inc. grants permission to use and redistribute aforementioned firmware files for the use with devices containing MediaTek chipsets, but not as part of the Linux kernel or in any other form which would require these files themselves to be covered by the terms of the GNU General Public License or the GNU Lesser General Public License."
>
> "These firmware files are distributed in the hope that they will be useful, but are provided WITHOUT ANY WARRANTY ..."

| Question | Answer from the text |
|---|---|
| Redistribution | Permitted ("use and redistribute"), for use with devices containing MediaTek chipsets. |
| Binary form | The text does not distinguish binary from source; the files are binary-only. Redistribution of them as-is is permitted. |
| Conditions | Not under the GPL/LGPL: the files must not be placed in a form that would require them to be GPL/LGPL-covered. No requirement to ship the licence text, no attribution clause, no fee. |
| Modification | Not mentioned. TODO-VERIFY (treat as not granted; we do not modify the files). |
| Reverse engineering | Not mentioned (neither granted nor forbidden). TODO-VERIFY. |

linux-firmware's own policy (`LICENSE-CRITERIA.md`) says redistribution licences there must allow "unrestricted redistribution by everyone", but also that older licences "may not fully adhere to the requirements and will be considered an exception"; it states "Nothing in this document is legal advice".

**What this means for mtdrop (not legal advice):**

- (a) Installer downloads from the official source at install time: yes. The user's machine fetches the files from linux-firmware; mtdrop does not redistribute anything. This is the recommended and only supported path.
- (b) Bundling in a release archive: the grant appears to allow it, provided the files stay separate, unmodified artefacts that are not made part of a GPL-covered work (the GPL-3.0 mtdrop code and the firmware would be an aggregate, not a combined work). We would have to ship them as a separate directory, include `LICENCE.mediatek` and the `WHENCE` entry (not required by the text, but the safe practice), and state the origin tag. Ambiguities (no explicit modification/reverse-engineering terms, GPL interaction of the installer package) remain. Conservative choice: **do not bundle**; download at install time, verify the hash, and revisit only after review by someone qualified.

## 4. Fetch procedure

Rules: pin to a tag; HTTPS only; verify SHA-256 against the table below; store per machine outside the repository; abort on mismatch.

Target directory: `%ProgramData%\mtdrop\firmware\<tag>\`. Never under the repository (`.gitignore` is not a safeguard).

SHA-256 of the pinned files (fill in when a version is pinned and verified; do not use a file whose row is empty):

| Tag | File | SHA-256 |
|---|---|---|
| `20231211` | `WIFI_MT7961_patch_mcu_1_2_hdr.bin` | TODO-FILL |
| `20231211` | `WIFI_RAM_CODE_MT7961_1.bin` | TODO-FILL |

Developer commands (PowerShell, no elevation needed if the directory is yours; `%ProgramData%` writes may need an elevated shell):

```powershell
$Tag  = '20231211'
$Dir  = Join-Path $env:ProgramData "mtdrop\firmware\$Tag"
$Base = "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$Tag/mediatek"
New-Item -ItemType Directory -Force $Dir | Out-Null
foreach ($f in 'WIFI_MT7961_patch_mcu_1_2_hdr.bin','WIFI_RAM_CODE_MT7961_1.bin') {
    Invoke-WebRequest -UseBasicParsing -Uri "$Base/$f" -OutFile (Join-Path $Dir $f)
    # or: curl.exe -fL --proto '=https' -o (Join-Path $Dir $f) "$Base/$f"
    (Get-FileHash (Join-Path $Dir $f) -Algorithm SHA256) | Format-List Hash, Path
}
```

Compare each printed `Hash` with the table (case-insensitive). If a hash differs, or the table row is still `TODO-FILL`, delete the file and stop. Fallback host: replace `$Base` with `https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek` and append `?h=$Tag` to each file URL.

## 5. Check that a file is the right kind

Build `driver/mt792x` (see its README) and run the test executable with the directory set:

```powershell
$env:MT792X_FW_DIR = Join-Path $env:ProgramData "mtdrop\firmware\$Tag"
.\build\mt792x\mt792x_tests.exe real_firmware   # path differs with multi-config generators, e.g. ...\Release\
```

It prints patch and RAM headers. Look at the RAM line `fw_ver '...'` and the line `release-info feature byte 0x.. : CNM yes/no`:

| | Works for mtdrop | Does not work |
|---|---|---|
| RAM `fw_ver` | `____010000` | `____000000` |
| CNM flag | yes (feature byte `0xc0` in F-007) | no (`0x00`) |

Project finding F-007 observed both facts together; the cause is an ASSUMPTION (different firmware branch without the unified command interface). The tool does not check hashes: do §4 first.

## 6. Do not use the Windows vendor package files

The MediaTek Windows driver package (3.5.0.1349 on the maintainer's machine) contains files with the same names. Project finding F-007: its RAM image (`____000000`, build 20250922, no CNM flag) does not answer the UNI commands the mtdrop driver sends, so monitor reception never starts. Download and boot of that firmware succeed (F-006), which makes the failure silent. Never copy files from `C:\Windows\System32\DriverStore` or the vendor package into the firmware directory. Their redistribution terms are also separate and not reviewed here (TODO-VERIFY).

## Open questions

- TODO-VERIFY: the current upstream build (RAM 20260224110949, tag `20260410` or later) still reports `____010000` + CNM and works on the chip.
- TODO-VERIFY: tag `20231211` really contains RAM build 20231109190959 (check with the tool; the inference is from commit dates).
- TODO-VERIFY: tag following `e83ad4497f19` (2024-02-23) and the intermediate builds; none were tested.
- TODO-VERIFY: GitHub mirror existence; git.kernel.org / GitLab rate limits for automated installers.
- TODO-VERIFY: modification and reverse-engineering terms (the licence is silent); legal review before any bundling.
- TODO-FILL: SHA-256 of the two pinned files.
- TODO-VERIFY: exact path of the Windows build of `mt792x_tests` with the project's CMake generator.
- TODO-VERIFY: redistribution terms of the MediaTek Windows vendor package (irrelevant while we do not use it).
