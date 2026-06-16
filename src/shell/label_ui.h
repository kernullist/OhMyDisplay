// OhMyDisplay - monitor label editor (FR-A7). Built from dynamic Win32 controls so
// it needs no dialog resource template.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace omd
{
    struct LabelRow
    {
        std::string id;           // monitor identity
        std::string friendlyName; // CCD friendly name shown to the user
        std::string current;      // existing label (prefilled)
        std::string edited;       // filled on Save
        bool collision = false;   // part of a same-model collision group
    };

    // Show the modal label editor. On Save returns true and fills row.edited for each
    // row. On Cancel/close returns false.
    bool ShowLabelEditor(HWND owner, HINSTANCE hInst, std::vector<LabelRow>& rows);
}
