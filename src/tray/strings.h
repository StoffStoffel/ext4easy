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
};

// Text in the user's UI language (German or English).
const wchar_t* tr(Str id);

}  // namespace ext4easy
