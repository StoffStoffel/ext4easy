// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ext4easy {

enum class Str {
    TipMounted,        // %d = number of mounted volumes
    TipNoService,
    MenuNoVolumes,
    MenuServiceDown,
    MenuOpen,
    MenuMount,
    MenuUnmount,
    MenuDetach,
    MenuPolicyAsk,
    MenuPolicyAlways,
    MenuPolicyNever,
    MenuAttachImage,
    MenuExit,
    StateUnsupported,
    StateError,
    AskTitle,
    AskMain,           // %s = name
    AskContent,        // %s size, %s disk, %d partition
    AskMount,
    AskSkip,
    AskRemember,
    BalloonMounted,    // %s name, %s drive
    BalloonFailed,     // %s name
    UnnamedVolume,
    ImageFilter,       // pairs separated by '|'
    OpenImageTitle,
    ImageAttached,     // %d volumes
    MenuAbout,
    AboutTitle,
    AboutMain,
    AboutContent,      // %s version; may contain <a href> links
    AboutNotices,      // footer link text
    MenuAllowWrite,
    AccessReadOnly,    // short: "read-only"
    AccessWritable,    // short: "read/write"
    AskReadOnly,       // sentence in the mount question
    AskWritable,
    ReadOnlyBecause,   // %s = reason (English text from the core)
    WriteConfirmTitle,
    WriteConfirmMain,  // %s name
    WriteConfirmContent,
    WriteConfirmYes,
    WriteConfirmNo,
    BalloonUnmountedSafe,  // %s name
};

// Text in the user's UI language (German or English).
const wchar_t* tr(Str id);

}  // namespace ext4easy
