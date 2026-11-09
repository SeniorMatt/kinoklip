# Licence texts for bundled packages

`scripts/package_linux.sh` uses these when an Arch package has no licence files and its metadata
uses old licence names instead of SPDX ids. Each folder holds the licence the upstream project states:

- e2fsprogs: upstream NOTICE, plus the MIT-style header of lib/et/com_err.c (Kinoklip bundles libcom_err)
- libasyncns: LGPL-2.1-or-later
- libidn2: the library is LGPL-3.0-or-later or GPL-2.0-or-later
- opencore-amr: Apache-2.0
- 0BSD.txt: SPDX text for xz, which Arch's spdx folder lacks
