# Third-party notices

ext4win is licensed under the GNU General Public License v3.0 or later (see `LICENSE`).
It contains or ships the following third-party software.

---

## WinFsp

**WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos**
<https://github.com/winfsp/winfsp>

Used to provide the drive letters. ext4win links to `winfsp-x64.dll`, and the setup
(`ext4win-setup-<version>.exe`) contains the unmodified WinFsp installer as released by
the WinFsp project. WinFsp is licensed under the GPLv3 with the following special
exception for free/open source software, under which ext4win uses it:

> As a special exception to GPLv3, Bill Zissimopoulos grants additional permissions to
> Free/Libre and Open Source Software ("FLOSS") without requiring that such software is
> covered by the GPLv3.
>
> 1. Permission to link with a platform specific version of the WinFsp DLL (one of:
>    winfsp-a64.dll, winfsp-x64.dll, winfsp-x86.dll, winfsp-msil.dll).
> 2. Permission to distribute unmodified binary releases of the WinFsp installer (as
>    released by the WinFsp project).
>
> These permissions (and no other) are granted provided that the software:
>
> 1. Is distributed under a license that satisfies the Free Software Definition
>    Version 1.141 (https://www.gnu.org/philosophy/free-sw.en.html) or the Open Source
>    Definition Version 1.9 (https://opensource.org/osd).
> 2. Includes the copyright notice "WinFsp - Windows File System Proxy, Copyright (C)
>    Bill Zissimopoulos" and a link to the WinFsp repository in its user-interface and
>    any user-facing documentation.
> 3. Is not linked or distributed with proprietary (non-FLOSS) software. [You cannot mix
>    FLOSS and proprietary software while using WinFsp under this special exception.]

The full license is installed with WinFsp (`License.txt` in its installation folder).

---

## lwext4

<https://github.com/gkostka/lwext4> — vendored in `third_party/lwext4` with local changes
(listed in `third_party/lwext4/PATCHES.md`).

Most files are licensed under the BSD 3-Clause license below. `ext4_extent.c` and
`ext4_xattr.c` are licensed under the GNU General Public License, version 2 or (at your
option) any later version.

Copyright (c) 2013, 2014, 2015, 2017 Grzegorz Kostka (kostka.grzegorz@gmail.com)<br>
Copyright (c) 2015, 2017 Kaho Ng (ngkaho1234@gmail.com)<br>
Copyright (c) 2012 Martin Sucha<br>
Copyright (c) 2012 Frantisek Princ<br>
All rights reserved.

> Redistribution and use in source and binary forms, with or without modification, are
> permitted provided that the following conditions are met:
>
> - Redistributions of source code must retain the above copyright notice, this list of
>   conditions and the following disclaimer.
> - Redistributions in binary form must reproduce the above copyright notice, this list
>   of conditions and the following disclaimer in the documentation and/or other
>   materials provided with the distribution.
> - The name of the author may not be used to endorse or promote products derived from
>   this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
> WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
> AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
> LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
> THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
> NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
> ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### Directory hash functions (lwext4 `ext4_hash.c`, from FreeBSD)

Copyright (c) 2010, 2013 Zheng Liu <lz@freebsd.org><br>
Copyright (c) 2012, Vyacheslav Matyushin<br>
All rights reserved.

> Redistribution and use in source and binary forms, with or without modification, are
> permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice, this list of
>    conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice, this list
>    of conditions and the following disclaimer in the documentation and/or other
>    materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR
> IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
> THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
> EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
> STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
> OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The half-MD4 directory hash in that file is derived from the RSA Data Security, Inc. MD4
Message-Digest Algorithm:

> Copyright (C) 1990-2, RSA Data Security, Inc. All rights reserved.
>
> License to copy and use this software is granted provided that it is identified as the
> "RSA Data Security, Inc. MD4 Message-Digest Algorithm" in all material mentioning or
> referencing this software or this function.
>
> License is also granted to make and use derivative works provided that such works are
> identified as "derived from the RSA Data Security, Inc. MD4 Message-Digest Algorithm"
> in all material mentioning or referencing the derived work.
>
> RSA Data Security, Inc. makes no representations concerning either the
> merchantability of this software or the suitability of this software for any
> particular purpose. It is provided "as is" without express or implied warranty of any
> kind.
>
> These notices must be retained in any copies of any part of this documentation and/or
> software.

---

## WiX Toolset

<https://wixtoolset.org> — the setup program (`ext4win-setup-<version>.exe`) is built with
the WiX Toolset and contains its bootstrapper engine and standard bootstrapper
application, licensed under the Microsoft Reciprocal License (MS-RL),
<https://opensource.org/license/ms-rl-html>. Their source code is available at
<https://github.com/wixtoolset/wix>.
