#ifndef GEARSF7000_NFD_PARENTED_H
#define GEARSF7000_NFD_PARENTED_H

#include "imgui/imgui.h"
#include "nfd/nfd.h"

inline nfdwindowhandle_t NFD_ParentWindow()
{
    nfdwindowhandle_t parent{};
#if defined(_WIN32) && !defined(__WINRT__)
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport && viewport->PlatformHandleRaw)
    {
        parent.type = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
        parent.handle = viewport->PlatformHandleRaw;
    }
#endif
    return parent;
}

inline nfdresult_t NFD_OpenDialogParented(nfdchar_t** outPath,
                                           const nfdfilteritem_t* filterList,
                                           nfdfiltersize_t filterCount,
                                           const nfdchar_t* defaultPath)
{
#ifdef NFD_NATIVE
    nfdopendialognargs_t args{filterList, filterCount, defaultPath, NFD_ParentWindow()};
    return NFD_OpenDialogN_With(outPath, &args);
#else
    nfdopendialogu8args_t args{filterList, filterCount, defaultPath, NFD_ParentWindow()};
    return NFD_OpenDialogU8_With(outPath, &args);
#endif
}

inline nfdresult_t NFD_SaveDialogParented(nfdchar_t** outPath,
                                           const nfdfilteritem_t* filterList,
                                           nfdfiltersize_t filterCount,
                                           const nfdchar_t* defaultPath,
                                           const nfdchar_t* defaultName)
{
#ifdef NFD_NATIVE
    nfdsavedialognargs_t args{filterList, filterCount, defaultPath, defaultName,
                              NFD_ParentWindow()};
    return NFD_SaveDialogN_With(outPath, &args);
#else
    nfdsavedialogu8args_t args{filterList, filterCount, defaultPath, defaultName,
                               NFD_ParentWindow()};
    return NFD_SaveDialogU8_With(outPath, &args);
#endif
}

inline nfdresult_t NFD_PickFolderParented(nfdchar_t** outPath,
                                           const nfdchar_t* defaultPath)
{
#ifdef NFD_NATIVE
    nfdpickfoldernargs_t args{defaultPath, NFD_ParentWindow()};
    return NFD_PickFolderN_With(outPath, &args);
#else
    nfdpickfolderu8args_t args{defaultPath, NFD_ParentWindow()};
    return NFD_PickFolderU8_With(outPath, &args);
#endif
}

#endif
