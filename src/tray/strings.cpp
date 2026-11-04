// SPDX-License-Identifier: GPL-3.0-or-later
#include "tray/strings.h"

#include <windows.h>

namespace ext4easy {

namespace {

struct Entry {
    Str id;
    const wchar_t* en;
    const wchar_t* de;
};

const Entry kStrings[] = {
    {Str::TipMounted, L"ext4easy - %d Linux drive(s) mounted", L"ext4easy - %d Linux-Laufwerk(e) eingebunden"},
    {Str::TipNoService, L"ext4easy - service is not running", L"ext4easy - Dienst läuft nicht"},
    {Str::MenuNoVolumes, L"No Linux drives found", L"Keine Linux-Laufwerke gefunden"},
    {Str::MenuServiceDown, L"Service not reachable", L"Dienst nicht erreichbar"},
    {Str::MenuOpen, L"&Open", L"Ö&ffnen"},
    {Str::MenuMount, L"&Mount", L"&Einbinden"},
    {Str::MenuUnmount, L"&Unmount", L"&Trennen"},
    {Str::MenuDetach, L"&Close image", L"Image &schließen"},
    {Str::MenuPolicyAsk, L"Ask when connected", L"Beim Anschließen fragen"},
    {Str::MenuPolicyAlways, L"Always mount automatically", L"Immer automatisch einbinden"},
    {Str::MenuPolicyNever, L"Never mount", L"Nie einbinden"},
    {Str::MenuAttachImage, L"Mount &image file...", L"&Image-Datei einbinden..."},
    {Str::MenuExit, L"E&xit", L"&Beenden"},
    {Str::StateUnsupported, L"not supported", L"nicht unterstützt"},
    {Str::StateError, L"error", L"Fehler"},
    {Str::AskTitle, L"Linux drive detected", L"Linux-Laufwerk erkannt"},
    {Str::AskMain, L"Mount “%s”?", L"„%s“ einbinden?"},
    {Str::AskContent, L"%s on %s, partition %d.\n%s", L"%s auf %s, Partition %d.\n%s"},
    {Str::AskMount, L"&Mount", L"&Einbinden"},
    {Str::AskSkip, L"&Don't mount", L"&Nicht einbinden"},
    {Str::AskRemember, L"Remember for this drive", L"Für dieses Laufwerk merken"},
    {Str::BalloonMounted, L"“%s” is now available as %s (%s).",
     L"„%s“ ist jetzt als %s verfügbar (%s)."},
    {Str::BalloonFailed, L"“%s” could not be mounted.", L"„%s“ konnte nicht eingebunden werden."},
    {Str::UnnamedVolume, L"Linux drive", L"Linux-Laufwerk"},
    {Str::ImageFilter, L"Disk images (*.img;*.raw;*.bin)|*.img;*.raw;*.bin|All files|*.*|",
     L"Disk-Images (*.img;*.raw;*.bin)|*.img;*.raw;*.bin|Alle Dateien|*.*|"},
    {Str::OpenImageTitle, L"Open image file with a Linux filesystem",
     L"Image-Datei mit Linux-Dateisystem öffnen"},
    {Str::ImageAttached, L"%d Linux volume(s) mounted from the image.",
     L"%d Linux-Laufwerk(e) aus dem Image eingebunden."},
    {Str::MenuAbout, L"&About...", L"&Info..."},
    {Str::AboutTitle, L"About ext4easy", L"Info über ext4easy"},
    {Str::AboutMain, L"ext4easy", L"ext4easy"},
    // The WinFsp notice is required verbatim by the WinFsp FLOSS license
    // exception (with a link to its repository) - keep it in English.
    {Str::AboutContent,
     L"Version %s\n"
     L"Linux drives (ext2/ext3/ext4) on Windows.\n"
     L"License: GNU General Public License v3 or later.\n\n"
     L"Uses:\n"
     L"WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos\n"
     L"<a href=\"https://github.com/winfsp/winfsp\">https://github.com/winfsp/winfsp</a>\n\n"
     L"lwext4, Copyright (C) Grzegorz Kostka and contributors\n"
     L"<a href=\"https://github.com/gkostka/lwext4\">https://github.com/gkostka/lwext4</a>",
     L"Version %s\n"
     L"Linux-Laufwerke (ext2/ext3/ext4) unter Windows.\n"
     L"Lizenz: GNU General Public License v3 oder neuer.\n\n"
     L"Verwendet:\n"
     L"WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos\n"
     L"<a href=\"https://github.com/winfsp/winfsp\">https://github.com/winfsp/winfsp</a>\n\n"
     L"lwext4, Copyright (C) Grzegorz Kostka und Mitwirkende\n"
     L"<a href=\"https://github.com/gkostka/lwext4\">https://github.com/gkostka/lwext4</a>"},
    {Str::AboutNotices, L"<a href=\"notices\">Show all license notices</a>",
     L"<a href=\"notices\">Alle Lizenzhinweise anzeigen</a>"},
    {Str::MenuAllowWrite, L"Allow &write access", L"&Schreibzugriff erlauben"},
    {Str::AccessReadOnly, L"read-only", L"nur lesen"},
    {Str::AccessWritable, L"read/write", L"lesen und schreiben"},
    {Str::AskReadOnly, L"The drive is mounted read-only.",
     L"Das Laufwerk wird schreibgeschützt eingebunden."},
    {Str::AskWritable,
     L"The drive is mounted with write access. Unmount it here before unplugging it.",
     L"Das Laufwerk wird mit Schreibzugriff eingebunden. Trennen Sie es hier, bevor Sie es "
     L"abziehen."},
    {Str::ReadOnlyBecause, L"Only readable: %s", L"Nur lesbar: %s"},
    {Str::WriteConfirmTitle, L"Write access", L"Schreibzugriff"},
    {Str::WriteConfirmMain, L"Allow write access to “%s”?", L"Schreibzugriff auf „%s“ erlauben?"},
    {Str::WriteConfirmContent,
     L"Windows programs can then create, change and delete files on this Linux drive.\n\n"
     L"Before unplugging the drive, choose “Unmount” in this menu so that all data is "
     L"written.\n\n"
     L"Linux permissions are not checked on Windows. New files get the owner of their folder.",
     L"Windows-Programme können dann Dateien auf diesem Linux-Laufwerk anlegen, ändern und "
     L"löschen.\n\n"
     L"Wählen Sie vor dem Abziehen in diesem Menü „Trennen“, damit alle Daten geschrieben "
     L"werden.\n\n"
     L"Linux-Berechtigungen werden unter Windows nicht geprüft. Neue Dateien erhalten den "
     L"Besitzer ihres Ordners."},
    {Str::WriteConfirmYes, L"&Allow write access", L"Schreibzugriff &erlauben"},
    {Str::WriteConfirmNo, L"&Cancel", L"&Abbrechen"},
    {Str::BalloonUnmountedSafe, L"“%s” was unmounted, all data is written. You can unplug it now.",
     L"„%s“ wurde getrennt, alle Daten sind geschrieben. Sie können es jetzt abziehen."},
};

bool useGerman() {
    static const bool german = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    return german;
}

}  // namespace

const wchar_t* tr(Str id) {
    for (const auto& e : kStrings)
        if (e.id == id) return useGerman() ? e.de : e.en;
    return L"?";
}

}  // namespace ext4easy
